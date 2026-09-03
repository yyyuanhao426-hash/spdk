/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

/* Experimental independent NVMe/URMA initiator transport.  TCP is used only
 * for bootstrap and NVMe capsule exchange; payload bytes move through URMA. */

#include "nvme_internal.h"
#include "nvme_urma_internal.h"

#include "spdk/dma.h"
#include "spdk/env.h"
#include "spdk/nvmf.h"

#include <netdb.h>
#include <sys/ioctl.h>

/* Modified By Yida: rdtsc-style per-phase timing for performance diagnosis */
struct nvme_urma_timing {
	uint64_t reg_ticks;      /* register_memory (cache miss only) */
	uint64_t reg_count;
	uint64_t cache_hit_count;
	uint64_t send_ticks;     /* nvme_urma_write_full (TCP send capsule) */
	uint64_t send_count;
	uint64_t compl_ticks;    /* recv MSG_PEEK + FIONREAD + read_full (wait for completion) */
	uint64_t compl_count;
	uint64_t release_ticks;  /* cache_release or unregister (cache release is near-zero) */
	uint64_t release_count;
	uint64_t total_ticks;    /* submit + completion round-trip */
	uint64_t total_count;
};

static struct nvme_urma_timing g_timing;

static void
nvme_urma_timing_dump(void)
{
	uint64_t hz = spdk_get_ticks_hz();
	uint64_t reg = __atomic_load_n(&g_timing.reg_ticks, __ATOMIC_RELAXED);
	uint64_t reg_n = __atomic_load_n(&g_timing.reg_count, __ATOMIC_RELAXED);
	uint64_t hit = __atomic_load_n(&g_timing.cache_hit_count, __ATOMIC_RELAXED);
	uint64_t send = __atomic_load_n(&g_timing.send_ticks, __ATOMIC_RELAXED);
	uint64_t send_n = __atomic_load_n(&g_timing.send_count, __ATOMIC_RELAXED);
	uint64_t compl = __atomic_load_n(&g_timing.compl_ticks, __ATOMIC_RELAXED);
	uint64_t compl_n = __atomic_load_n(&g_timing.compl_count, __ATOMIC_RELAXED);
	uint64_t rel = __atomic_load_n(&g_timing.release_ticks, __ATOMIC_RELAXED);
	uint64_t rel_n = __atomic_load_n(&g_timing.release_count, __ATOMIC_RELAXED);
	uint64_t tot = __atomic_load_n(&g_timing.total_ticks, __ATOMIC_RELAXED);
	uint64_t tot_n = __atomic_load_n(&g_timing.total_count, __ATOMIC_RELAXED);

	/* Modified By Yida: use printf instead of SPDK_NOTICELOG — urma_perf's
	 * default log level filters NOTICE, but results are printed via printf
	 * which always shows on stdout. */
	printf("==== URMA timing breakdown (hz=%lu) ====\n", hz);
	printf("  register (cache miss): %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       reg, reg_n, reg_n ? reg * 1000000000ULL / (hz * reg_n) : 0, reg * 1000 / hz);
	printf("  cache_hit:             n=%lu\n", hit);
	printf("  send (TCP capsule):    %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       send, send_n, send_n ? send * 1000000000ULL / (hz * send_n) : 0, send * 1000 / hz);
	printf("  completion_wait:       %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       compl, compl_n, compl_n ? compl * 1000000000ULL / (hz * compl_n) : 0, compl * 1000 / hz);
	printf("  release (cache/unreg): %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       rel, rel_n, rel_n ? rel * 1000000000ULL / (hz * rel_n) : 0, rel * 1000 / hz);
	printf("  TOTAL (submit+compl):  %lu ticks, n=%lu, avg=%lu us, total=%lu ms\n",
	       tot, tot_n, tot_n ? tot * 1000000ULL / (hz * tot_n) : 0, tot * 1000 / hz);
	if (tot > 0) {
		printf("  breakdown: reg=%.1f%% send=%.1f%% compl=%.1f%% release=%.1f%%\n",
		       100.0 * reg / tot, 100.0 * send / tot,
		       100.0 * compl / tot, 100.0 * rel / tot);
	}
	fflush(stdout);
}

void
spdk_nvme_urma_dump_timing(void)
{
	nvme_urma_timing_dump();
}

/* Modified By Yida: Memory registration cache (Initiator 侧) */
#define NVME_URMA_REG_CACHE_SIZE 64

struct nvme_urma_reg_entry {
	void *va;
	size_t len;
	struct spdk_nvme_urma_memory_region *region;
	int refcount;   /* 引用计数，>0 表示有在途 I/O 正在使用 */
	bool used;
};

struct nvme_urma_req {
	struct nvme_request *req;
	struct spdk_nvme_urma_memory_region *region;
	/* Modified By Yida: cache entry when region is cached (NULL if not cached) */
	struct nvme_urma_reg_entry *cache_entry; /* NULL if not cached */
	uint64_t submit_tick;   /* 总往返计时起点 */
	uint64_t send_start;    /* send 阶段计时起点 */
	TAILQ_ENTRY(nvme_urma_req) link;
};

struct nvme_urma_qpair {
	struct spdk_nvme_qpair qpair;
	struct spdk_urma_device *device;
	urma_jetty_t *jetty;
	urma_target_jetty_t *target_jetty;
	int fd;
	uint32_t num_entries;
	/* Modified by Yin: 新增 transport-level CID 计数器（submit 时分配唯一 cid） */
	uint16_t next_cid;
	/* Modified By Yida (v3): CID 位图，保证 outstanding 的 cid 绝不重复 */
	uint8_t *cid_bitmap;
	/* Modified By Yida: memory registration cache */
	struct nvme_urma_reg_entry reg_cache[NVME_URMA_REG_CACHE_SIZE];
	TAILQ_HEAD(, nvme_urma_req) outstanding;
};

struct nvme_urma_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_urma_transport_opts opts;
};

struct nvme_urma_poll_group {
	struct spdk_nvme_transport_poll_group group;
};

static inline struct nvme_urma_qpair *
nvme_urma_qpair(struct spdk_nvme_qpair *qpair)
{
	return SPDK_CONTAINEROF(qpair, struct nvme_urma_qpair, qpair);
}

/* Modified By Yida (v3): CID 位图分配器。旧计数器在 fail_outstanding（重置
 * queue_depth=0）或 send 失败路径上与实际在飞请求脱钩后，wrap 可能撞上仍在
 * 飞（outstanding）的 cid，completion 按 cid 匹配会命中错误 request。位图
 * 从 next_cid 起找第一个空闲位，保证任一时刻 outstanding 的 cid 唯一。
 * 位图分配失败时退回旧计数器行为（不阻塞连接建立）。 */
static uint16_t
nvme_urma_cid_alloc(struct nvme_urma_qpair *uqpair)
{
	uint32_t count = uqpair->num_entries;
	uint32_t i;

	if (uqpair->cid_bitmap == NULL) {
		uint16_t cid = uqpair->next_cid++;
		if (uqpair->next_cid >= count) {
			uqpair->next_cid = 0;
		}
		return cid;
	}
	for (i = 0; i < count; i++) {
		uint32_t idx = (uqpair->next_cid + i) % count;
		uint8_t mask = (uint8_t)(1u << (idx & 7));
		if ((uqpair->cid_bitmap[idx >> 3] & mask) == 0) {
			uqpair->cid_bitmap[idx >> 3] |= mask;
			uqpair->next_cid = (uint16_t)((idx + 1) % count);
			return (uint16_t)idx;
		}
	}
	/* 位图满（理论上不会发生：submit 前已检查 queue_depth < num_entries） */
	return (uint16_t)count;
}

static void
nvme_urma_cid_free(struct nvme_urma_qpair *uqpair, uint16_t cid)
{
	if (uqpair->cid_bitmap != NULL && cid < uqpair->num_entries) {
		uqpair->cid_bitmap[cid >> 3] &= (uint8_t)~(1u << (cid & 7));
	}
}

/* Modified By Yida: registration cache helpers */
/* Look up registration cache by (va, len). Returns entry if found, NULL if miss. */
static struct nvme_urma_reg_entry *
nvme_urma_reg_cache_lookup(struct nvme_urma_qpair *uqpair, void *va, size_t len)
{
	for (int i = 0; i < NVME_URMA_REG_CACHE_SIZE; i++) {
		struct nvme_urma_reg_entry *e = &uqpair->reg_cache[i];
		if (e->used && e->va == va && e->len == len) {
			return e;
		}
	}
	return NULL;
}

/* Find a free slot in the registration cache and insert. Returns entry on
 * success, NULL if cache is full (caller falls back to uncached register). */
static struct nvme_urma_reg_entry *
nvme_urma_reg_cache_insert(struct nvme_urma_qpair *uqpair, void *va, size_t len,
			   struct spdk_nvme_urma_memory_region *region)
{
	for (int i = 0; i < NVME_URMA_REG_CACHE_SIZE; i++) {
		struct nvme_urma_reg_entry *e = &uqpair->reg_cache[i];
		if (!e->used) {
			e->va = va;
			e->len = len;
			e->region = region;
			e->refcount = 1;
			e->used = true;
			return e;
		}
	}
	return NULL;
}

/* Release a cache entry's refcount. Does NOT unregister — the region stays
 * cached for future I/O reuse. Actual unregister happens at qpair destroy. */
static void
nvme_urma_reg_cache_release(struct nvme_urma_reg_entry *entry)
{
	if (entry != NULL && entry->refcount > 0) {
		entry->refcount--;
	}
}

static int
nvme_urma_write_full(int fd, const void *buf, size_t length)
{
	const uint8_t *pos = buf;

	while (length != 0) {
		ssize_t rc = send(fd, pos, length, MSG_NOSIGNAL);
		if (rc < 0 && errno == EINTR) {
			continue;
		}
		if (rc <= 0) {
			return rc == 0 ? -ECONNRESET : -errno;
		}
		pos += rc;
		length -= rc;
	}
	return 0;
}

static int
nvme_urma_read_full(int fd, void *buf, size_t length)
{
	uint8_t *pos = buf;

	while (length != 0) {
		ssize_t rc = recv(fd, pos, length, 0);
		if (rc < 0 && errno == EINTR) {
			continue;
		}
		if (rc <= 0) {
			return rc == 0 ? -ECONNRESET : -errno;
		}
		pos += rc;
		length -= rc;
	}
	return 0;
}

static int
nvme_urma_connect_socket(const struct spdk_nvme_transport_id *trid)
{
	struct addrinfo hints = {}, *result = NULL, *it;
	int fd = -1, rc;

	hints.ai_family = trid->adrfam == SPDK_NVMF_ADRFAM_IPV6 ? AF_INET6 : AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(trid->traddr, trid->trsvcid, &hints, &result);
	if (rc != 0) {
		return -EINVAL;
	}
	for (it = result; it != NULL; it = it->ai_next) {
		fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
		if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
			break;
		}
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(result);
	return fd < 0 ? -errno : fd;
}

static int
nvme_urma_create_jetty(struct nvme_urma_qpair *uqpair)
{
	urma_jfs_cfg_t jfs = {};
	urma_jetty_cfg_t cfg = {};

	jfs.depth = uqpair->device->opts.jetty_depth;
	jfs.trans_mode = uqpair->device->opts.transport_mode;
	jfs.priority = SPDK_URMA_DEFAULT_PRIORITY;
	jfs.max_sge = SPDK_URMA_DEFAULT_MAX_SGE;
	jfs.rnr_retry = SPDK_URMA_DEFAULT_RNR_RETRY;
	jfs.err_timeout = SPDK_URMA_DEFAULT_ERR_TIMEOUT;
	jfs.jfc = uqpair->device->jfcs[0];
	/* Modified by Yin: UB transport 强制 share_jfr=1，改用 device 预建的共享 jfr */
	cfg.jfs_cfg = jfs;
	cfg.flag.bs.share_jfr = 1;
	cfg.shared.jfr = uqpair->device->jfr;
	cfg.shared.jfc = uqpair->device->jfcs[0];
	uqpair->jetty = urma_create_jetty(uqpair->device->context, &cfg);
	return uqpair->jetty == NULL ? -EIO : 0;
}

static int
nvme_urma_exchange_hello(struct nvme_urma_qpair *uqpair)
{
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_endpoint_desc local = {}, remote = {};
	urma_rjetty_t rjetty = {};
	int rc;

	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_HELLO;
	hdr.length = sizeof(local);
	hdr.qid = uqpair->qpair.id;
	local.eid = uqpair->device->eid;
	local.jetty_id = uqpair->jetty->jetty_id.id;
	local.transport_mode = uqpair->device->opts.transport_mode;
	local.max_queue_depth = uqpair->num_entries + 1;
	local.max_io_size = uqpair->device->opts.max_io_size;
	rc = nvme_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc == 0) {
		rc = nvme_urma_write_full(uqpair->fd, &local, sizeof(local));
	}
	if (rc == 0) {
		rc = nvme_urma_read_full(uqpair->fd, &hdr, sizeof(hdr));
	}
	if (rc == 0 && (hdr.magic != SPDK_URMA_WIRE_MAGIC ||
			 hdr.version != SPDK_URMA_WIRE_VERSION ||
			 hdr.type != SPDK_URMA_MSG_HELLO_RSP || hdr.length != sizeof(remote))) {
		rc = -EPROTO;
	}
	if (rc == 0) {
		rc = nvme_urma_read_full(uqpair->fd, &remote, sizeof(remote));
	}
	if (rc != 0 || remote.transport_mode != uqpair->device->opts.transport_mode) {
		return rc != 0 ? rc : -EPROTONOSUPPORT;
	}
	if (remote.max_queue_depth < 2 || remote.max_io_size == 0) {
		return -EPROTO;
	}
	uqpair->num_entries = spdk_min(uqpair->num_entries, remote.max_queue_depth - 1);
	uqpair->device->opts.max_io_size = spdk_min(uqpair->device->opts.max_io_size,
					 remote.max_io_size);
	rjetty.jetty_id.eid = remote.eid;
	rjetty.jetty_id.id = remote.jetty_id;
	rjetty.trans_mode = remote.transport_mode;
	rjetty.type = URMA_JETTY;
	rjetty.tp_type = URMA_CTP;
	{
		urma_token_t token = {.token = SPDK_URMA_DEFAULT_TOKEN};
		uqpair->target_jetty = urma_import_jetty(uqpair->device->context, &rjetty, &token);
	}
	if (uqpair->target_jetty == NULL) {
		return -EIO;
	}
	if (uqpair->device->opts.transport_mode == URMA_TM_RC) {
		urma_status_t status = urma_bind_jetty(uqpair->jetty, uqpair->target_jetty);

		if (status != URMA_SUCCESS && status != URMA_EEXIST) {
			urma_unimport_jetty(uqpair->target_jetty);
			uqpair->target_jetty = NULL;
			return -EIO;
		}
	}
	return 0;
}

static enum spdk_nvme_urma_memory_type
nvme_urma_req_memory_type(const struct nvme_request *req)
{
	const char *id;

	if (req->payload.opts == NULL || req->payload.opts->memory_domain == NULL ||
	    req->payload.opts->memory_domain == spdk_memory_domain_get_system_domain()) {
		return SPDK_NVME_URMA_MEM_HOST;
	}
	id = spdk_memory_domain_get_dma_device_id(req->payload.opts->memory_domain);
	if (id != NULL && strcasestr(id, "cuda") != NULL) {
		return SPDK_NVME_URMA_MEM_CUDA;
	}
	if (id != NULL && strcasestr(id, "rocm") != NULL) {
		return SPDK_NVME_URMA_MEM_ROCM;
	}
	if (id != NULL && (strcasestr(id, "npu") != NULL || strcasestr(id, "ascend") != NULL)) {
		return SPDK_NVME_URMA_MEM_NPU;
	}
	return SPDK_NVME_URMA_MEM_XDS;
}

static int
nvme_urma_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	struct nvme_urma_req *ureq;
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_capsule_cmd capsule = {};
	void *addr = NULL;
	int rc;

	if (qpair->queue_depth >= uqpair->num_entries) {
		return -EAGAIN;
	}
	if (req->payload.size != 0 && nvme_req_payload_type(req) != NVME_PAYLOAD_TYPE_CONTIG) {
		return -ENOTSUP;
	}
	ureq = calloc(1, sizeof(*ureq));
	if (ureq == NULL) {
		return -ENOMEM;
	}
	ureq->req = req;
	ureq->submit_tick = spdk_get_ticks();  /* total round-trip start */
	/* Modified by Yin: submit 前分配唯一 cid，防 completion 匹配到错误 request。
	 * Modified By Yida (v3): 改用位图分配器保证 outstanding 的 cid 唯一。 */
	req->cmd.cid = nvme_urma_cid_alloc(uqpair);
	if ((uint32_t)req->cmd.cid >= uqpair->num_entries) {
		free(ureq);
		return -EAGAIN;
	}
	capsule.cmd = req->cmd;
	if (req->payload.size != 0) {
		capsule.cmd.dptr.sgl1.address = 0;
		capsule.cmd.dptr.sgl1.unkeyed.length = req->payload.size;
		capsule.cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
		capsule.cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
		addr = (void *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload.offset);
		/* Modified By Yida: check registration cache before doing a full register */
		uint64_t t_reg0 = spdk_get_ticks();
		struct nvme_urma_reg_entry *entry = nvme_urma_reg_cache_lookup(uqpair, addr,
				req->payload.size);
		if (entry != NULL) {
			ureq->region = entry->region;
			ureq->cache_entry = entry;
			entry->refcount++;
			__atomic_add_fetch(&g_timing.cache_hit_count, 1, __ATOMIC_RELAXED);
		} else {
			rc = spdk_nvme_urma_register_memory(uqpair->device->context, addr,
					req->payload.size, nvme_urma_req_memory_type(req), &ureq->region);
			if (rc != 0) {
				nvme_urma_cid_free(uqpair, req->cmd.cid);
				free(ureq);
				return rc;
			}
			ureq->cache_entry = nvme_urma_reg_cache_insert(uqpair, addr,
					req->payload.size, ureq->region);
			/* If cache_entry is NULL (cache full), region stays uncached and
			 * will be unregistered on completion (old behavior). */
			uint64_t t_reg1 = spdk_get_ticks();
			__atomic_add_fetch(&g_timing.reg_ticks, t_reg1 - t_reg0, __ATOMIC_RELAXED);
			__atomic_add_fetch(&g_timing.reg_count, 1, __ATOMIC_RELAXED);
		}
		capsule.data.seg = spdk_urma_memory_region_get_tseg(ureq->region)->seg;
		capsule.data.address = (uint64_t)addr;
		capsule.data.length = req->payload.size;
	}
	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_CAPSULE_CMD;
	hdr.length = sizeof(capsule);
	hdr.qid = qpair->id;
	ureq->send_start = spdk_get_ticks();  /* send-phase start */
	rc = nvme_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc == 0) {
		rc = nvme_urma_write_full(uqpair->fd, &capsule, sizeof(capsule));
	}
	{
		uint64_t t_send1 = spdk_get_ticks();
		__atomic_add_fetch(&g_timing.send_ticks, t_send1 - ureq->send_start, __ATOMIC_RELAXED);
		__atomic_add_fetch(&g_timing.send_count, 1, __ATOMIC_RELAXED);
	}
	if (rc != 0) {
		/* Modified By Yida: on send failure, release cache refcount if cached */
		if (ureq->cache_entry != NULL) {
			nvme_urma_reg_cache_release(ureq->cache_entry);
		} else {
			spdk_nvme_urma_unregister_memory(ureq->region);
		}
		/* Modified By Yida (v3): 归还 cid 位 */
		nvme_urma_cid_free(uqpair, req->cmd.cid);
		free(ureq);
		return rc;
	}
	TAILQ_INSERT_TAIL(&uqpair->outstanding, ureq, link);
	qpair->queue_depth++;
	return 0;
}

