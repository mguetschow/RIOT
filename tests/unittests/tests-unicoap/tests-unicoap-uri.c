/*
 * SPDX-FileCopyrightText: 2024-2026 Carl Seifert
 * SPDX-FileCopyrightText: 2024-2026 TU Dresden
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/**
 * @file
 * @ingroup unittests
 * @brief   Unit tests for testing path matching functions
 * @author  Carl Seifert <carl.seifert@tu-dresden.de>
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "embUnit/AssertImpl.h"
#include "net/unicoap/options.h"
#include "net/unicoap/options/base.h"
#include "net/unicoap/transport.h"
#include "tests-unicoap.h"

#include "net/unicoap.h"


// todo: taken from client.c, could be factored out?
static int _parse(char *uri, unicoap_endpoint_t *endpoint, unicoap_options_t *options)
{
    int res;
    unicoap_destination_t destination = unicoap_destination_uri_string(uri);

    if (!uri_parser_is_absolute(destination.remote.uri, destination._string_length)) {
        // _URI_DEBUG("absolute URI expected\n");
        return -EINVAL;
    }

    uri_parser_result_t parsed = { 0 };
    if ((res = uri_parser_process(
        &parsed, destination.remote.uri, destination._string_length
    )) < 0) {
        return res;
    }
    if (!parsed.scheme) {
        return -EINVAL;
    }

    return unicoap_uri_populate(&parsed, endpoint, options);
}

static void test_gatt(void)
{
    unicoap_endpoint_t endpoint = { 0 };
    UNICOAP_OPTIONS_ALLOC_DEFAULT(options);
    TEST_ASSERT_EQUAL_INT(0, _parse("coap://001122334455.ble.arpa/.well-known/core", &endpoint, &options));
    TEST_ASSERT_EQUAL_INT(UNICOAP_PROTO_GATT, endpoint.proto);
    uint8_t exp[sizeof(endpoint.peer_addr)] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    _TEST_ASSERT_EQUAL_BYTES(exp, endpoint.peer_addr, sizeof(endpoint.peer_addr));
    TEST_ASSERT_EQUAL_INT(0, endpoint.conn_handle_set);
}

Test* tests_unicoap_uri(void)
{
    EMB_UNIT_TESTFIXTURES(fixtures){
        new_TestFixture(test_gatt),
    };

    EMB_UNIT_TESTCALLER(test_unicoap, NULL, NULL, fixtures);

    return (Test*)&test_unicoap;
}
