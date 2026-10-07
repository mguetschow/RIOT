
#include <stddef.h>
#include <stdint.h>
#include "container.h"

#include "macros/utils.h"
#include "modules.h"
#include "net/unicoap/config.h"
#include "net/unicoap/message.h"
#include "net/unicoap/transport.h"
#include "private/drivers/gatt.h"
#include "private/packet.h"

#define ENABLE_DEBUG CONFIG_UNICOAP_DEBUG_LOGGING
#define DEBUG_PREFIX " "
#include "debug.h"
#include "private.h"

#define MESSAGING_GATT_DEBUG(...) _UNICOAP_PREFIX_DEBUG(".messaging.gatt", __VA_ARGS__)

#define PACKET_GATT_DEBUG(packet)                                                               \
    DEBUG("<%s %s %s %s token=", unicoap_string_from_gatt_mid(&packet->properties),                \
          unicoap_string_from_gatt_con(&packet->properties),                               \
          unicoap_string_from_gatt_ack(&packet->properties),                                       \
          unicoap_string_from_code_class((packet)->message->code));                             \
                                                                                                \
    _UNICOAP_DEBUG_HEX((packet)->properties.token, (packet)->properties.token_length);          \
                                                                                                \
    DEBUG(" code=" UNICOAP_CODE_CLASS_DETAIL_FORMAT " %s payload=(%" PRIuSIZE " bytes) "        \
          "options=(%" PRIuSIZE "; %" PRIuSIZE " bytes)>",                                      \
          unicoap_code_class((packet)->message->code),                                          \
          unicoap_code_detail((packet)->message->code),                                         \
          unicoap_string_from_code((packet)->message->code),                                    \
          (packet)->message->payload_size,                                                      \
          (packet)->message->options ? (packet)->message->options->option_count : 0,            \
          (packet)->message->options ? (packet)->message->options->storage_size : 0             \
    )

unicoap_gatt_ctx_t _gatt_ctx[CONFIG_UNICOAP_GATT_CENTRAL_CONNECTIONS_MAX + CONFIG_UNICOAP_GATT_PERIPHERAL_CONNECTIONS_MAX] = { 0 };

/**
 * Alloc new endpoint state. Based on transmission in rfc7252.
 */
static inline unicoap_gatt_ctx_t *_gatt_ctx_alloc(unicoap_transport_gatt_role_t role)
{
    assert(role != UNICOAP_TRANSPORT_GATT_ROLE_UNSPECIFIED);
    size_t start = (role == UNICOAP_TRANSPORT_GATT_ROLE_PERIPHERAL) ? CONFIG_UNICOAP_GATT_CENTRAL_CONNECTIONS_MAX : 0;
    size_t end = (role == UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL) ? CONFIG_UNICOAP_GATT_CENTRAL_CONNECTIONS_MAX : ARRAY_SIZE(_gatt_ctx);
    /* Find empty slot in list of endpoint states. */
    for (size_t i=start; i<end; i++) {
        if (!_gatt_ctx[i].is_used) {
            return &_gatt_ctx[i];
        }
    }
    return NULL;
}

/**
 * Free endpoint state.
 */
void _gatt_ctx_free(unicoap_gatt_ctx_t *ctx)
{
    ctx->is_used = false;
}

/**
 * Reset endpoint state.
 */
void _gatt_ctx_reset(unicoap_gatt_ctx_t *ctx)
{
    assert(ctx);
    assert(!ctx->is_ready);

    unicoap_transport_gatt_role_t role = ctx->role;
    memset(ctx, 0, sizeof(unicoap_gatt_ctx_t));
    ctx->is_used = 1;
    ctx->role = role;

    ctx->messaging.own_message_id = 1;
    ctx->messaging.peer_message_id = 0;
    ctx->messaging.waiting_for_ack = 0;
}

/**
 * Create new endpoint state.
 */
unicoap_gatt_ctx_t *_gatt_ctx_create(unicoap_transport_gatt_role_t role)
{
    unicoap_gatt_ctx_t *ctx = _gatt_ctx_alloc(role);
    if (!ctx) {
        return NULL;
    }
    ctx->role = role;
    _gatt_ctx_reset(ctx);
    // todo: missing matching free!

    // todo: combine with alloc or not?

    return ctx;
}

/**
 * Find the endpoint state for a given endpoint.
 */
