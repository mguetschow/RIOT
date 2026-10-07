#include "container.h"

#include <string.h>

#include "mutex.h"
#include "net/unicoap.h"
#include "net/unicoap/transport.h"
#include "private/drivers/gatt.h"

#include "host/ble_att.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"

#define ENABLE_DEBUG CONFIG_UNICOAP_DEBUG_LOGGING
#define DEBUG_PREFIX " "
#include "debug.h"
#include "private.h"

#define _GATT_COMMON_DEBUG(...) _UNICOAP_PREFIX_DEBUG(".transport.gatt.common", __VA_ARGS__)

static unicoap_transport_gatt_event_cb _gatt_callback = NULL; /**< only accessed and called by unicoap thread */

static void _notify(unicoap_gatt_ctx_t *ctx, unicoap_transport_gatt_event_t event)
{
    if (_gatt_callback) {
        unicoap_endpoint_t ep = _ep_from_ctx(ctx);
        _gatt_callback(&ep, ctx->role, event);
    }
}

static void _start_gatt(unicoap_gatt_ctx_t *ctx)
{
#if IS_USED(MODULE_UNICOAP_DRIVER_GATT_CENTRAL)
    if (ctx->role == UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL) {
        unicoap_transport_start_gatt_central(ctx);
    }
#endif
#if IS_USED(MODULE_UNICOAP_DRIVER_GATT_PERIPHERAL)
    if (ctx->role == UNICOAP_TRANSPORT_GATT_ROLE_PERIPHERAL) {
        unicoap_transport_start_gatt_peripheral(ctx);
    }
#endif
}

static void _on_disconnect(unicoap_job_t *job)
{
    unicoap_gatt_ctx_t *ctx = container_of(job, unicoap_gatt_ctx_t, transport.job);
    assert(ctx->is_ready);
    ctx->is_ready = false;
    _notify(ctx, UNICOAP_TRANSPORT_GATT_EVENT_DISCONNECT);
    mutex_unlock(&ctx->transport.roadblock);

    if (IS_ACTIVE(CONFIG_UNICOAP_GATT_AUTOCONNECT)) {
        _gatt_ctx_reset(ctx);
        _start_gatt(ctx);
    }
    else {
        _gatt_ctx_free(ctx);
    }
}

void unicoap_gatt_notify_disconnect(unicoap_gatt_ctx_t *ctx)
{
    if (!mutex_trylock(&ctx->transport.roadblock)) {
        _GATT_COMMON_DEBUG("previous job not processed, dropping DISCONNECT event\n");
        return;
    }

    ctx->transport.job = UNICOAP_JOB(_on_disconnect);
    unicoap_loop_enqueue(&ctx->transport.job);
}


static void _on_ready(unicoap_job_t *job)
{
    unicoap_gatt_ctx_t *ctx = container_of(job, unicoap_gatt_ctx_t, transport.job);
    assert(!ctx->is_ready);
    ctx->is_ready = true;
    _notify(ctx, UNICOAP_TRANSPORT_GATT_EVENT_CONNECT);
    mutex_unlock(&ctx->transport.roadblock);

    if (IS_ACTIVE(CONFIG_UNICOAP_GATT_AUTOCONNECT)) {
        ctx = _gatt_ctx_create(ctx->role);
        if (ctx) {
            _start_gatt(ctx);
        }
    }
}

void unicoap_gatt_notify_ready(unicoap_gatt_ctx_t *ctx)
{
    if (!mutex_trylock(&ctx->transport.roadblock)) {
        _GATT_COMMON_DEBUG("previous job not processed, dropping READY event\n");
        return;
    }

    ctx->transport.nimble.state.reported = true;
    ctx->transport.nimble.att_mtu = ble_att_mtu(ctx->transport.nimble.connection_handle);
    ctx->transport.job = UNICOAP_JOB(_on_ready);
    unicoap_loop_enqueue(&ctx->transport.job);
}

static void _on_rx(unicoap_job_t *job)
{
    unicoap_gatt_ctx_t *ctx = container_of(job, unicoap_gatt_ctx_t, transport.job);
    struct os_mbuf *buf = ctx->transport.buf;
    mutex_unlock(&ctx->transport.roadblock);

    int res = 0;
    uint16_t len;

    /* read received data */
    res = ble_hs_mbuf_to_flat(buf, unicoap_receiver_buffer, sizeof(unicoap_receiver_buffer), &len);
    if (res != 0) {
        _GATT_COMMON_DEBUG("Error: mbuf to flat conversion failed; res=%d\n", res);
        return;
    }
    res = os_mbuf_free_chain(buf);
    if (res != 0) {
        _GATT_COMMON_DEBUG("Error: failure while freeing mbuf; res=%d\n", res);
        return;
    }

    // todo: could directly pass os_mbuf to messaging layer instead, no need to copy to flat
    unicoap_messaging_process_gatt(unicoap_receiver_buffer, len, ctx);
}