static int32_t
nvme_urma_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	uint32_t completed = 0;

	if (max_completions == 0) {
		max_completions = UINT32_MAX;
	}
	while (completed < max_completions) {
		struct spdk_urma_msg_hdr hdr;
		struct spdk_urma_capsule_rsp rsp;
		struct nvme_urma_req *ureq;
		uint64_t t_compl0 = spdk_get_ticks();  /* completion-wait start */
		ssize_t rc = recv(uqpair->fd, &hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}
		if (rc == 0) {
			return -ECONNRESET;
		}
		if (rc < 0) {
			return -errno;
		}
		if ((size_t)rc < sizeof(hdr)) {
			break;
		}
		{
			int available = 0;

			if (ioctl(uqpair->fd, FIONREAD, &available) != 0) {
				return -errno;
			}
			if ((size_t)available < sizeof(hdr) + hdr.length) {
				break;
			}
		}
		if (nvme_urma_read_full(uqpair->fd, &hdr, sizeof(hdr)) != 0 ||
		    hdr.magic != SPDK_URMA_WIRE_MAGIC || hdr.type != SPDK_URMA_MSG_CAPSULE_RSP ||
		    hdr.length != sizeof(rsp) || nvme_urma_read_full(uqpair->fd, &rsp, sizeof(rsp)) != 0) {
			return -EPROTO;
		}
		uint64_t t_compl1 = spdk_get_ticks();
		__atomic_add_fetch(&g_timing.compl_ticks, t_compl1 - t_compl0, __ATOMIC_RELAXED);
		__atomic_add_fetch(&g_timing.compl_count, 1, __ATOMIC_RELAXED);
		TAILQ_FOREACH(ureq, &uqpair->outstanding, link) {
			if (ureq->req->cmd.cid == rsp.cpl.cid) {
				break;
			}
		}
		if (ureq == NULL) {
			/* Modified by Yin: 诊断用：completion 的 cid 无匹配 outstanding request */
			SPDK_ERRLOG("process_completions: no matching ureq for cid=%u\n",
				    rsp.cpl.cid);
			return -EPROTO;
		}
		TAILQ_REMOVE(&uqpair->outstanding, ureq, link);
		qpair->queue_depth--;
		/* Modified By Yida (v3): 归还 cid 位 */
		nvme_urma_cid_free(uqpair, ureq->req->cmd.cid);
		/* Modified By Yida: release cache refcount instead of unregister */
		uint64_t t_rel0 = spdk_get_ticks();
		if (ureq->cache_entry != NULL) {
			nvme_urma_reg_cache_release(ureq->cache_entry);
		} else {
			spdk_nvme_urma_unregister_memory(ureq->region);
		}
		uint64_t t_rel1 = spdk_get_ticks();
		__atomic_add_fetch(&g_timing.release_ticks, t_rel1 - t_rel0, __ATOMIC_RELAXED);
		__atomic_add_fetch(&g_timing.release_count, 1, __ATOMIC_RELAXED);
		/* total round-trip: submit_tick → now */
		uint64_t t_total1 = spdk_get_ticks();
		__atomic_add_fetch(&g_timing.total_ticks, t_total1 - ureq->submit_tick, __ATOMIC_RELAXED);
		__atomic_add_fetch(&g_timing.total_count, 1, __ATOMIC_RELAXED);
		nvme_complete_request(ureq->req->cb_fn, ureq->req->cb_arg, qpair, ureq->req, &rsp.cpl);
		free(ureq);
		completed++;
	}
	if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
		/* Modified by Yin: 加 in_connect_poll 守卫，切断 connect_poll 互递归 */
		if (qpair->in_connect_poll) {
			return completed;
		}
		qpair->in_connect_poll = true;
		int rc = nvme_fabric_qpair_connect_poll(qpair);
		qpair->in_connect_poll = false;
		if (rc == 0) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (rc != -EAGAIN) {
			return rc;
		}
	}
	return completed;
}

