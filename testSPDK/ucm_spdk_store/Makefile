SPDK_ROOT_DIR := /home/qizhenlin/devSPDK/spdk
APP = ucm_spdk_store
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

# The engine now has an optional IPC transport backed by the shared client.
IPC_CLIENT_SRC := ../ucm_spdk_ipc/ucm_spdk_ipc_client.c
IPC_CLIENT_OBJ := ../ucm_spdk_ipc/ucm_spdk_ipc_client.o
C_SRCS += $(IPC_CLIENT_SRC)
CFLAGS += -I../ucm_spdk_ipc

$(IPC_CLIENT_OBJ): $(IPC_CLIENT_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

LINK_C += && rm -f $(OBJS) $(OBJS:.o=.d)

$(APP): $(IPC_CLIENT_OBJ)