unicoap_gatt_ctx_t *_gatt_ctx_find(const unicoap_endpoint_t *endpoint)
{
    for (size_t i=0; i<ARRAY_SIZE(_gatt_ctx); i++) {
        unicoap_gatt_ctx_t *ctx = &_gatt_ctx[i];
        if (ctx->is_used == 0) {
            continue;
        }
        unicoap_endpoint_t ep = _ep_from_ctx(ctx);
        if (unicoap_endpoint_is_equal(&ep, endpoint)) {
            return ctx;
        }
    }
    return NULL;
}

/**
 * Find the endpoint state for a given endpoint.
 */
unicoap_gatt_ctx_t *_gatt_ctx_find_by_conn(uint16_t conn_handle)
{
    for (size_t i=0; i<ARRAY_SIZE(_gatt_ctx); i++) {
        unicoap_gatt_ctx_t *ctx = &_gatt_ctx[i];
        if (ctx->is_used == 0) {
            continue;
        }
        if (ctx->transport.nimble.connection_handle == conn_handle) {
            return ctx;
        }
    }
    return NULL;
}

/**
 * Send via GATT
 */
static int _sendv(iolist_t* iolist, const unicoap_gatt_ctx_t *ctx, bool reliable)
{
    assert(iolist);
    assert(ctx);
#if IS_USED(MODULE_UNICOAP_DRIVER_GATT_CENTRAL)
    if (ctx->role == UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL) {
        extern int unicoap_transport_sendv_gatt_central(iolist_t* iolist, const unicoap_gatt_ctx_t *ctx, bool reliable);
        return unicoap_transport_sendv_gatt_central(iolist, ctx, reliable);
    }
#endif
#if IS_USED(MODULE_UNICOAP_DRIVER_GATT_PERIPHERAL)
    if (ctx->role == UNICOAP_TRANSPORT_GATT_ROLE_PERIPHERAL) {
        extern int unicoap_transport_sendv_gatt_peripheral(iolist_t* iolist, const unicoap_gatt_ctx_t *ctx, bool reliable);
        return unicoap_transport_sendv_gatt_peripheral(iolist, ctx, reliable);
    }
#endif
    return -ENOTSUP;
}

/**
 * Build and send BLE GATT PDU.
 */
static ssize_t _build_and_send_pdu(unicoap_packet_t *packet, unicoap_gatt_ctx_t *ctx)
{
    assert(packet);
    assert(packet->remote);

    ssize_t res = 0;
    uint8_t header[UNICOAP_HEADER_SIZE_MAX];
    iolist_t lists[UNICOAP_PDU_IOLIST_COUNT];

    if ((res = unicoap_header_build_gatt(header, sizeof(header) - 1, packet->message,
                                         &packet->properties)) < 0) {
        return res;
    }
    if ((res = unicoap_pdu_buildv_options_and_payload(header, res, packet->message, lists)) < 0) {
        return res;
    }

    MESSAGING_GATT_DEBUG("sending ");
    PACKET_GATT_DEBUG(packet);
    DEBUG("\n");

    if ((res = _sendv(lists, ctx, packet->properties.gatt.confirm)) < 0) {
        return res;
    }

    return 0;
}

/**
 * Based on _send_empty_message from rfc7252
 */
static int _send_empty_message(unicoap_packet_t *packet, unicoap_gatt_ctx_t *ctx)
{
    packet->message->code = UNICOAP_CODE_EMPTY;
    packet->message->options = NULL;
    packet->message->payload = NULL;
    packet->message->payload_size = 0;

    packet->properties.token = NULL;
    packet->properties.token_length = 0;

    return (int)_build_and_send_pdu(packet, ctx);
}

/**
 * Just send acknowledgment as empty message.
 * - set ack bit to latest peer message ID.
 * - set conf bit to 0 (otherwise we get an infinite ack-loop).
 * - set message-id bit to our latest message ID.
 */
static inline int _empty_acknowledge(unicoap_packet_t *packet, unicoap_gatt_ctx_t *ctx)
{
    assert(packet);
    assert(ctx);
    MESSAGING_GATT_DEBUG("sending empty ACK\n");
    packet->properties.gatt.acknowledge_id = ctx->messaging.peer_message_id;
    packet->properties.gatt.confirm = 0;
    packet->properties.gatt.id = ctx->messaging.own_message_id;;
    return _send_empty_message(packet, ctx);
}