static void
nvme_urma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	struct nvme_urma_req *ureq, *tmp;
	struct spdk_nvme_cpl cpl = {};

	cpl.sqid = qpair->id;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.dnr = dnr;
	TAILQ_FOREACH_SAFE(ureq, &uqpair->outstanding, link, tmp) {
		TAILQ_REMOVE(&uqpair->outstanding, ureq, link);
		/* Modified By Yida (v3): 归还 cid 位 */
		nvme_urma_cid_free(uqpair, ureq->req->cmd.cid);
		/* Modified By Yida: release cache refcount, unregister only uncached */
		if (ureq->cache_entry != NULL) {
			nvme_urma_reg_cache_release(ureq->cache_entry);
		} else {
			spdk_nvme_urma_unregister_memory(ureq->region);
		}
		nvme_complete_request(ureq->req->cb_fn, ureq->req->cb_arg, qpair, ureq->req, &cpl);
		free(ureq);
	}
	qpair->queue_depth = 0;
}

static struct spdk_nvme_qpair *
nvme_urma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid, uint32_t qsize,
			     enum spdk_nvme_qprio qprio, uint32_t requests, bool async)
{
	struct nvme_urma_qpair *uqpair = calloc(1, sizeof(*uqpair));

	if (uqpair == NULL) {
		return NULL;
	}
	uqpair->fd = -1;
	uqpair->num_entries = qsize - 1;
	/* Modified By Yida (v3): CID 位图按创建时 num_entries 分配；connect 阶段
	 * 只会向下收窄 num_entries，位图始终覆盖所有可能的 cid。 */
	if (uqpair->num_entries > 0) {
		uqpair->cid_bitmap = calloc((uqpair->num_entries + 7) / 8, 1);
		if (uqpair->cid_bitmap == NULL) {
			free(uqpair);
			return NULL;
		}
	}
	TAILQ_INIT(&uqpair->outstanding);
	if (nvme_qpair_init(&uqpair->qpair, qid, ctrlr, qprio, requests, async) != 0) {
		free(uqpair->cid_bitmap);
		free(uqpair);
		return NULL;
	}
	return &uqpair->qpair;
}

