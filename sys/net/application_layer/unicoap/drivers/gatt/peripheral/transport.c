#include "host/ble_uuid.h"

#include <assert.h>
#include <services/gap/ble_svc_gap.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"
#include "net/unicoap/transport.h"
#include <stdio.h>
#include <string.h>

#define ENABLE_DEBUG CONFIG_UNICOAP_DEBUG_LOGGING
#define DEBUG_PREFIX " "
#include "debug.h"
#include "private.h"

#include "nimble_riot.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "net/bluetil/ad.h"

#include "os/os_mbuf.h"

#include "private/drivers/gatt.h"

#define _GATT_PERIPHERAL_DEBUG(...) _UNICOAP_PREFIX_DEBUG(".transport.gatt.peripheral", __VA_ARGS__)

static void _setup_gatt(void);

// todo: make configurable
static const char *device_name = "CoAP over GATT";
static const char *device_name_short = "CoAP";

static uint16_t _upstream_val_handle;
static uint16_t _downstream_val_handle;

/**
 * Callback that is fired after GAP events. We currently process the following:
 * - succesfull connect event (fired after advertising and connection process)
 * - unsuccesfull connect event (fired after advertising and connection process,
 *   if connection establishment with peer failed)
 * - disconnect event
 */
static int _gap_event_cb(struct ble_gap_event *event, void *arg)
{
    unicoap_gatt_ctx_t *ctx = arg;
    assert(ctx);

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            _GATT_PERIPHERAL_DEBUG("Connecting to central was unsuccessful\n");
            // Start advertising again -- the connection was unsuccessful
            unicoap_transport_start_gatt_peripheral(ctx);
            return 0;
        }
        assert(!ctx->transport.nimble.state.connected);
        ctx->transport.nimble.state.connected = true;
        ctx->transport.nimble.connection_handle = event->connect.conn_handle;
        ctx->transport.nimble.downstream_handle = _downstream_val_handle;
        ctx->transport.nimble.upstream_handle = _upstream_val_handle;

        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->connect.conn_handle, &desc);
        ctx->transport.nimble.addr = desc.peer_id_addr;

        _GATT_PERIPHERAL_DEBUG("connected to central\n");
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        _GATT_PERIPHERAL_DEBUG("disconnect event\n");
        unicoap_gatt_notify_disconnect(ctx);

        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        if (event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_TERM) {
            // todo: or rather notify disconnect directly as subscription is prerequisite for sending
            return 0;
        }
        _GATT_PERIPHERAL_DEBUG("SUBSCRIBE event with reason %d\n", event->subscribe.reason);
        assert(!ctx->transport.nimble.state.upstream_subscribed);
        ctx->transport.nimble.state.upstream_subscribed = true;

        unicoap_gatt_notify_ready(ctx);
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_TX: {
        _GATT_PERIPHERAL_DEBUG("NOTIFY_TX event with status %d\n", event->notify_tx.status);
        return 0;
    }

    default:
        _GATT_PERIPHERAL_DEBUG("Unhandled GAP event: %d\n", event->type);
    }

    return 0;
}

/**
 * Callback that is fired when the upstream characteristic is accessed.
 * Since we use this characteristics only ourself to send data to the peer
 * in form of indicate/notify, we don't do anything here.
 */
static int _upstream_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    (void)ctxt;

    _GATT_PERIPHERAL_DEBUG("unhandled operation!\n");

    return 1;
}

/**
 * Callback that is fired when the downstream characteristic is accessed.
 * This happens when a peer writes on it.
 */
static int _downstream_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    unicoap_gatt_ctx_t *ctx = _gatt_ctx_find_by_conn(conn_handle);
    assert(ctx);
    assert(ctx->transport.nimble.connection_handle == conn_handle);
    assert(ctx->transport.nimble.downstream_handle == attr_handle);

    int res = 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        unicoap_gatt_on_rx(ctx, ctxt->om);
    }
    else {
        _GATT_PERIPHERAL_DEBUG("unhandled operation on characteristic (only supports write)!\n");
        res = 1;
    }

    return res;
}

/**
 * Define coap over gatt bluetooth service and characteristics for our device
 */
static struct ble_gatt_svc_def _gatt_svr_svcs[] = {
    {
        /* Service: CoAP demo */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&unicoap_gatt_uuid_service.u,
        .characteristics =
            // todo: currently only one pair of characteristics supported
            (struct ble_gatt_chr_def[]){
                /* Downstream characteristc (receive data from peer) */
                {
                    .uuid = (ble_uuid_t *)&unicoap_gatt_uuid_downstream.u,
                    .access_cb = _downstream_cb,
                    .arg = NULL,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_RELIABLE_WRITE,
                    .val_handle = &_downstream_val_handle,
                },
                /* Upstream characteristic (send data to peer) */
                {
                    .uuid = (ble_uuid_t *)&unicoap_gatt_uuid_upstream.u,
                    .access_cb = _upstream_cb,
                    .arg = NULL,
                    .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
                    .val_handle = &_upstream_val_handle,
                },
                {
                    0, /* No more characteristics in this service */
                },
            },
    },
    {
        0, /* No more services */
    },
};



