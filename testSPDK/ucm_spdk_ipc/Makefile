# Build the local IPC target against the SPDK source/build tree, plus the
# worker-side IPC client, module test, and hardware-free unit tests.
SPDK_ROOT_DIR ?= /home/qizhenlin/devSPDK/spdk

APP = ucm_spdk_ipc_target
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

CLEAN_FILES += ucm_spdk_ipc_test ipc_protocol_test ipc_client_test ipc_target_test \
	       ucm_spdk_ipc_client.o ucm_spdk_ipc_module_test.o

all: ucm_spdk_ipc_test ipc_protocol_test ipc_client_test ipc_target_test

ucm_spdk_ipc_client.o: ucm_spdk_ipc_client.c ucm_spdk_ipc_client.h ipc_protocol.h
	$(CC) -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE -I. -c -o $@ ucm_spdk_ipc_client.c

ucm_spdk_ipc_test: ucm_spdk_ipc_module_test.o ucm_spdk_ipc_client.o
	$(CC) -O2 -g -o $@ ucm_spdk_ipc_module_test.o ucm_spdk_ipc_client.o -pthread

ucm_spdk_ipc_module_test.o: ucm_spdk_ipc_module_test.c ucm_spdk_ipc_client.h ipc_protocol.h
	$(CC) -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE -I. -c -o $@ ucm_spdk_ipc_module_test.c

ipc_protocol_test: ipc_protocol_test.c ipc_protocol.h
	$(CC) -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE -I. -o $@ ipc_protocol_test.c

ipc_client_test: ipc_client_test.c ucm_spdk_ipc_client.c ucm_spdk_ipc_client.h ipc_protocol.h
	$(CC) -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE -I. -o $@ \
		ipc_client_test.c ucm_spdk_ipc_client.c -pthread

ipc_target_test: ipc_target_test.c ucm_spdk_ipc_target.c ipc_protocol.h
	$(CC) -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE -I. \
		-I$(SPDK_ROOT_DIR)/include -ffunction-sections -fdata-sections \
		-o $@ ipc_target_test.c -Wl,--gc-sections -pthread

test: ipc_protocol_test ipc_client_test ipc_target_test
	./ipc_protocol_test
	./ipc_client_test
	./ipc_target_test

.PHONY: test
