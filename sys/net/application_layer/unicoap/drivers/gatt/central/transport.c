#include "container.h"
#include "host/ble_uuid.h"

#include <host/ble_att.h>
#include <services/gap/ble_svc_gap.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"
#include "iolist.h"
#include "modules.h"
#include "mutex.h"
#include "net/unicoap.h"
#include "net/unicoap/transport.h"
#include "private/drivers/gatt.h"
#include <string.h>

#define ENABLE_DEBUG CONFIG_UNICOAP_DEBUG_LOGGING
#define DEBUG_PREFIX " "
#include "debug.h"
#include "private.h"

#include "nimble_addr.h"
#include "nimble_riot.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "net/bluetil/ad.h"

#include "os/os_mbuf.h"

#define _GATT_CENTRAL_DEBUG(...) _UNICOAP_PREFIX_DEBUG(".transport.gatt.central", __VA_ARGS__)

UNICOAP_DECL_RECEIVER_STORAGE_EXTERN;

/** Check whether we want to connect to a peer given the discovered fields (services).
 * If the coap over gatt service is present, we return true.
 * Based on: https://github.com/apache/mynewt-nimble/blob/e4f97e24a1c5844bae9fa45ad846324008cf14e0/apps/blecent/sres/main.c#L260
 */
static inline bool _nimble_should_connect(const struct ble_gap_disc_desc *disc)
{
    /* The device has to be advertising connectability. */
    if (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
        disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
        return false;
    }

    int res;
    struct ble_hs_adv_fields fields;
    res = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (res != 0) {
        _GATT_CENTRAL_DEBUG("error: parsing fields: %d\n", res);
        return false;
    }

    for (int i = 0; i < fields.num_uuids128; i++) {
        if (ble_uuid_cmp(&fields.uuids128[i].u, &unicoap_gatt_uuid_service.u) == 0) {
            return true;
        }
    }
    return false;
}

static void _nimble_notify_if_ready(unicoap_gatt_ctx_t *ctx)
{
    if (!ctx->transport.nimble.state.downstream_found ||
        !ctx->transport.nimble.state.upstream_subscribed ||
        ctx->transport.nimble.state.reported) {
        /* not yet ready or already reported to unicoap thread */
        return;
    }

    unicoap_gatt_notify_ready(ctx);
}

/**
 * Callback that is fired after we performed the subscription on the notify/indicate characteristic.
 */
static int _nimble_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    unicoap_gatt_ctx_t *ctx = arg;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        DEBUG("error: _on_subscribe with status: %" PRIu16 "\n", error->status);
        return error->status;
    }

    _GATT_CENTRAL_DEBUG("Subscribe complete; status=%d conn_handle=%d "
                       "attr_handle=%d\n",
                       error->status, conn_handle, attr->handle);

    assert(!ctx->transport.nimble.state.upstream_subscribed);
    ctx->transport.nimble.state.upstream_subscribed = true;

    _nimble_notify_if_ready(ctx);

    return 0;
}

/**
 * Callback that is fired when a upstream descripter was discovered.
 * If we found the CCCD (Client Characteristic Configuration Descriptor), we
 * perform the subscription process on it.
 */
static int _nimble_on_upstream_descriptor(uint16_t conn_handle, const struct ble_gatt_error *error,
                                   uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        DEBUG("error: _on_upstream_descriptor with status: %" PRIu16 "\n", error->status);
        return error->status;
    }

    if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) != 0) {
        return 0;
    }

    _GATT_CENTRAL_DEBUG("found: upstream descriptor\n");

    /*
     * Subscribe to notifications for the Unread Alert Status characteristic.
     * A central enables notifications by writing two bytes (1, 0) to the
     * characteristic's client-characteristic-configuration-descriptor (CCCD).
     * Based on: https://github.com/apache/mynewt-nimble/blob/ca67e3015eec30eb97c853112441082d709e43ba/apps/blecent/sres/main.c#L153
     */
    uint8_t value[2] = { 1, 0 };

    int res;
    res = ble_gattc_write_flat(conn_handle, dsc->handle, value, sizeof value,
                              _nimble_on_subscribe, arg);
    if (res != 0) {
        _GATT_CENTRAL_DEBUG("error: failed to subscribe to characteristic; res=%d\n", res);
        return res;
    }
    return 0;
}

/**
 * @brief Handle characteristic by saving CoAP-over-GATT downstream and
 *        discovering CoAP-over-GATT upstream descriptors
 *
 * Runs on nimble thread.
 */
