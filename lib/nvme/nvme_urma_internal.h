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
#define SPDK_URMA_RX_QUIESCE_BUDGET 4096

#define SPDK_URMA_WIRE_MAGIC 0x41524d55u /* "URMA", little endian */
#define SPDK_URMA_WIRE_VERSION 6
#define SPDK_URMA_MAX_RJETTY_SIZE (16 * 1024)
#define SPDK_URMA_MAX_SEG_CTX_SIZE 4096
#define SPDK_URMA_MAX_JETTY_PER_EP 32
#define SPDK_URMA_INVALID_CHIP_ID UINT8_MAX

/* End-to-end correlation for Admin requests (Identify, Get/Set Features,
 * Fabrics Connect/Property Get/Set, ...).  Admin/Fabrics commands are rare and
 * must always be diagnosable; I/O commands are only logged when the initiator
 * explicitly requested a stage trace, so this does not add a per-I/O log flood.
 * Keeping the gate in one place makes target and initiator log the same set of
 * commands, which is what makes a mount failure correlatable across both logs. */
#define SPDK_URMA_DIAG_ADMIN(qid, opc, trace_flags) \
	((qid) == 0 || (opc) == SPDK_NVME_OPC_FABRIC || \
	 ((trace_flags) & SPDK_URMA_STAGE_TRACE_REQUESTED) != 0)

/* fctype lives in the Fabrics command layout; for Admin opcodes it is not
 * meaningful, so report 0 rather than misread cdw0 bits. */
#define SPDK_URMA_DIAG_FCTYPE(opc, cmd) \
	((opc) == SPDK_NVME_OPC_FABRIC ? \
	 ((const struct spdk_nvmf_capsule_cmd *)(cmd))->fctype : 0)

enum spdk_urma_capsule_transport {
	SPDK_URMA_CAPSULE_TRANSPORT_TCP = 0,
	SPDK_URMA_CAPSULE_TRANSPORT_URMA = 1,
};

enum spdk_urma_msg_type {
	SPDK_URMA_MSG_HELLO = 1,
	SPDK_URMA_MSG_HELLO_RSP,
	SPDK_URMA_MSG_CAPSULE_CMD,
	SPDK_URMA_MSG_CAPSULE_RSP,
	SPDK_URMA_MSG_DISCONNECT,
};

enum spdk_urma_stage_trace_flags {
	SPDK_URMA_STAGE_TRACE_REQUESTED = 1u << 0,
	SPDK_URMA_STAGE_TRACE_COMPLETE = 1u << 1,
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
	/* Complete provider contexts follow this descriptor, in jetty order. */
	uint32_t rjetty_lengths[SPDK_URMA_MAX_JETTY_PER_EP];
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
	uint64_t trace_seq;
	uint32_t trace_flags;
	uint32_t reserved;
	uint32_t seg_ctx_length;
	uint8_t seg_ctx[SPDK_URMA_MAX_SEG_CTX_SIZE];
};

/* Target-local ticks returned with the completion.  The timestamps are only
 * populated when the initiator requests stage tracing.  Durations within this
 * structure use target_ticks_hz and never require cross-host clock sync. */
struct spdk_urma_stage_trace {
	uint64_t trace_seq;
	uint64_t target_ticks_hz;
	uint64_t rx_tick;
	uint64_t buffer_tick;
	uint64_t exec_tick;
	uint64_t bdev_cpl_tick;
	uint64_t data_post_tick;
	uint64_t data_cpl_tick;
	uint32_t target_lcore;
	uint32_t flags;
};

