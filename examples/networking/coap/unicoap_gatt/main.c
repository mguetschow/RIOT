/*
 * SPDX-FileCopyrightText: 2024-2026 Carl Seifert
 * SPDX-FileCopyrightText: 2024-2026 TU Dresden
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/**
 * @file
 * @ingroup examples
 * @brief   Sample `unicoap` server application
 * @author  Carl Seifert <carl.seifert@tu-dresden.de>
 */

#include <stdio.h>
#include "net/unicoap.h"

/* This is needed for netifs_print_ipv6 in main below. */
#include "net/netif.h"
#include "net/unicoap/transport.h"
#include "shell.h"

static int handle_hello_request(unicoap_message_t* message, const unicoap_aux_t* aux,
                                unicoap_request_context_t* ctx, void* arg) {
    (void)aux;
    (void)arg;

    printf("app: %s /, %" PRIuSIZE " bytes\n",
           unicoap_string_from_method(message->method),
           unicoap_message_payload_get_size(message));

    /* String literals in C are null-terminated. For null-terminated string, we can use the
     * _string message initializers. */
    unicoap_response_init_string(message, UNICOAP_STATUS_CONTENT, "Hello, World!");

    /* Respond. Note that you do not need to return the result of @ref unicoap_send_response. */
    return unicoap_send_response(message, ctx);
}

/*
 * Let's define a CoAP resource. To define a new resource statically (at compile-time), you
 * use the @ref UNICOAP_RESOURCE macro. Note that you will need to supply an identifier. Let's
 * call it `hello`. The identifier is required by the [implementation](@ref UNICOAP_RESOURCE),
 * but not used otherwise. You can give it any name you want, as long as it is a unique C
 * variable name.
 */

UNICOAP_RESOURCE(hello) {
    /* Each resource must be assigned a path. This will be the part that follows the host/domain
     * and port in the CoAP URI. */
    .path = UNICOAP_PATH_ROOT,

    .flags = 0,

    /* You must declare what CoAP methods you want to allow for each resource. */
    .methods = UNICOAP_METHODS(UNICOAP_METHOD_GET, UNICOAP_METHOD_PUT, UNICOAP_METHOD_POST),

    /* Optionally, you can also restrict the resource to a set of transports. */
    .protocols = UNICOAP_PROTOCOLS(UNICOAP_PROTO_GATT),

    /* Finally, pass the handler and an optional argument. */
    .handler = handle_hello_request,
    .handler_arg = NULL
};

/* Next, we create a more complex resource. */

static bool is_valid_name(const char* string, size_t length) {
    for (size_t i = 0; i < length; i += 1) {
        if (string[i] == 0) {
            return false;
        }
    }
    return true;
}