static struct spdk_nvme_qpair *
nvme_urma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_urma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests, opts->async_mode);
}

static void
nvme_urma_qpair_release_transport(struct nvme_urma_qpair *uqpair)
{
	if (uqpair->target_jetty != NULL) {
		if (uqpair->device != NULL && uqpair->device->opts.transport_mode == URMA_TM_RC) {
			urma_unbind_jetty(uqpair->jetty);
		}
		urma_unimport_jetty(uqpair->target_jetty);
		uqpair->target_jetty = NULL;
	}
	if (uqpair->jetty != NULL) {
		urma_delete_jetty(uqpair->jetty);
		uqpair->jetty = NULL;
	}
	if (uqpair->fd >= 0) {
		close(uqpair->fd);
	}
	uqpair->fd = -1;
	/* Modified By Yida: unregister all cached memory regions before closing the device */
	for (int i = 0; i < NVME_URMA_REG_CACHE_SIZE; i++) {
		struct nvme_urma_reg_entry *e = &uqpair->reg_cache[i];
		if (e->used) {
			spdk_nvme_urma_unregister_memory(e->region);
			e->used = false;
			e->refcount = 0;
			e->region = NULL;
		}
	}
	/* Modified By Yida (v3): 释放 CID 位图 */
	free(uqpair->cid_bitmap);
	uqpair->cid_bitmap = NULL;
	spdk_urma_device_close(uqpair->device);
	uqpair->device = NULL;
}

