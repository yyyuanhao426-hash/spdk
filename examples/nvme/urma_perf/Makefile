#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Huawei Technologies Co., Ltd.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

APP = urma_perf

# Modified By NDS: NPU (AscendCL) support lives in its own translation unit
# so the CUDA path stays untouched.
C_SRCS-y += urma_perf_npu.c

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

SYS_LIBS += -ldl
