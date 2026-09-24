SPDK_ROOT_DIR := /home/qizhenlin/devSPDK/spdk
APP = nvme_cmd_test
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

LINK_C += && rm -f $(OBJS) $(OBJS:.o=.d)