/* from rfc7252/common/messaging/messaging.c */
#define _IGNORED (-7252)

/**
 * Process messaging layer relevant parts of the received packet.
 * Oriented on rfc7252 messaging.
 */
static int _process_messaging_layer(unicoap_packet_t *packet, unicoap_gatt_ctx_t *ctx)
{
    assert(packet);
    assert(packet->message);
    assert(ctx);

    MESSAGING_GATT_DEBUG("received ");
    PACKET_GATT_DEBUG(packet);
    DEBUG("\n");

    /* Confirm bit handling */
    if (packet->properties.gatt.confirm) {
        if (ctx->messaging.pending_ack) {
            // todo: or should we let it overwrite?
            // RFC states: The sender may cancel the transmission by sending an empty message with the same M and C bits, or by sending different message with these bits (which are then all unreliable transmissions).
            MESSAGING_GATT_DEBUG("received reliable message while ACK was pending, ignoring\n");
            return -EPROTO;
        }
        ctx->messaging.pending_ack = true;
        ctx->messaging.peer_message_id = packet->properties.gatt.id;
    }

    unicoap_message_t *message = packet->message;

    /* from rfc7252/udp/messaging.c */
    if (message->code == UNICOAP_CODE_EMPTY && message->payload_size != 0) {
        MESSAGING_GATT_DEBUG("received code 0.00, but message not empty, ignoring\n");
        return -EPROTO;
    }

    /* ACK handling */
    if (ctx->messaging.waiting_for_ack &&
        ctx->messaging.own_message_id == packet->properties.gatt.acknowledge_id) {

        MESSAGING_GATT_DEBUG("received ACK, handling ACK w.r.t. endpoint state\n");
        // todo: unclear in RFC when MID is actually flipped
        ctx->messaging.own_message_id = !ctx->messaging.own_message_id;
        ctx->messaging.waiting_for_ack = false;

        /* ACK without payload for our previous packet. */
        if (message->code == UNICOAP_CODE_EMPTY) {
            return _IGNORED;
        }
    }

    /* Empty non-confirmable message -> do nothing */
    if (message->code == UNICOAP_CODE_EMPTY && !packet->properties.gatt.confirm) {
        MESSAGING_GATT_DEBUG("received empty NON, ignoring\n");
        return _IGNORED;
    }

    return 0;
}

/**
 * Process incoming coap over gatt message.
 * Oriented on rfc7252 messaging
 */
int unicoap_messaging_process_gatt(const uint8_t *pdu, size_t size, unicoap_gatt_ctx_t *ctx)
{
    unicoap_options_t options = { 0 };
    unicoap_message_t message = { .options = &options };

    unicoap_endpoint_t remote = _ep_from_ctx(ctx);
    unicoap_packet_t packet = { .remote = &remote, .message = &message };

    /* Deduplication */
    if (ctx->messaging.last_header_length > 0 &&
        size >= ctx->messaging.last_header_length &&
        memcmp(pdu, ctx->messaging.last_header, ctx->messaging.last_header_length) == 0) {
        MESSAGING_GATT_DEBUG("ignoring duplicate message\n");
        return 0;
    }
    ctx->messaging.last_header_length = MIN(size, sizeof(ctx->messaging.last_header));
    memcpy(ctx->messaging.last_header, pdu, ctx->messaging.last_header_length);

    int res = 0;
    if ((res = unicoap_pdu_parse_gatt((uint8_t *)pdu, size, &message, &packet.properties)) < 0) {
        return res;
    }

    if ((res = _process_messaging_layer(&packet, ctx)) < 0) {
        if (res != _IGNORED) {
            MESSAGING_GATT_DEBUG("messaging layer failure: %i (%s)\n", res, strerror(-res));
        }
        return res;
    }

    unicoap_exchange_arg_t arg;
    unicoap_messaging_flags_t flags;

    switch (res = unicoap_exchange_preprocess(&packet, &flags, &arg, 0)) {
    case UNICOAP_PREPROCESSING_SUCCESS_REQUEST:
        /* Nothing to do? */
        break;

    case UNICOAP_PREPROCESSING_SUCCESS_RESPONSE:
        // todo: this is broken!!
        // if (packet.properties.gatt.confirm) {
        //     MESSAGING_GATT_DEBUG("sending empty ACK for expected response\n");
        //     _empty_acknowledge(&packet, ctx);
        //     return 0;
        // }
        break;

    default:
        if (res < 0) {
            return res;
        }
        break;
    }

    /* processor is prewarmed, let's throw the message in the processor */
    if ((res = unicoap_exchange_process(&packet, arg)) < 0) {
        return res;
    }

    return 0;
}