static int
nvme_urma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_urma_ctrlr *uctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_urma_ctrlr, ctrlr);
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	int rc;

	rc = spdk_urma_device_open(&uctrlr->opts, &uqpair->device);
	if (rc != 0) {
		return rc;
	}
	rc = nvme_urma_create_jetty(uqpair);
	if (rc != 0) {
		goto fail;
	}
	uqpair->fd = nvme_urma_connect_socket(&ctrlr->trid);
	if (uqpair->fd < 0) {
		rc = uqpair->fd;
		goto fail;
	}
	rc = nvme_urma_exchange_hello(uqpair);
	if (rc != 0) {
		goto fail;
	}
	uctrlr->opts.max_io_size = spdk_min(uctrlr->opts.max_io_size,
					 uqpair->device->opts.max_io_size);
	rc = nvme_fabric_qpair_connect_async(qpair, uqpair->num_entries + 1);
	if (rc < 0) {
		goto fail;
	}
	return 0;

fail:
	nvme_urma_qpair_release_transport(uqpair);
	return rc;
}

static void
nvme_urma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);

	nvme_urma_qpair_abort_reqs(qpair, qpair->abort_dnr);
	if (uqpair->fd >= 0) {
		close(uqpair->fd);
		uqpair->fd = -1;
	}
	nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
	nvme_transport_ctrlr_disconnect_qpair_done(qpair);
}

