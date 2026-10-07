#include <stdint.h>
#include "net/unicoap/message.h"

#define ENABLE_DEBUG CONFIG_UNICOAP_DEBUG_LOGGING
#define DEBUG_PREFIX " "
#include "debug.h"
#include "private.h"

#define PDU_GATT_DEBUG(...) _UNICOAP_PREFIX_DEBUG(".pdu.gatt", __VA_ARGS__)

/**
 * 0   1   2   3   4       8       16      varying
 * +---+---+---+---+-------+-------+-------+---------+----+---------+
 * | R | M | C | A |  TKL  |  Code | Token | Options | ff | Payload |
 * +---+---+---+---+-------+-------+-------+---------+----+---------+
 * https://www.ietf.org/archive/id/draft-ietf-core-coap-over-gatt-00.html#figure-1
 */
typedef struct __attribute__((packed)) {
    uint8_t res_msgid_con_ackid_tokenlen;
    uint8_t code;
} unicoap_header_gatt_t;

/**
 * @brief       Gets the reserved bit
 * @param[in]   header   CoAP PDU header
 * @returns     Reserved bit
 */
static inline bool _get_reserved(const unicoap_header_gatt_t *header)
{
    return (header->res_msgid_con_ackid_tokenlen & 0x80) >> 7;
}

/**
 * @brief       Gets a message's ID
 * @param[in]   header   CoAP PDU header
 * @returns     Raw message ID
 */
static inline bool _get_message_id(const unicoap_header_gatt_t *header)
{
    return (header->res_msgid_con_ackid_tokenlen & 0x40) >> 6;
}

/**
 * @brief       Sets a message's ID.
 * @param[in]   header   CoAP PDU header
 * @param       message_id Raw message ID
 */
static inline void _set_message_id(unicoap_header_gatt_t *header, bool message_id)
{
    assert((message_id & ~0x1) == 0);
    header->res_msgid_con_ackid_tokenlen |= (message_id << 6);
}

/**
 * @brief       Gets the Confirm bit.
 * @param[in]   header   CoAP PDU header
 * @returns     Raw Confirm bit
 */
static inline bool _get_confirm(const unicoap_header_gatt_t *header)
{
    return (header->res_msgid_con_ackid_tokenlen & 0x20) >> 5;
}

/**
 * @brief       Sets the Confirm bit.
 * @param[in]   header   CoAP PDU header
 * @param       confirm Raw Confirm bit
 */
static inline void _set_confirm(unicoap_header_gatt_t *header, bool confirm)
{
    assert((confirm & ~0x1) == 0);
    header->res_msgid_con_ackid_tokenlen |= (confirm << 5);
}

/**
 * @brief       Gets the acknowledge ID.
 * @param[in]   header   CoAP PDU header
 * @returns     Raw acknowledge ID.
 */
static inline bool _get_acknowledge_id(const unicoap_header_gatt_t *header)
{
    return (header->res_msgid_con_ackid_tokenlen & 0x10) >> 4;
}

/**
 * @brief       Sets the acknowledge ID.
 * @param[in]   header   CoAP PDU header
 * @param       acknowledge_id Raw acknowledge ID.
 */
static inline void _set_acknowledge_id(unicoap_header_gatt_t *header, bool acknowledge_id)
{
    assert((acknowledge_id & ~0x1) == 0);
    header->res_msgid_con_ackid_tokenlen |= (acknowledge_id << 4);
}

/**
 * @brief       Gets a message's token length (segment)
 * @param[in]   header   CoAP PDU header
 * @returns     First segment of token length
 */
static inline uint8_t _get_token_length(const unicoap_header_gatt_t *header)
{
    return header->res_msgid_con_ackid_tokenlen & 0xf;
}

/**
 * @brief       Sets a message's token length (segment)
 * @param[in]   header   CoAP PDU header
 * @param token_length     First segment of token length
 */
static inline void _set_token_length(unicoap_header_gatt_t *header, uint8_t token_length)
{
    assert((token_length & ~0xf) == 0);
    header->res_msgid_con_ackid_tokenlen |= token_length;
}

/**
 * @brief       Gets a message's raw code (class + detail)
 * @param[in]   header   CoAP PDU header
 * @returns     Raw message code
 */
static inline uint8_t _get_code(const unicoap_header_gatt_t *header)
{
    return header->code;
}

