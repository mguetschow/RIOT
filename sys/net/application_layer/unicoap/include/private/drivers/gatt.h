/*
 * SPDX-FileCopyrightText: 2026 Mikolai Gütschow
 * SPDX-FileCopyrightText: 2026 TU Dresden
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#pragma once

#include <host/ble_uuid.h>
#include <nimble/ble.h>
#include <stdint.h>

#include "net/unicoap.h"

typedef struct {
    struct {
        struct {
            ble_addr_t addr;
            uint16_t connection_handle;
            uint16_t downstream_handle;
            uint16_t upstream_handle;
            uint16_t att_mtu;
            struct {
                bool discovered : 1;
                bool connected : 1;
                bool service_found : 1;
                bool downstream_found : 1;
                bool upstream_found : 1;
                bool upstream_subscribed : 1;
                bool reported : 1;
            } state; /**< only accessed by nimble thread */
        } nimble; /**< will not be updated after state.reported == true */
        struct os_mbuf *buf;
        unicoap_job_t job;
        mutex_t roadblock; /**< locked as long as @p job is enqueued */
    } transport; /**< transport-layer state */
    struct {
        bool own_message_id : 1;  /**< Current own message ID */
        bool waiting_for_ack : 1; /**< currently waiting for ACK of sent confirmable message */
        bool peer_message_id : 1; /**< Current peer message ID */
        bool pending_ack : 1;     /**< ACK pending for received confirmable message */
        uint8_t last_header[2 + 8]; /**< Last received message data until token. Used for deduplication. */
        uint8_t last_header_length; /**< Length of last received message data. */
    } messaging; /**< messaging-layer state */
    bool is_used : 1;
    bool is_ready : 1;
    unicoap_transport_gatt_role_t role : 2;
} unicoap_gatt_ctx_t;

static inline unicoap_endpoint_t _ep_from_ctx(const unicoap_gatt_ctx_t *ctx)
{
    unicoap_endpoint_t ep = {
        .proto = UNICOAP_PROTO_GATT,
        .conn_handle_set = true,
        .conn_handle = ctx->transport.nimble.connection_handle,
    };
    memcpy(ep.peer_addr, ctx->transport.nimble.addr.val, sizeof(ep.peer_addr));
    return ep;
}

int unicoap_transport_start_gatt_central(unicoap_gatt_ctx_t *ctx);
int unicoap_transport_start_gatt_peripheral(unicoap_gatt_ctx_t *ctx);
void unicoap_gatt_notify_disconnect(unicoap_gatt_ctx_t *ctx);
void unicoap_gatt_notify_ready(unicoap_gatt_ctx_t *ctx);
void unicoap_gatt_on_rx(unicoap_gatt_ctx_t *ctx, struct os_mbuf *om);

// void unicoap_gatt_notify_callback(unicoap_gatt_ctx_t *ctx, unicoap_transport_gatt_event_t event);

/* Coap over gatt service UUID = 8df804b7-3300-496d-9dfa-f8fb40a236bc */
static const ble_uuid128_t unicoap_gatt_uuid_service = BLE_UUID128_INIT(
    0xbc, 0x36, 0xa2, 0x40, 0xfb, 0xf8, 0xfa, 0x9d,
    0x6d, 0x49, 0x00, 0x33, 0xb7, 0x04, 0xf8, 0x8d);

/* Coap over gatt downstream characteristic UUID = 8bf52767-5625-43ca-a678-70883a366866 */
static const ble_uuid128_t unicoap_gatt_uuid_downstream = BLE_UUID128_INIT(
    0x66, 0x68, 0x36, 0x3a, 0x88, 0x70, 0x78, 0xa6,
    0xca, 0x43, 0x25, 0x56, 0x67, 0x27, 0xf5, 0x8b);

/* Coap over gatt upstream characteristic UUID = ab3720c8-7fc0-41f8-aa2a-9a45c2c01a4b */
static const ble_uuid128_t unicoap_gatt_uuid_upstream = BLE_UUID128_INIT(
    0x4b, 0x1a, 0xc0, 0xc2, 0x45, 0x9a, 0x2a, 0xaa,
    0xf8, 0x41, 0xc0, 0x7f, 0xc8, 0x20, 0x37, 0xab);

// todo: maybe get rid of extern declaration, hide behind functions as below
extern unicoap_gatt_ctx_t _gatt_ctx[CONFIG_UNICOAP_GATT_CENTRAL_CONNECTIONS_MAX + CONFIG_UNICOAP_GATT_PERIPHERAL_CONNECTIONS_MAX];

unicoap_gatt_ctx_t *_gatt_ctx_create(unicoap_transport_gatt_role_t role);
unicoap_gatt_ctx_t *_gatt_ctx_find(const unicoap_endpoint_t *endpoint);
unicoap_gatt_ctx_t *_gatt_ctx_find_by_conn(uint16_t conn_handle);
void _gatt_ctx_free(unicoap_gatt_ctx_t *ctx);
void _gatt_ctx_reset(unicoap_gatt_ctx_t *ctx);
