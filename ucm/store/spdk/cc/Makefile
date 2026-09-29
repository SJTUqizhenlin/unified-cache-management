# Build the SPDK store engine as a static library for the UCM .so wrapper.
# The engine is the same code as the standalone test binary (ucm_spdk_store.c)
# but with main() compiled out (ENGINE_LIB=1).

SPDK_ROOT_DIR ?= /home/qizhenlin/devSPDK/spdk
UCM_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
UCM_SPDK_DIR := $(UCM_ROOT)/testSPDK/ucm_spdk_store

# Source: the prototype's C file, with main() guarded
ENGINE_SRC := $(UCM_SPDK_DIR)/ucm_spdk_store.c
SHIM_SRC := engine_shim.c
ENGINE_OBJ := ucm_spdk_engine.o
SHIM_OBJ := engine_shim.o
ENGINE_LIB := libucm_spdk_engine.a

SPDK_CFLAGS := -I$(SPDK_ROOT_DIR)/include \
	       -I$(SPDK_ROOT_DIR)/lib/nvme -I$(SPDK_ROOT_DIR)/lib/env_dpdk \
	       -DENGINE_LIB=1 -fPIC -O2 -g -Wall -Wextra

all: $(ENGINE_LIB)

$(ENGINE_OBJ): $(ENGINE_SRC)
	$(CC) $(SPDK_CFLAGS) -c -o $@ $<

$(SHIM_OBJ): $(SHIM_SRC)
	$(CC) -fPIC -O2 -c -o $@ $<

$(ENGINE_LIB): $(ENGINE_OBJ) $(SHIM_OBJ)
	$(AR) rcs $@ $^

clean:
	rm -f $(ENGINE_OBJ) $(SHIM_OBJ) $(ENGINE_LIB)

.PHONY: all clean
