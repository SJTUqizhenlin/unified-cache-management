SPDK_ROOT_DIR := /home/qizhenlin/devSPDK/spdk
APP = nvme_iops_test
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

LINK_C += && rm -f $(OBJS) $(OBJS:.o=.d)

all: kernel_iops_test

CLEAN_FILES += kernel_iops_test

kernel_iops_test: kernel_iops_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -laio