/**
* Setup GATT table
*/
static inline void _setup_gatt(void)
{
    static bool _setup = false;

    if (_setup) {
        return;
    }

    int res;

    res = ble_gatts_count_cfg(_gatt_svr_svcs);
    assert(res == 0);
    res = ble_gatts_add_svcs(_gatt_svr_svcs);
    assert(res == 0);

    res = ble_svc_gap_device_name_set(device_name);
    if (res != 0) {
        _GATT_PERIPHERAL_DEBUG("error setting device name; res=%x\n", res);
        return;
    }
    res = ble_gatts_start();
    if (res != 0) {
        _GATT_PERIPHERAL_DEBUG("error starting GATT server; res=%x\n", res);
        return;
    }

    /* configure and set the advertising data */
    uint8_t buf[BLE_HS_ADV_MAX_SZ];
    bluetil_ad_t ad;
    res = bluetil_ad_init_with_flags(&ad, buf, sizeof(buf), BLUETIL_AD_FLAGS_DEFAULT);
    res = bluetil_ad_add(&ad, BLE_GAP_AD_NAME_SHORT, device_name_short, strlen(device_name_short));
    res = bluetil_ad_add(&ad, BLE_GAP_AD_UUID128_COMP, &unicoap_gatt_uuid_service.value, sizeof(unicoap_gatt_uuid_service.value));
    res = ble_gap_adv_set_data(ad.buf, ad.pos);
    if (res != 0) {
        _GATT_PERIPHERAL_DEBUG("error setting advertisement data; res=%x\n", res);
        return;
    }

    _setup = true;
}


/**
 * Start advertising GATT service and characteristics
 */
int unicoap_transport_start_gatt_peripheral(unicoap_gatt_ctx_t *ctx)
{
    _setup_gatt();

    int res;
    struct ble_gap_adv_params advp = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    _GATT_PERIPHERAL_DEBUG("start advertising\n");

    res = ble_gap_adv_start(nimble_riot_own_addr_type, NULL, BLE_HS_FOREVER, &advp, _gap_event_cb, ctx);
    if (res != 0) {
        _GATT_PERIPHERAL_DEBUG("error starting GAP advertisement procedure; res=%x\n", res);
    }
    return res;
}

/**
  * Send message using BLE (GATT).
  *
  * @param pdu         data to send
  * @param pdu_size    size of the data
  * @param conn_handle handle over which connection the data should be send (nimble handle)
  *
  * @return            0 on success, != 0 on error
  */
int unicoap_transport_sendv_gatt_peripheral(iolist_t* iolist, const unicoap_gatt_ctx_t *ctx, bool reliable)
{
    assert(iolist);
    assert(ctx);
    assert(ctx->is_used);

    uint16_t len = iolist_size(iolist);
    if (ctx->transport.nimble.att_mtu - 3 < len) {
        _GATT_PERIPHERAL_DEBUG(
            "error: ATT MTU too small " _UNICOAP_NEED_HAVE "\n",
            len, ctx->transport.nimble.att_mtu - 3);
        return -ENOBUFS;
    }

    _GATT_PERIPHERAL_DEBUG("sendv: %" PRIuSIZE " bytes\n", len);

    int res = 0;
    struct os_mbuf *om = os_msys_get_pkthdr(len, 0);
    if (!om) {
        _GATT_PERIPHERAL_DEBUG("error: failed to allocate nimble buffer\n");
        return -ENOBUFS;
    }

    while (iolist) {
        res = os_mbuf_append(om, iolist->iol_base, iolist->iol_len);
        if (res != 0) {
            _GATT_PERIPHERAL_DEBUG("error: failed to append to nimble buffer: %d\n", res);
        }
        iolist = iolist->iol_next;
    }

    if (reliable) {
        res = ble_gatts_indicate_custom(ctx->transport.nimble.connection_handle, ctx->transport.nimble.upstream_handle, om);
    }
    else {
        res = ble_gatts_notify_custom(ctx->transport.nimble.connection_handle, ctx->transport.nimble.upstream_handle, om);
    }

    if (res != 0) {
        _GATT_PERIPHERAL_DEBUG("error, notifying/indicating failed: %i\n", res);
        // TODO: translate errors
        return res;
    }

    return 0;
}