static int _nimble_on_characteristic(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    unicoap_gatt_ctx_t *ctx = arg;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        DEBUG("error: _on_characteristic with status: %" PRIu16 "\n", error->status);
        return error->status;
    }

    if (ble_uuid_cmp(&unicoap_gatt_uuid_upstream.u, &chr->uuid.u) == 0) {
        _GATT_CENTRAL_DEBUG("found: upstream characteristic\n");
        // todo: check whether same service may advertise same characteristic twice, also handle multiple services gracefully
        assert(!ctx->transport.nimble.state.upstream_found);
        ctx->transport.nimble.state.upstream_found = true;
        ctx->transport.nimble.upstream_handle = chr->val_handle;
        int res;
        res = ble_gattc_disc_all_dscs(conn_handle, chr->val_handle, 65535, _nimble_on_upstream_descriptor, ctx);
        if (res != 0) {
            _GATT_CENTRAL_DEBUG("error: failed descriptor discovery; res=%d\n", res);
            return res;
        }
    }
    else if (ble_uuid_cmp(&unicoap_gatt_uuid_downstream.u, &chr->uuid.u) == 0) {
        _GATT_CENTRAL_DEBUG("found: downstream characteristic\n");
        assert(!ctx->transport.nimble.state.downstream_found);
        ctx->transport.nimble.state.downstream_found = true;
        ctx->transport.nimble.downstream_handle = chr->val_handle;
    }
    else {
        return -1; // todo: this would actually stop the whole discovery! return 0 instead to continue until we found everything we were looking for!
    }

    _nimble_notify_if_ready(ctx);

    return 0;
}

/**
 * @brief Handle service by discovering characteristics on CoAP-over-GATT service
 *
 * Runs on nimble thread.
 */
static int _nimble_on_service(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service, void *arg)
{
    unicoap_gatt_ctx_t *ctx = arg;

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        DEBUG("error: _on_service with status: %" PRIu16 "\n", error->status);
        return error->status;
    }

    if (ble_uuid_cmp(&unicoap_gatt_uuid_service.u, &service->uuid.u) != 0) {
        return 1;
    }

    // todo: double-check if BLE may advertise same service twice
    assert(!ctx->transport.nimble.state.service_found);
    ctx->transport.nimble.state.service_found = true;

    _GATT_CENTRAL_DEBUG("found: CoAP-over-GATT service\n");
    int res;
    res = ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                  _nimble_on_characteristic, ctx);
    if (res != 0) {
        _GATT_CENTRAL_DEBUG("error: failed characteristic discovery; res=%d\n", res);
        return res;
    }

    return 0;
}

/**
 * Callback that is fired after GAP events. We currently process the following:
 * - discovery event (check if we discoverd a coap over gatt client)
 * - connect event (fired after discovery and connection process)
 * - notify/indicate event (fired when we are connected and subscribed to a characteristic
 *   and the peer notifies/indicates on that characteristic)
 */
