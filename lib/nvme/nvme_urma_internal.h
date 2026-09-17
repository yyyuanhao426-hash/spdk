/*   SPDX-License-Identifier: BSD-3-Clause */

#ifndef SPDK_NVME_URMA_INTERNAL_H
#define SPDK_NVME_URMA_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/nvme_urma.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/dma.h"
#include "spdk/env.h"

#include <urma_api.h>
#include <urma_ubagg.h>

#define SPDK_URMA_DEFAULT_JFC_COUNT 2
#define SPDK_URMA_DEFAULT_JFC_DEPTH 4096
#define SPDK_URMA_DEFAULT_JETTY_COUNT 1
#define SPDK_URMA_DEFAULT_JETTY_DEPTH 2048
#define SPDK_URMA_DEFAULT_MAX_SGE 5
#define SPDK_URMA_DEFAULT_RNR_RETRY 7
#define SPDK_URMA_DEFAULT_ERR_TIMEOUT 17
#define SPDK_URMA_DEFAULT_TOKEN 0xACFE

#define SPDK_URMA_WIRE_MAGIC 0x41524d55u /* "URMA", little endian */
#define SPDK_URMA_WIRE_VERSION 3
#define SPDK_URMA_MAX_JETTY_PER_EP 32
#define SPDK_URMA_INVALID_CHIP_ID UINT8_MAX

enum spdk_urma_capsule_transport {
	SPDK_URMA_CAPSULE_TRANSPORT_TCP = 0,
	SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV = 1,
};

enum spdk_urma_msg_type {
	SPDK_URMA_MSG_HELLO = 1,
	SPDK_URMA_MSG_HELLO_RSP,
	SPDK_URMA_MSG_CAPSULE_CMD,
	SPDK_URMA_MSG_CAPSULE_RSP,
	SPDK_URMA_MSG_DISCONNECT,
};

#pragma pack(push, 1)

struct spdk_urma_msg_hdr {
	uint32_t magic;
	uint16_t version;
	uint16_t type;
	uint32_t length;
	uint32_t qid;
};

struct spdk_urma_endpoint_desc {
	urma_eid_t eid;
	uint32_t jetty_count;
	uint32_t jetty_ids[SPDK_URMA_MAX_JETTY_PER_EP];
	uint32_t transport_mode;
	uint32_t tp_type;
	uint32_t max_queue_depth;
	uint32_t max_io_size;
	uint32_t capsule_transport;
};

struct spdk_urma_data_desc {
	urma_seg_t seg;
	uint64_t address;
	uint32_t length;
	uint8_t chip_id;
	uint8_t reserved[3];
};

struct spdk_urma_capsule_cmd {
	struct spdk_nvme_cmd cmd;
	struct spdk_urma_data_desc data;
};

struct spdk_urma_capsule_rsp {
	struct spdk_nvme_cpl cpl;
};

struct spdk_urma_capsule_cmd_frame {
	struct spdk_urma_msg_hdr hdr;
	struct spdk_urma_capsule_cmd capsule;
};

struct spdk_urma_capsule_rsp_frame {
	struct spdk_urma_msg_hdr hdr;
	struct spdk_urma_capsule_rsp capsule;
};

#pragma pack(pop)

struct spdk_urma_transport_opts {
	char dev_name[URMA_MAX_NAME];
	uint32_t eid_index;
	int32_t active_port;
	urma_transport_mode_t transport_mode;
	uint32_t send_jfc_count;
	uint32_t recv_jfc_count;
	uint32_t jfc_depth;
	uint32_t num_jetty_per_ep;
	uint32_t jetty_depth;
	uint32_t max_io_size;
	int32_t priority;
	urma_tp_type_t tp_type;
	enum spdk_urma_capsule_transport capsule_transport;
	bool bonding_balance;
	bool bonding_multipath;
	bool numa_affinity;
};

struct spdk_urma_device {
	urma_context_t *context;
	urma_device_attr_t attr;
	urma_eid_t eid;
	uint32_t eid_index;
	uint8_t active_port;
	struct spdk_urma_transport_opts opts;
	char dev_name[URMA_MAX_NAME];
	urma_jfc_t **send_jfcs;
	uint32_t send_jfc_count;
	urma_jfc_t **recv_jfcs;
	uint32_t recv_jfc_count;
	urma_jfr_t **jfrs;
	uint32_t jfr_count;
	uint32_t next_send_jfc;
	uint32_t next_jfr;
	pthread_mutex_t *send_jfc_poll_locks;
	pthread_mutex_t *recv_jfc_poll_locks;
	uint32_t send_jfc_poll_lock_count;
	uint32_t recv_jfc_poll_lock_count;
	uint32_t priority;
	uint32_t refs;
	TAILQ_ENTRY(spdk_urma_device) link;
	struct spdk_memory_domain *memory_domain;
};

void spdk_urma_opts_init(struct spdk_urma_transport_opts *opts);
int spdk_urma_parse_capsule_transport(const char *value,
				       enum spdk_urma_capsule_transport *transport);
const char *spdk_urma_capsule_transport_name(enum spdk_urma_capsule_transport transport);
int spdk_urma_device_open(const struct spdk_urma_transport_opts *opts,
			  struct spdk_urma_device **device);
void spdk_urma_device_get(struct spdk_urma_device *device);
void spdk_urma_device_close(struct spdk_urma_device *device);
uint32_t spdk_urma_device_next_send_jfc(struct spdk_urma_device *device);
uint32_t spdk_urma_device_next_jfr(struct spdk_urma_device *device);
int spdk_urma_device_poll_send_jfc(struct spdk_urma_device *device, uint32_t index,
				   int max_cr, urma_cr_t *cr);
int spdk_urma_device_poll_recv_jfc(struct spdk_urma_device *device, uint32_t index,
				   int max_cr, urma_cr_t *cr);
uint8_t spdk_urma_memory_chip_id(const void *addr);
urma_target_seg_t *spdk_urma_memory_region_get_tseg(
	struct spdk_nvme_urma_memory_region *region);

/* Modified By Yida(v7): 整池注册表（实现见 nvme_urma_common.c）。由 initiator
 * 预注册缓冲或 target iobuf 整池注册填充；I/O 提交路径用 find() 采纳覆盖本
 * I/O 缓冲的 region，跳过 per-I/O register。按共享 urma_context 键控。 */
int nvme_urma_region_registry_add(void *urma_context, void *addr, size_t length,
				  struct spdk_nvme_urma_memory_region *region);
void nvme_urma_region_registry_remove(struct spdk_nvme_urma_memory_region *region);
struct spdk_nvme_urma_memory_region *
nvme_urma_region_registry_find(void *urma_context, uint64_t addr, size_t length);

#endif /* SPDK_NVME_URMA_INTERNAL_H */