static int handle_greeting_request(unicoap_message_t* message, const unicoap_aux_t* aux,
                                   unicoap_request_context_t* ctx, void* arg) {
    (void)aux;
    (void)arg;

    printf("app: %s /greeting, %" PRIuSIZE " bytes\n",
           unicoap_string_from_method(message->method),
           unicoap_message_payload_get_size(message));

    /* Our /greeting resource accepts a 'name' query parameter.
     * If you GET /greeting?name=RIOTeer, we want to respond with "Hello, RIOTeer!".
     * Besides, this URI is transmitted as two CoAP options:
     * - `Uri-Path`: "greeting"
     * - `Uri-Query`: "name=RIOTeer"
     */

    ssize_t res = 0;
    const char* name = NULL;

    if ((res = unicoap_options_get_first_uri_query_by_name_string(message->options, "name", &name)) <= 0) {
        printf("app: no name provided: %" PRIdSIZE " (%s)\n", res, strerror(-res));
        /* Returning a status code here will result in a response being sent automatically. */
        return UNICOAP_STATUS_BAD_REQUEST;
    }

    /* Validate any input. Here, we apply a 30 UTF-8 character limit. */
    if (res > 30 || !is_valid_name(name, res)) {
        unicoap_response_init_string(message, UNICOAP_STATUS_BAD_REQUEST, "invalid 'name' query");
        return unicoap_send_response(message, ctx);
    }

    /* Now we can craft out response. Let's start with the part that can fail. */

    /* Allocate options on the stack. Provide the size of the buffer that will contain the
     * response options. */
    UNICOAP_OPTIONS_ALLOC(options, 2);

    /* Set Content-Format option to text/plain */
    if (unicoap_options_set_content_format(&options, UNICOAP_FORMAT_TEXT) < 0) {
        return UNICOAP_STATUS_INTERNAL_SERVER_ERROR;
    }

    message->options = &options;

    /* Now, we create our payload. In theory, we could write "Hello, ", then the name, and then
     * "! ..." into that buffer. Instead, we can use a vectored payload. */

#define static_strlen(string) sizeof(string) - 1

    /* These are going to be the first and last payload chunks. */
#define _PREFIX "Hello, "
    iolist_t list = {
        .iol_base = _PREFIX,
        .iol_len = static_strlen(_PREFIX),
    };

#define _SUFFIX "! Welcome to our itsy bitsy tiny CoAP server!"
    iolist_t suffix = {
        .iol_base = _SUFFIX,
        .iol_len = static_strlen(_SUFFIX)
    };

    iolist_t name_chunk = {
        .iol_next = &suffix,
        .iol_base = (void*)name,
        .iol_len = res
    };
    list.iol_next = &name_chunk;

    unicoap_message_payload_set_chunks(message, &list);
    unicoap_response_set_status(message, UNICOAP_STATUS_CONTENT);

    /* Respond. Note that you do not need to return the result of @ref unicoap_send_response. */
    return unicoap_send_response(message, ctx);
}

UNICOAP_RESOURCE(greeting) {
    .path = UNICOAP_PATH("greeting"),

    .flags = UNICOAP_RESOURCE_FLAG_RELIABLE,

    .methods = UNICOAP_METHODS(UNICOAP_METHOD_GET),

    .handler = handle_greeting_request,
    .handler_arg = NULL
};

void _gatt_event_cb(unicoap_endpoint_t *endpoint, unicoap_transport_gatt_role_t role, unicoap_transport_gatt_event_t event)
{
    if (event == UNICOAP_TRANSPORT_GATT_EVENT_CONNECT) {
        printf("app: connected as %s to ", role == UNICOAP_TRANSPORT_GATT_ROLE_CENTRAL ? "central" : "peripheral");
    }
    else {
        printf("app: disconnected from ");
    }
    /* Because we're in possession of an endpoint now, we can print
     * a debug description of it. */
    unicoap_print_endpoint(endpoint);
    printf("\n");
}

int main(void) {
    /* By default, unicoap_init() is automatically called for you before main().
     * This is because auto_init_unicoap is part of the DEFAULT_MODULE makefile variable.
     * You can opt out of this default behavior by setting DISABLE_MODULE += auto_init_unicoap.
     * However, then you need to call unicoap_init() yourself. */
#if !IS_USED(MODULE_AUTO_INIT_UNICOAP)
    if (unicoap_init() < 0) {
        printf("app: failed to initialize unicoap\n");
    }
#endif
#if IS_USED(MODULE_UNICOAP_DRIVER_GATT_COMMON)
    unicoap_transport_gatt_set_event_callback(_gatt_event_cb);

#endif /* MODULE_UNICOAP_DRIVER_GATT_COMMON */

    /* By default, unicoap_init() will create a background thread. If you do not want that,
     * set CONFIG_UNICOAP_CREATE_THREAD to 0 and run the processing loop on a thread of your choice.
     * Note that this function is not available when CONFIG_UNICOAP_CREATE_THREAD is 1. Multiple
     * unicoap instances are not allowed. Note too that you can add multiple ports instead. */
#if !CONFIG_UNICOAP_CREATE_THREAD
    printf("app: running unicoap loop on main thread\n");
    unicoap_loop_run();
#endif
    /* start shell */
    puts("All up, running unicoap client shell");
    char line_buf[SHELL_DEFAULT_BUFSIZE];
    shell_run(NULL, line_buf, SHELL_DEFAULT_BUFSIZE);
    return 0;
}