static int _nimble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    unicoap_gatt_ctx_t *ctx = arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* peripheral discovered */
        assert(!ctx->transport.nimble.state.discovered);

        int res;

        /* Don't do anything if we don't care about this advertiser. */
        if (!_nimble_should_connect(&event->disc)) {
            _GATT_CENTRAL_DEBUG("does not match, not connecting\n");
            return -1;
        }

        /* Scanning must be stopped before a connection can be initiated. */
        res = ble_gap_disc_cancel();
        if (res != 0) {
            _GATT_CENTRAL_DEBUG("Failed to cancel scan; res=%d\n", res);
            return res;
        }

        ctx->transport.nimble.state.discovered = true;
        ctx->transport.nimble.addr = event->disc.addr;

        /* Try to connect the to advertiser with 3s timeout */
        res = ble_gap_connect(nimble_riot_own_addr_type, &event->disc.addr, 3000, NULL, _nimble_gap_event_cb, ctx);
        if (res != 0) {
            _GATT_CENTRAL_DEBUG(
                "Error: Failed to connect to device; addr_type=%d res=%d addr=", event->disc.addr.type, res);
            nimble_addr_print(&event->disc.addr);
            _GATT_CENTRAL_DEBUG("\n");
            return res;
        }

        return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status == 0) {
            // todo: make sure that on further failures we continue scanning!
            assert(!ctx->transport.nimble.state.connected);
            ctx->transport.nimble.state.connected = true;
            ctx->transport.nimble.connection_handle = event->connect.conn_handle;

            _GATT_CENTRAL_DEBUG("connected to peripheral\n");

            int res;
            /* Find service descriptor */
            res = ble_gattc_disc_svc_by_uuid(event->connect.conn_handle, &unicoap_gatt_uuid_service.u, _nimble_on_service, ctx);
            if (res != 0) {
                _GATT_CENTRAL_DEBUG("error: failed service discovery; res=%d\n", res);
                return 1;
            }

            /* as a central device, negotiate the MTU size to allow for larger messages */
            res = ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
            if (res != 0) {
                _GATT_CENTRAL_DEBUG("error: failed exchange MTU; res=%d\n", res);
                return 1;
            }

            return 0;
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        _GATT_CENTRAL_DEBUG("disconnect event\n");
        unicoap_gatt_notify_disconnect(ctx);

        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* Peer sent us a notification or indication. */
        _GATT_CENTRAL_DEBUG("received %s; conn_handle=%d attr_handle=%d "
                           "attr_len=%d\n",
                           event->notify_rx.indication ? "indication" : "notification",
                           event->notify_rx.conn_handle, event->notify_rx.attr_handle,
                           OS_MBUF_PKTLEN(event->notify_rx.om));

        assert(ctx->transport.nimble.connection_handle == event->notify_rx.conn_handle);
        assert(ctx->transport.nimble.upstream_handle == event->notify_rx.attr_handle);
        unicoap_gatt_on_rx(ctx, event->notify_rx.om);

        return 0;
    }

    default:
        _GATT_CENTRAL_DEBUG("Unhandled GAP event: %d\n", event->type);
    }

    return 0;
}

/**
 * Start scan procedure for peripheral discovery.
 * Based on: https://github.com/apache/mynewt-nimble/blob/bc190e4f1c00ae05201359c29c5c2bf94d9c0c59/apps/blecent/sres/main.c#L216
 */
int unicoap_transport_start_gatt_central(unicoap_gatt_ctx_t *ctx)
{
    int res;

    struct ble_gap_disc_params disc_params = {
        /* avoid repeated advertisements from the same device */
        .filter_duplicates = 1,
        /* don't send follow-up scan requests to each advertiser. */
        .passive = 1,
        /* Use defaults for the rest of the parameters. */
    };

    res = ble_gap_disc(nimble_riot_own_addr_type, BLE_HS_FOREVER, &disc_params, _nimble_gap_event_cb, ctx);
    if (res != 0) {
        _GATT_CENTRAL_DEBUG("error initiating GAP peripheral discovery procedure; res=%x\n", res);
    }
    return res;
}

// todo: maybe pass mbuf from messaging as that may also be passed during rx there
int unicoap_transport_sendv_gatt_central(iolist_t* iolist, const unicoap_gatt_ctx_t *ctx, bool reliable)
{
    assert(iolist);
    assert(ctx);
    assert(ctx->is_used);

    // todo: according to Claude, write does never truncate (as notify/indicate in peripheral does)
    _GATT_CENTRAL_DEBUG("sendv: %" PRIuSIZE " bytes\n", iolist_size(iolist));

    int res = 0;
    struct os_mbuf *om = os_msys_get_pkthdr(iolist_size(iolist), 0);
    if (!om) {
        _GATT_CENTRAL_DEBUG("error: failed to allocate nimble buffer\n");
        return -ENOBUFS;
    }

    while (iolist) {
        res = os_mbuf_append(om, iolist->iol_base, iolist->iol_len);
        if (res != 0) {
            _GATT_CENTRAL_DEBUG("error: failed to append to nimble buffer: %d\n", res);
        }
        iolist = iolist->iol_next;
    }

    if (reliable) {
        struct ble_gatt_attr attr = { .handle = ctx->transport.nimble.downstream_handle, .offset = 0, .om = om };
        res = ble_gattc_write_reliable(ctx->transport.nimble.connection_handle, &attr, 1, NULL, NULL);
    }
    else {
        res = ble_gattc_write(ctx->transport.nimble.connection_handle, ctx->transport.nimble.downstream_handle, om, NULL, NULL);
    }
    if (res != 0) {
        _GATT_CENTRAL_DEBUG("error, writing failed: %i\n", res);
        // TODO: translate errors
        return res;
    }

    return 0;
}
