# Default Makefile, for simple unicoap server sample application

# name of your application
APPLICATION = unicoap_server

# If no BOARD is found in the environment, use this default:
BOARD ?= native

# This has to be the absolute path to the RIOT base directory:
RIOTBASE ?= $(CURDIR)/../../../..

# Import unicoap
USEMODULE += unicoap

# Import server submodule
USEMODULE += unicoap_server
# Cross-file CoAP resource definitions
USEMODULE += unicoap_server_resource_declarations

# Import client submodule
USEMODULE += unicoap_client
# URI support
USEMODULE += unicoap_client_uri

CFLAGS += -DDEBUG_ASSERT_VERBOSE=1

# todo
# This module is needed for CoAP over BLE GATT
# USEMODULE += unicoap_driver_gatt_central
# USEMODULE += unicoap_driver_gatt_peripheral
# It is okay to import only either of the CoAP over UDP and CoAP over DTLS drivers.

# Use object dump to print response payload in hex
USEMODULE += od

# Use shell module to accept and parse RIOT shell commands to send requests with configurable
# CoAP request methods.
USEMODULE += shell
USEMODULE += shell_cmds_default
USEMODULE += shell_cmd_ps

# Comment this out to disable code in RIOT that does safety checking
# which is not needed in a production environment but helps in the
# development process:
DEVELHELP ?= 1

# Change this to 0 show compiler invocation lines by default:
QUIET ?= 1

include $(RIOTBASE)/Makefile.include
