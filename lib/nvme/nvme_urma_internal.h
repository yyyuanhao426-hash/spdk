/*   SPDX-License-Identifier: BSD-3-Clause */

#ifndef SPDK_NVME_URMA_INTERNAL_H
#define SPDK_NVME_URMA_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/nvme_urma.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/dma.h"

#include <urma_api.h>
#include <urma_ubagg.h>

#define SPDK_URMA_DEFAULT_JFC_COUNT 2
#define SPDK_URMA_DEFAULT_JFC_DEPTH 4096
#define SPDK_URMA_DEFAULT_JETTY_COUNT 1
#define SPDK_URMA_DEFAULT_JETTY_DEPTH 2048
#define SPDK_URMA_DEFAULT_MAX_SGE 5
#define SPDK_URMA_DEFAULT_PRIORITY 15
#define SPDK_URMA_DEFAULT_RNR_RETRY 7
#define SPDK_URMA_DEFAULT_ERR_TIMEOUT 17
#define SPDK_URMA_DEFAULT_TOKEN 0xACFE

#define SPDK_URMA_WIRE_MAGIC 0x41524d55u /* "URMA", little endian */
#define SPDK_URMA_WIRE_VERSION 1

enum spdk_urma_msg_type {
	SPDK_URMA_MSG_HELLO = 1,
	SPDK_URMA_MSG_HELLO_RSP,
	SPDK_URMA_MSG_CAPSULE_CMD,
	SPDK_URMA_MSG_CAPSULE_RSP,
	SPDK_URMA_MSG_DISCONNECT,
};

struct spdk_urma_msg_hdr {
	uint32_t magic;
	uint16_t version;
	uint16_t type;
	uint32_t length;
	uint32_t qid;
} __attribute__((packed));

struct spdk_urma_endpoint_desc {
	urma_eid_t eid;
	uint32_t jetty_id;
	uint32_t transport_mode;
	uint32_t max_queue_depth;
	uint32_t max_io_size;
} __attribute__((packed));

struct spdk_urma_data_desc {
	urma_seg_t seg;
	uint64_t address;
	uint32_t length;
	uint32_t reserved;
} __attribute__((packed));

struct spdk_urma_capsule_cmd {
	struct spdk_nvme_cmd cmd;
	struct spdk_urma_data_desc data;
} __attribute__((packed));

struct spdk_urma_capsule_rsp {
	struct spdk_nvme_cpl cpl;
} __attribute__((packed));

struct spdk_urma_transport_opts {
	char dev_name[URMA_MAX_NAME];
	uint32_t eid_index;
	int32_t active_port;
	urma_transport_mode_t transport_mode;
	uint32_t jfc_count;
	uint32_t jfc_depth;
	uint32_t jetty_count;
	uint32_t jetty_depth;
	uint32_t max_io_size;
	bool bonding_balance;
	bool bonding_multipath;
};

struct spdk_urma_device {
	urma_context_t *context;
	urma_device_attr_t attr;
	urma_eid_t eid;
	uint32_t eid_index;
	uint8_t active_port;
	struct spdk_urma_transport_opts opts;
	urma_jfc_t **jfcs;
	uint32_t jfc_count;
	/* Modified by Yin: 新增 device 级共享 jfr 字段（UB transport share_jfr 用） */
	urma_jfr_t *jfr;
	struct spdk_memory_domain *memory_domain;
};

void spdk_urma_opts_init(struct spdk_urma_transport_opts *opts);
int spdk_urma_device_open(const struct spdk_urma_transport_opts *opts,
			  struct spdk_urma_device **device);
void spdk_urma_device_close(struct spdk_urma_device *device);
urma_target_seg_t *spdk_urma_memory_region_get_tseg(
	struct spdk_nvme_urma_memory_region *region);

#endif /* SPDK_NVME_URMA_INTERNAL_H */