static int
nvme_urma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);

	nvme_urma_qpair_abort_reqs(qpair, qpair->abort_dnr);
	nvme_urma_qpair_release_transport(uqpair);
	nvme_qpair_deinit(qpair);
	free(uqpair);
	return 0;
}

static struct spdk_nvme_ctrlr *
nvme_urma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts, void *devhandle)
{
	struct nvme_urma_ctrlr *uctrlr = calloc(1, sizeof(*uctrlr));

	if (uctrlr == NULL) {
		return NULL;
	}
	uctrlr->ctrlr.opts = *opts;
	uctrlr->ctrlr.trid = *trid;
	spdk_urma_opts_init(&uctrlr->opts);
	if (nvme_ctrlr_construct(&uctrlr->ctrlr) != 0) {
		free(uctrlr);
		return NULL;
	}
	uctrlr->ctrlr.adminq = nvme_urma_ctrlr_create_qpair(&uctrlr->ctrlr, 0,
				       opts->admin_queue_size, 0, opts->admin_queue_size, true);
	if (uctrlr->ctrlr.adminq == NULL) {
		nvme_ctrlr_destruct_finish(&uctrlr->ctrlr);
		free(uctrlr);
		return NULL;
	}
	if (nvme_ctrlr_add_process(&uctrlr->ctrlr, 0) != 0) {
		nvme_urma_ctrlr_delete_io_qpair(&uctrlr->ctrlr, uctrlr->ctrlr.adminq);
		uctrlr->ctrlr.adminq = NULL;
		nvme_ctrlr_destruct_finish(&uctrlr->ctrlr);
		free(uctrlr);
		return NULL;
	}
	return &uctrlr->ctrlr;
}