/**
 * Send message. Based on rfc7252/common/messaging/messaging.c
 */
int unicoap_messaging_send_gatt(unicoap_packet_t* packet, unicoap_messaging_flags_t flags, void* exchange)
{
    (void)exchange;

    assert(packet);
    assert(packet->remote);

    int res = 0;
    unicoap_gatt_ctx_t *ctx = _gatt_ctx_find(packet->remote);
    if (!ctx) {
        MESSAGING_GATT_DEBUG("unknown peer\n");
        return -EADDRNOTAVAIL;
    }

    if (ctx->messaging.waiting_for_ack && (flags & UNICOAP_MESSAGING_FLAG_RELIABLE)) {
        /* Previous confirmable message not yet acknowledged */
        MESSAGING_GATT_DEBUG("sending failed (previous confirmable message not yet confirmed)\n");
        return -EAGAIN;
    }

    packet->properties.gatt.id = ctx->messaging.own_message_id;
    packet->properties.gatt.confirm = ((flags & UNICOAP_MESSAGING_FLAG_RELIABLE) != 0);

    if (ctx->messaging.pending_ack) {
        // todo: A=1 seems to be set although not expected for C=0....
        packet->properties.gatt.acknowledge_id = ctx->messaging.peer_message_id;
    }

    if ((res = (int)_build_and_send_pdu(packet, ctx)) < 0) {
        MESSAGING_GATT_DEBUG("sending failed (during _build_and_send_pdu)\n");
        return res;
    }

    if (ctx->messaging.pending_ack) {
        /* no failure during send */
        ctx->messaging.pending_ack = false;
    }

    return res;
}


void unicoap_messaging_print_gatt_state(void)
{
#if ENABLE_DEBUG
    printf("\n\t- GATT roles: %s %s\n",
           (IS_USED(MODULE_UNICOAP_DRIVER_GATT_CENTRAL) ? "central" : ""),
           (IS_USED(MODULE_UNICOAP_DRIVER_GATT_PERIPHERAL) ? "peripheral" : ""));

    printf("\t- GATT contexts (%" PRIuSIZE " total):\n",
           ARRAY_SIZE(_gatt_ctx));

#  if CONFIG_UNICOAP_RFC7252_TRANSMISSIONS_MAX > 0
    for (size_t i = 0; i < ARRAY_SIZE(_gatt_ctx); i += 1) {
        unicoap_gatt_ctx_t* ctx = &_gatt_ctx[i];
        printf("\t\t- context #%" PRIuSIZE "\n", i);

        if (!ctx->is_used) {
            continue;
        }

        if (!ctx->is_ready) {
            printf("\t\t\t(no connection established yet)");
            continue;
        }

        printf("\t\t\t- role=%s\n", ctx->role == UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL ? "central" :
                                    ctx->role == UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL ? "peripheral" :
                                    "(unspecified)");
        // printf("\t\t\t- addr=%" PRIu16 "\n", transmission->id);
        printf("\t\t\t- connection handle=%" PRIu16 "\n", ctx->transport.nimble.connection_handle);
        printf("\t\t\t- downstream handle=%" PRIu16 "\n", ctx->transport.nimble.downstream_handle);
        printf("\t\t\t- upstream handle=%" PRIu16 "\n", ctx->transport.nimble.upstream_handle);
        printf("\t\t\t- ATT MTU=%" PRIu16 "\n", ctx->transport.nimble.att_mtu);
        printf("\t\t\t- messaging state:\n");
        printf("\t\t\t\t- next message ID:%u\n", ctx->messaging.own_message_id);
        printf("\t\t\t\t- peer message ID:%u\n", ctx->messaging.peer_message_id);
        printf("\t\t\t\t- waiting for ACK:%u\n", ctx->messaging.waiting_for_ack);
    }
#endif /* CONFIG_UNICOAP_RFC7252_TRANSMISSIONS_MAX > 0 */

#endif /* ENABLE_DEBUG */
}