struct spdk_urma_capsule_rsp {
	struct spdk_nvme_cpl cpl;
	struct spdk_urma_stage_trace stage;
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

#define SPDK_URMA_CMD_BASE_SIZE offsetof(struct spdk_urma_capsule_cmd, seg_ctx)
#define SPDK_URMA_CMD_BASE_FRAME_SIZE (sizeof(struct spdk_urma_msg_hdr) + SPDK_URMA_CMD_BASE_SIZE)

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
	bool electrical_network;
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

/* HELLO owns the provider bytes until every peer jetty has been imported. */
int spdk_urma_hello_data_length(const struct spdk_urma_endpoint_desc *desc,
			      uint32_t frame_length, size_t *data_length);
int spdk_urma_export_rjettys(struct spdk_urma_device *device, urma_jetty_t **jettys,
			   struct spdk_urma_endpoint_desc *desc, uint8_t **data);
int spdk_urma_import_rjetty(struct spdk_urma_device *device,
			  const struct spdk_urma_endpoint_desc *desc, uint32_t index,
			  const void *data, urma_target_jetty_t **target);

int spdk_urma_validate_cmd(const struct spdk_urma_msg_hdr *hdr,
			   const struct spdk_urma_capsule_cmd *cmd);
int spdk_urma_import_segment(struct spdk_urma_device *device,
			     const struct spdk_urma_data_desc *data, const void *context,
			     uint32_t context_length, urma_target_seg_t **target,
			     void **retained_context);
void spdk_urma_bonding_chip_ids(bool electrical, uint8_t local, uint8_t remote, uint32_t *src,
				uint32_t *dst);

#ifdef SPDK_CONFIG_URMA_LOG_FUNC
/* Install the liburma log sink once per process, before urma_init().
 * Also callable from the registration harness to test level mapping. */
void spdk_urma_log_redirect_init(void);
#endif

typedef bool (*spdk_urma_recv_cr_prepare_cb)(urma_cr_t *cr, void *ctx);

/* BALANCE+PORT needs the JFS multi_path bit too (Mooncake TE policy).
 * The existing bonding_multipath option still selects IODIE in device_open;
 * keep that level choice separate from enabling multipath on each jetty. */
static inline bool
spdk_urma_device_uses_multipath(const struct spdk_urma_device *device)
{
	return device->opts.bonding_multipath ||
	       (device->opts.bonding_balance && strncmp(device->dev_name, "bonding", 7) == 0);
}

/* UMDK constructs these queue-state notification CRs without a valid
 * user_ctx.  They must be consumed before any transport context lookup. */
static inline bool
spdk_urma_cr_is_fake(const urma_cr_t *cr)
{
	return cr->status == URMA_CR_WR_SUSPEND_DONE ||
	       cr->status == URMA_CR_WR_FLUSH_ERR_DONE;
}

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
				   int max_cr, urma_cr_t *cr,
				   spdk_urma_recv_cr_prepare_cb prepare_cb, void *prepare_ctx);
int spdk_urma_device_poll_recv_jfc_locked(struct spdk_urma_device *device, uint32_t index,
					  int max_cr, urma_cr_t *cr,
					  spdk_urma_recv_cr_prepare_cb prepare_cb, void *prepare_ctx);
void spdk_urma_device_lock_recv_jfcs(struct spdk_urma_device *device);
void spdk_urma_device_unlock_recv_jfcs(struct spdk_urma_device *device);
uint8_t spdk_urma_memory_chip_id(const void *addr);
/* Capsule slot arrays own complete host/grant pages. Register the logical
 * count * slot_size bytes; free() only after unregister and WR/CR drain.
 * Unlike DMA/iobuf allocations these are ordinary zeroed host allocations. */
void *spdk_urma_alloc_capsule_slots(size_t count, size_t slot_size);
urma_target_seg_t *spdk_urma_memory_region_get_tseg(
	struct spdk_nvme_urma_memory_region *region);

/* Modified By Yida(v7): 整池注册表（实现见 nvme_urma_common.c）。由 initiator
 * 预注册缓冲或 target iobuf 整池注册填充；I/O 提交路径用 find() 采纳覆盖本
 * I/O 缓冲的 region，跳过 per-I/O register。按共享 urma_context 键控。 */
int spdk_nvme_urma_region_registry_add(void *urma_context, void *addr, size_t length,
				  struct spdk_nvme_urma_memory_region *region);
/* Acquire one owner reference to an exact whole-pool registration. */
int spdk_urma_register_memory_pool(void *urma_context, void *addr, size_t length,
			       enum spdk_nvme_urma_memory_type type,
			       struct spdk_nvme_urma_memory_region **region);
void spdk_nvme_urma_region_registry_remove(struct spdk_nvme_urma_memory_region *region);
struct spdk_nvme_urma_memory_region *
spdk_nvme_urma_region_registry_find(void *urma_context, uint64_t addr, size_t length);

#endif /* SPDK_NVME_URMA_INTERNAL_H */