static int
nvme_urma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_urma_ctrlr *uctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_urma_ctrlr, ctrlr);

	if (ctrlr->adminq != NULL) {
		nvme_urma_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}
	nvme_ctrlr_destruct_finish(ctrlr);
	free(uctrlr);
	return 0;
}

static int nvme_urma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr) { return 0; }
static int nvme_urma_qpair_reset(struct spdk_nvme_qpair *qpair) { return 0; }
static uint32_t nvme_urma_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return SPDK_CONTAINEROF(ctrlr, struct nvme_urma_ctrlr, ctrlr)->opts.max_io_size;
}
static uint16_t nvme_urma_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr) { return 1; }

static int
nvme_urma_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				 int (*iter_fn)(struct nvme_request *, void *), void *arg)
{
	struct nvme_urma_req *ureq;
	TAILQ_FOREACH(ureq, &nvme_urma_qpair(qpair)->outstanding, link) {
		int rc = iter_fn(ureq->req, arg);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int nvme_urma_qpair_authenticate(struct spdk_nvme_qpair *qpair) { return -ENOTSUP; }
static void nvme_urma_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	nvme_urma_qpair_abort_reqs(qpair, 0);
}

static struct spdk_nvme_transport_poll_group *
nvme_urma_poll_group_create(void)
{
	return calloc(1, sizeof(struct nvme_urma_poll_group));
}

static int64_t
nvme_urma_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair;
	int64_t total = 0;
	STAILQ_FOREACH(qpair, &tgroup->connected_qpairs, poll_group_stailq) {
		int rc = nvme_urma_qpair_process_completions(qpair, completions_per_qpair);
		if (rc < 0) {
			return rc;
		}
		total += rc;
	}
	return total;
}

