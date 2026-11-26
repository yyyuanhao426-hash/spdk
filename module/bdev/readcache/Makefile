# SPDX-License-Identifier: BSD-3-Clause
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 1
C_SRCS = readcache.c writeback.c vbdev_readcache.c vbdev_readcache_rpc.c
LIBNAME = bdev_readcache
SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