/**
 * @brief       Sets a message's raw code (class + detail)
 * @param[in]   header   CoAP PDU header
 * @param code  Raw message code
 */
static inline void _set_code(unicoap_header_gatt_t *header, uint8_t code)
{
    header->code = code;
}

ssize_t unicoap_pdu_parse_gatt(uint8_t *pdu, size_t size, unicoap_message_t *message,
                               unicoap_message_properties_t *properties)
{
    // todo: remove
    // printf("PDU parse: ");
    // for (unsigned i = 0; i<size; i++) {
    //     printf("%02x", pdu[i]);
    // }
    // printf("\n");

    if (size < sizeof(unicoap_header_gatt_t)) {
        PDU_GATT_DEBUG("msg too short\n");
        return -EBADMSG;
    }
    // todo: may have alignment issues, same on rfc7252 btw
    const unicoap_header_gatt_t *header = (unicoap_header_gatt_t *)pdu;

    if (_get_reserved(header)) {
        PDU_GATT_DEBUG("reserved bit set\n");
        return -EBADMSG;
    }

    if ((_get_code(header) == UNICOAP_CODE_EMPTY) && (size > sizeof(unicoap_header_gatt_t))) {
        PDU_GATT_DEBUG("empty msg is too long\n");
        return -EBADMSG;
    }

    uint8_t *end = pdu + size;
    uint8_t *cursor = pdu + sizeof(unicoap_header_gatt_t);

    message->code = _get_code(header);
    properties->gatt.id = _get_message_id(header);
    properties->gatt.confirm = _get_confirm(header);
    properties->gatt.acknowledge_id = _get_acknowledge_id(header);
    properties->token_length = _get_token_length(header);
    properties->token = cursor;

    if (properties->token_length > CONFIG_UNICOAP_EXTERNAL_TOKEN_LENGTH_MAX) {
        /* From RFC 7252, Section 3
         * https://datatracker.ietf.org/doc/html/rfc7252#section-3
         * Lengths 9-15 are
         * reserved, MUST NOT be sent, and MUST be processed as a message
         * format error. */
        PDU_GATT_DEBUG("invalid token length %" PRIu8 "\n", properties->token_length);
        return -EBADMSG;
    }

    cursor += properties->token_length;
    if (cursor > end) {
        return -EBADMSG;
    }

    return unicoap_pdu_parse_options_and_payload(cursor, end, message);
}

ssize_t unicoap_header_build_gatt(uint8_t *header, size_t capacity,
                                  const unicoap_message_t *message,
                                  const unicoap_message_properties_t *properties)
{
    assert(properties->token_length <= 0xf);
    if (capacity < sizeof(unicoap_header_gatt_t) + properties->token_length) {
        PDU_GATT_DEBUG("not enough buffer space to build header " _UNICOAP_NEED_HAVE "\n",
            sizeof(unicoap_header_gatt_t) + properties->token_length, capacity);
        return -ENOBUFS;
    }

    unicoap_header_gatt_t *_header = (unicoap_header_gatt_t *)header;
    *_header = (unicoap_header_gatt_t){};
    _set_message_id(_header, properties->gatt.id);
    _set_confirm(_header, properties->gatt.confirm);
    _set_acknowledge_id(_header, properties->gatt.acknowledge_id);
    _set_token_length(_header, properties->token_length);
    header += sizeof(unicoap_header_gatt_t);
    _set_code(_header, message->code);

    memcpy(header, properties->token, properties->token_length);

    // todo: remove
    // printf("PDU build: ");
    // for (unsigned i = 0; i<sizeof(unicoap_header_gatt_t) + properties->token_length; i++) {
    //     printf("%02x", pdu[i]);
    // }
    // printf("\n");
    return sizeof(unicoap_header_gatt_t) + properties->token_length;
}

const char *unicoap_string_from_gatt_con(unicoap_message_properties_t *properties)
{
    return properties->gatt.confirm ? "C=1" : "C=0";
}

const char *unicoap_string_from_gatt_mid(unicoap_message_properties_t *properties)
{
    return properties->gatt.id ? "M=1" : "M=0";
}

const char *unicoap_string_from_gatt_ack(unicoap_message_properties_t *properties)
{
    return properties->gatt.acknowledge_id ? "A=1" : "A=0";
}