static void
nvme_urma_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb cb)
{
	struct spdk_nvme_qpair *qpair;
	STAILQ_FOREACH(qpair, &tgroup->disconnected_qpairs, poll_group_stailq) {
		cb(qpair, tgroup->group->ctx);
	}
}

static int nvme_urma_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	free(tgroup);
	return 0;
}

static int
nvme_urma_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	if (domains != NULL && array_size > 0) {
		const struct nvme_urma_qpair *uqpair = nvme_urma_qpair(ctrlr->adminq);
		domains[0] = uqpair->device != NULL ? uqpair->device->memory_domain :
			     spdk_memory_domain_get_system_domain();
	}
	return 1;
}

const struct spdk_nvme_transport_ops urma_ops = {
	.name = "URMA",
	.type = SPDK_NVME_TRANSPORT_URMA,
	.ctrlr_construct = nvme_urma_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_urma_ctrlr_destruct,
	.ctrlr_enable = nvme_urma_ctrlr_enable,
	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,
	.ctrlr_get_max_xfer_size = nvme_urma_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_urma_ctrlr_get_max_sges,
	.ctrlr_create_io_qpair = nvme_urma_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_urma_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_urma_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_urma_ctrlr_disconnect_qpair,
	.ctrlr_get_memory_domains = nvme_urma_ctrlr_get_memory_domains,
	.qpair_abort_reqs = nvme_urma_qpair_abort_reqs,
	.qpair_reset = nvme_urma_qpair_reset,
	.qpair_submit_request = nvme_urma_qpair_submit_request,
	.qpair_process_completions = nvme_urma_qpair_process_completions,
	.qpair_iterate_requests = nvme_urma_qpair_iterate_requests,
	.qpair_authenticate = nvme_urma_qpair_authenticate,
	.admin_qpair_abort_aers = nvme_urma_admin_qpair_abort_aers,
	.poll_group_create = nvme_urma_poll_group_create,
	.poll_group_process_completions = nvme_urma_poll_group_process_completions,
	.poll_group_check_disconnected_qpairs = nvme_urma_poll_group_check_disconnected_qpairs,
	.poll_group_destroy = nvme_urma_poll_group_destroy,
};

SPDK_NVME_TRANSPORT_REGISTER(urma, &urma_ops);
SPDK_LOG_REGISTER_COMPONENT(nvme_urma)
