SPDK_ROOT_DIR := /home/qizhenlin/devSPDK/spdk
APP = nvme_cache_test
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

LINK_C += && rm -f $(OBJS) $(OBJS:.o=.d)

all: kernel_cache_test

CLEAN_FILES += kernel_cache_test

kernel_cache_test: kernel_cache_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread -laio