void unicoap_gatt_on_rx(unicoap_gatt_ctx_t *ctx, struct os_mbuf *om)
{
    if (!mutex_trylock(&ctx->transport.roadblock)) {
        _GATT_COMMON_DEBUG("previous job not processed, dropping RX event\n");
        return;
    }
    ctx->transport.buf = os_mbuf_dup(om);
    if (!ctx->transport.buf) {
        _GATT_COMMON_DEBUG("failed to duplicate buffer, dropping RX event\n");
        return;
    }
    ctx->transport.job = UNICOAP_JOB(_on_rx);
    unicoap_loop_enqueue(&ctx->transport.job);
}

typedef struct {
    unicoap_job_t job;
    unicoap_transport_gatt_role_t role;
    mutex_t roadblock;
} _start_args;

// todo: naming with _start above
static void _start2(unicoap_job_t *job)
{
    _start_args *args = container_of(job, _start_args, job);
    unicoap_gatt_ctx_t *ctx = _gatt_ctx_create(args->role);
    if (ctx) {
        _start_gatt(ctx);
    }
    mutex_unlock(&args->roadblock);
}

int unicoap_transport_gatt_start(unicoap_transport_gatt_role_t role)
{
    if (IS_ACTIVE(CONFIG_UNICOAP_GATT_AUTOCONNECT)) {
        unicoap_assist(
            API_ERROR("cannot manually start scanning in this configuration")
            FIXIT("Disable CONFIG_UNICOAP_GATT_AUTOCONNECT"));
        return -1;
    }

    _start_args args = {
        .job = UNICOAP_JOB(_start2),
        .role = role,
        .roadblock = MUTEX_INIT_LOCKED,
    };

    unicoap_loop_enqueue(&args.job);
    mutex_lock(&args.roadblock);

    return 0;
}

int unicoap_init_gatt(event_queue_t *queue)
{
    (void)queue;

    if (IS_ACTIVE(CONFIG_UNICOAP_GATT_AUTOCONNECT)) {
        if (IS_USED(MODULE_UNICOAP_DRIVER_GATT_CENTRAL)) {
            unicoap_gatt_ctx_t *ctx = _gatt_ctx_create(UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL);
            assert(ctx);
            _start_gatt(ctx);
        }
        if (IS_USED(MODULE_UNICOAP_DRIVER_GATT_PERIPHERAL)) {
            unicoap_gatt_ctx_t *ctx = _gatt_ctx_create(UNICOAP_TRANSPORT_GATT_ROLE_PERIPHERAL);
            assert(ctx);
            _start_gatt(ctx);
        }
    }

    return 0;
}

int unicoap_deinit_gatt(event_queue_t *queue)
{
    (void)queue;

    // todo: deinit should stop advertising/scanning if running, and teardown existing connections

    return 0;
}


typedef struct {
    unicoap_job_t job;
    unicoap_transport_gatt_event_cb callback;
    mutex_t roadblock;
} _set_event_args;

/**
 * @brief Set GATT Central event callback.
 *
 * runs on unicoap thread
 */
static void _set_event_callback(unicoap_job_t *job)
{
    _set_event_args *args = container_of(job, _set_event_args, job);

    // todo: how to hide _gatt_ctx object from here?
    for (size_t i=0; i<ARRAY_SIZE(_gatt_ctx); i++) {
        if (_gatt_ctx[i].is_ready) {
            /* fake disconnect event for previous callback */
            _notify(&_gatt_ctx[i], UNICOAP_TRANSPORT_GATT_EVENT_DISCONNECT);
        }
    }

    _gatt_callback = args->callback;
    mutex_unlock(&args->roadblock);

    for (size_t i=0; i<ARRAY_SIZE(_gatt_ctx); i++) {
        if (_gatt_ctx[i].is_ready) {
            /* inform new callback about existing connection */
            _notify(&_gatt_ctx[i], UNICOAP_TRANSPORT_GATT_EVENT_CONNECT);
        }
    }
}

void unicoap_transport_gatt_set_event_callback(unicoap_transport_gatt_event_cb callback)
{
    assert(callback);

    _set_event_args args = {
        .job = UNICOAP_JOB(_set_event_callback),
        .callback = callback,
        .roadblock = MUTEX_INIT_LOCKED,
    };

    unicoap_loop_enqueue(&args.job);
    mutex_lock(&args.roadblock);
}
