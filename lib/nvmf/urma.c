/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

/* Experimental independent NVMe/URMA target transport.  TCP carries endpoint
 * descriptors and, when configured, NVMe capsules. The alternate capsule path
 * uses URMA SEND/RECV; payload bytes always use URMA READ/WRITE. */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/json.h"

#include "nvmf_internal.h"
#include "transport.h"
#include "../nvme/nvme_urma_internal.h"

#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

/* Modified By Yida: Target-side memory registration cache */
#define NVMF_URMA_REG_CACHE_SIZE 128

/* Modified By Yida(v7): 远端段 import 缓存。capsule 每条 I/O 携带 initiator 的
 * 内存段描述 (urma_seg_t)，同一段反复 import/unimport 是纯开销：按 seg 字节
 * 缓存 import 结果（urma_target_seg_t），命中则本 I/O 完全不调 import，release
 * 也不 unimport。配合 initiator 整池注册（spdk_nvme_urma_register_memory_for_qpair
 * + urma_perf URMA_PERF_REGION_REG=1），capsule 每条 I/O 携带同一全区 seg →
 * 每个共享 context 只保留不同 seg 的一次 import，所有 endpoint 复用同一 UMMU 映射。
 * 约束：initiator 在 transport 存活期间不得 unregister 已缓存的 seg。
 * SPDK_URMA_IMPORT_CACHE=0 可关闭。 */
#define NVMF_URMA_IMPORT_BUCKETS 256

struct nvmf_urma_import_entry {
	urma_seg_t seg;
	urma_target_seg_t *tseg;
	LIST_ENTRY(nvmf_urma_import_entry) link;
};
LIST_HEAD(nvmf_urma_import_bucket, nvmf_urma_import_entry);
/* Modified By Yida(v4): max capsules parsed per poll round. Bounded so JFC
 * completions for in-flight data still get polled promptly on bursts. */
#define NVMF_URMA_CAPSULE_BATCH 64

struct nvmf_urma_reg_entry {
	void *va;
	size_t len;
	struct spdk_nvme_urma_memory_region *region;
	bool used;
};

/* Modified By Yida(v3): target-side staged latency instrumentation, mirrors
 * lib/nvme/nvme_urma.c g_timing (tick accumulators + printf dump). Stages:
 * W4a parse capsule / W4b iobuf alloc / W5 import_seg / W6 register_memory /
 * W7 post WR / W8 JFC wait (pull) / W9 exec->completion (bdev/SSD) /
 * C2H push JFC wait / W10 send_response / release (unreg+unimport). */
struct nvmf_urma_tgt_timing {
	uint64_t capsule_ticks;  /* W4a: read_full hdr+capsule */
	uint64_t capsule_n;
	uint64_t buffer_ticks;   /* W4b: capsule parsed -> buffers ready */
	uint64_t buffer_n;
	uint64_t import_ticks;   /* W5: urma_import_seg (cache miss only) */
	uint64_t import_n;
	uint64_t import_hits;    /* Modified By Yida(v7): W5 import 缓存命中数 */
	uint64_t reg_ticks;      /* W6: register_memory (cache miss only) */
	uint64_t reg_misses;
	uint64_t reg_hits;
	uint64_t post_ticks;     /* W7: urma_post_jetty_send_wr */
	uint64_t post_n;
	uint64_t jfc_ticks;      /* W8: WR posted -> JFC completion (incl. poller latency) */
	uint64_t jfc_n;
	uint64_t exec_ticks;     /* W9: spdk_nvmf_request_exec -> req_complete */
	uint64_t exec_n;
	uint64_t push_ticks;     /* C2H: push WR posted -> JFC completion */
	uint64_t push_n;
	uint64_t rsp_ticks;      /* W10: response TCP write or inline SEND post */
	uint64_t rsp_n;
	uint64_t release_ticks;  /* unregister (uncached) + urma_unimport_seg */
	uint64_t release_n;
	uint64_t total_ticks;    /* capsule parsed -> rsp written (target service time) */
	uint64_t total_n;
	/* Modified By Yida(v4): W4p — capsule queueing before parse. peek = first
	 * poll round whose MSG_PEEK saw the hdr; parse = t_parse0. Covers rcvbuf
	 * residency + poller interval. partial_n counts rounds where the hdr was
	 * visible but the capsule body was still in flight (FIONREAD gate bounced). */
	uint64_t peek_wait_ticks;
	uint64_t peek_wait_n;
	uint64_t partial_n;
};

static struct nvmf_urma_tgt_timing g_tgt_timing;

#define NVMF_URMA_TGT_ADD(field, val) \
	__atomic_add_fetch(&g_tgt_timing.field, (uint64_t)(val), __ATOMIC_RELAXED)
#define NVMF_URMA_TGT_INC(field) \
	__atomic_add_fetch(&g_tgt_timing.field, 1, __ATOMIC_RELAXED)
#define NVMF_URMA_TGT_STAGE(field, ticks) do { \
	NVMF_URMA_TGT_ADD(field ## _ticks, (ticks)); \
	NVMF_URMA_TGT_INC(field ## _n); \
} while (0)

/* Modified By Yida(v4): per-I/O receive trace for cross-machine correlation.
 * In TCP mode, peek is the first observation of the header and parse means the
 * complete capsule is read. In SEND/RECV mode both are the receive completion
 * processing tick. rsp is the response write/post return tick. Joined with the
 * initiator's send records by (qid, cid); comparing hosts requires clock sync.
 * Ring keeps the most recent N records; outstanding requests are far below N
 * so a wrapped record can never belong to an in-flight request. */
struct nvmf_urma_rx_trace {
	uint64_t peek_tick;
	uint64_t parse_tick;
	uint64_t rsp_tick;
	uint32_t length;
	uint16_t qid;
	uint16_t cid;
	uint8_t opcode;
	uint8_t xfer;
};

#define NVMF_URMA_RX_TRACE_SIZE 8192
static struct nvmf_urma_rx_trace g_rx_trace[NVMF_URMA_RX_TRACE_SIZE];
static uint64_t g_rx_trace_idx;

static void
nvmf_urma_timing_dump(void)
{
	uint64_t hz = spdk_get_ticks_hz();
	uint64_t capsule = __atomic_load_n(&g_tgt_timing.capsule_ticks, __ATOMIC_RELAXED);
	uint64_t cap_n = __atomic_load_n(&g_tgt_timing.capsule_n, __ATOMIC_RELAXED);
	uint64_t buffer = __atomic_load_n(&g_tgt_timing.buffer_ticks, __ATOMIC_RELAXED);
	uint64_t buf_n = __atomic_load_n(&g_tgt_timing.buffer_n, __ATOMIC_RELAXED);
	uint64_t imp = __atomic_load_n(&g_tgt_timing.import_ticks, __ATOMIC_RELAXED);
	uint64_t imp_n = __atomic_load_n(&g_tgt_timing.import_n, __ATOMIC_RELAXED);
	uint64_t reg = __atomic_load_n(&g_tgt_timing.reg_ticks, __ATOMIC_RELAXED);
	uint64_t miss = __atomic_load_n(&g_tgt_timing.reg_misses, __ATOMIC_RELAXED);
	uint64_t hit = __atomic_load_n(&g_tgt_timing.reg_hits, __ATOMIC_RELAXED);
	uint64_t post = __atomic_load_n(&g_tgt_timing.post_ticks, __ATOMIC_RELAXED);
	uint64_t post_n = __atomic_load_n(&g_tgt_timing.post_n, __ATOMIC_RELAXED);
	uint64_t jfc = __atomic_load_n(&g_tgt_timing.jfc_ticks, __ATOMIC_RELAXED);
	uint64_t jfc_n = __atomic_load_n(&g_tgt_timing.jfc_n, __ATOMIC_RELAXED);
	uint64_t exec = __atomic_load_n(&g_tgt_timing.exec_ticks, __ATOMIC_RELAXED);
	uint64_t exec_n = __atomic_load_n(&g_tgt_timing.exec_n, __ATOMIC_RELAXED);
	uint64_t push = __atomic_load_n(&g_tgt_timing.push_ticks, __ATOMIC_RELAXED);
	uint64_t push_n = __atomic_load_n(&g_tgt_timing.push_n, __ATOMIC_RELAXED);
	uint64_t rsp = __atomic_load_n(&g_tgt_timing.rsp_ticks, __ATOMIC_RELAXED);
	uint64_t rsp_n = __atomic_load_n(&g_tgt_timing.rsp_n, __ATOMIC_RELAXED);
	uint64_t rel = __atomic_load_n(&g_tgt_timing.release_ticks, __ATOMIC_RELAXED);
	uint64_t rel_n = __atomic_load_n(&g_tgt_timing.release_n, __ATOMIC_RELAXED);
	uint64_t tot = __atomic_load_n(&g_tgt_timing.total_ticks, __ATOMIC_RELAXED);
	uint64_t tot_n = __atomic_load_n(&g_tgt_timing.total_n, __ATOMIC_RELAXED);
	/* Modified By Yida(v4): W4p queueing before parse */
	uint64_t peek_wait = __atomic_load_n(&g_tgt_timing.peek_wait_ticks, __ATOMIC_RELAXED);
	uint64_t peek_n = __atomic_load_n(&g_tgt_timing.peek_wait_n, __ATOMIC_RELAXED);
	uint64_t partial = __atomic_load_n(&g_tgt_timing.partial_n, __ATOMIC_RELAXED);

	/* printf instead of SPDK_NOTICELOG, same reason as the initiator dump:
	 * nvmf_tgt's default log level filters NOTICE, printf always shows. */
	/* Modified By Yida: avg 一律先除 n 再乘系数，避免 ticks×1e9 溢出 uint64
	 * （64K/128K 的 W9 累计 2.3e10~1.1e11 ticks，旧式先乘后除打印出错的 avg） */
	printf("==== URMA target timing breakdown (hz=%lu) ====\n", hz);
	printf("  W4p peek->parse:     %lu ticks, n=%lu, avg=%lu ns (rcvbuf/poll queueing)\n",
	       peek_wait, peek_n, peek_n ? peek_wait / peek_n * 1000000000ULL / hz : 0);
	printf("  capsule splits (hdr seen, body pending): n=%lu\n", partial);
	printf("  W4a parse capsule:   %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       capsule, cap_n, cap_n ? capsule / cap_n * 1000000000ULL / hz : 0,
	       capsule * 1000 / hz);
	printf("  W4b iobuf alloc:     %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       buffer, buf_n, buf_n ? buffer / buf_n * 1000000000ULL / hz : 0,
	       buffer * 1000 / hz);
	printf("  W5 import_seg:       %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       imp, imp_n, imp_n ? imp / imp_n * 1000000000ULL / hz : 0,
	       imp * 1000 / hz);
	printf("  W5 cache_hit:        n=%lu, miss=%lu\n",
	       __atomic_load_n(&g_tgt_timing.import_hits, __ATOMIC_RELAXED), imp_n);
	printf("  W6 register (miss):  %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       reg, miss, miss ? reg / miss * 1000000000ULL / hz : 0,
	       reg * 1000 / hz);
	printf("  W6 cache_hit:        n=%lu, miss=%lu\n", hit, miss);
	printf("  W7 post WR:          %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       post, post_n, post_n ? post / post_n * 1000000000ULL / hz : 0,
	       post * 1000 / hz);
	printf("  W8 JFC wait (pull):  %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       jfc, jfc_n, jfc_n ? jfc / jfc_n * 1000000000ULL / hz : 0,
	       jfc * 1000 / hz);
	printf("  W9 exec->cpl (SSD):  %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       exec, exec_n, exec_n ? exec / exec_n * 1000000000ULL / hz : 0,
	       exec * 1000 / hz);
	printf("  push JFC wait (C2H): %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       push, push_n, push_n ? push / push_n * 1000000000ULL / hz : 0,
	       push * 1000 / hz);
	printf("  W10 send rsp:        %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       rsp, rsp_n, rsp_n ? rsp / rsp_n * 1000000000ULL / hz : 0,
	       rsp * 1000 / hz);
	printf("  release (unreg+unimport): %lu ticks, n=%lu, avg=%lu ns, total=%lu ms\n",
	       rel, rel_n, rel_n ? rel / rel_n * 1000000000ULL / hz : 0,
	       rel * 1000 / hz);
	printf("  TOTAL (parse->rsp):  %lu ticks, n=%lu, avg=%lu us, total=%lu ms\n",
	       tot, tot_n, tot_n ? tot / tot_n * 1000000ULL / hz : 0,
	       tot * 1000 / hz);
	if (tot > 0) {
		printf("  breakdown: parse=%.1f%% buf=%.1f%% import=%.1f%% reg=%.1f%% post=%.1f%% jfc=%.1f%% exec=%.1f%% push=%.1f%% rsp=%.1f%% release=%.1f%%\n",
		       100.0 * capsule / tot, 100.0 * buffer / tot, 100.0 * imp / tot,
		       100.0 * reg / tot, 100.0 * post / tot, 100.0 * jfc / tot,
		       100.0 * exec / tot, 100.0 * push / tot, 100.0 * rsp / tot,
		       100.0 * rel / tot);
	}
	fflush(stdout);
}

static int
nvmf_urma_timing_dump_poller(void *ctx)
{
	nvmf_urma_timing_dump();
	return SPDK_POLLER_IDLE;
}

/* Modified By Yida(v6): per-I/O rx trace CSV 默认关闭（每轮最多 8192 行，刷屏）；
 * SPDK_URMA_TRACE=1 时才采集并 dump，聚合 breakdown 不受影响。 */
static int
nvmf_urma_trace_enabled(void)
{
	static int enabled = -1;

	if (enabled == -1) {
		const char *v = getenv("SPDK_URMA_TRACE");

		enabled = (v != NULL && v[0] == '1');
	}
	return enabled;
}

/* Modified By Yida(v4): dump the per-I/O receive trace as CSV. Called only at
 * round end (new connection resets) and transport destroy — never from the
 * periodic poller dump, which would flood the log. */
static void
nvmf_urma_rx_trace_dump(void)
{
	uint64_t total = __atomic_load_n(&g_rx_trace_idx, __ATOMIC_RELAXED);
	uint64_t n = total < NVMF_URMA_RX_TRACE_SIZE ? total : NVMF_URMA_RX_TRACE_SIZE;
	uint64_t start = total - n;

	if (!nvmf_urma_trace_enabled() || n == 0) {
		return;
	}
	printf("==== URMA rx trace (most recent %lu of %lu; peek/parse/rsp are target-local TSC) ====\n",
	       n, total);
	printf("seq,qid,cid,opcode,length,peek_tick,parse_tick,rsp_tick,peek_to_parse_ticks\n");
	for (uint64_t i = 0; i < n; i++) {
		const struct nvmf_urma_rx_trace *rec = &g_rx_trace[(start + i) % NVMF_URMA_RX_TRACE_SIZE];

		printf("%lu,%u,%u,%u,%u,%lu,%lu,%lu,%lu\n",
		       start + i, rec->qid, rec->cid, rec->opcode, rec->length,
		       rec->peek_tick, rec->parse_tick, rec->rsp_tick,
		       rec->parse_tick - rec->peek_tick);
	}
	fflush(stdout);
}

/* Modified By Yida(v3): target 是长驻进程，计数自启动起一直累加，多轮 urma_perf
 * （不同 iosize）的数据会混在一起没法按轮分析。每接受一条新连接（新一轮
 * urma_perf 开始）时，先把上一轮的最终累计 dump 出来再清零——日志里每轮独立成块。
 * urma_perf 的一轮会连续建 1 admin + N IO 条连接，工作 I/O 在全部连接就绪后才开始，
 * 所以 burst 内的多次 reset 都发生在测量之前，最后那次生效，setup 噪音也被清掉。 */
static void
nvmf_urma_timing_reset(void)
{
	if (__atomic_load_n(&g_tgt_timing.total_n, __ATOMIC_RELAXED) > 0) {
		printf("---- 新连接进入：以下为上一轮（自上次清零以来）的最终计时 ----\n");
		nvmf_urma_timing_dump();
	}
	/* Modified By Yida(v4): per-round per-I/O trace, cleared with the aggregates */
	nvmf_urma_rx_trace_dump();
	__atomic_store_n(&g_rx_trace_idx, 0, __ATOMIC_RELAXED);
	memset(&g_tgt_timing, 0, sizeof(g_tgt_timing));
}

enum nvmf_urma_req_state {
	NVMF_URMA_REQ_FREE = 0,
	NVMF_URMA_REQ_NEED_BUFFER,
	NVMF_URMA_REQ_PULLING,
	NVMF_URMA_REQ_EXECUTING,
	NVMF_URMA_REQ_PUSHING,
};

struct nvmf_urma_transport;
struct nvmf_urma_poll_group;
struct nvmf_urma_qpair;

enum nvmf_urma_cqe_type {
	NVMF_URMA_CQE_DATA = 1,
	NVMF_URMA_CQE_CAPSULE_RX,
	NVMF_URMA_CQE_CAPSULE_TX,
};

struct nvmf_urma_cqe_ctx {
	enum nvmf_urma_cqe_type type;
	void *owner;
};

struct nvmf_urma_cmd_rx_slot {
	struct nvmf_urma_cqe_ctx cqe;
	struct nvmf_urma_qpair *qpair;
	struct spdk_urma_capsule_cmd_frame frame;
	urma_sge_t sge;
	urma_jfr_wr_t wr;
};

struct nvmf_urma_req {
	struct nvmf_urma_cqe_ctx cqe;
	struct spdk_nvmf_request req;
	union nvmf_h2c_msg cmd;
	union nvmf_c2h_msg rsp;
	struct spdk_urma_data_desc remote_data;
	urma_target_seg_t *remote_seg;
	/* remote_seg 保活于 transport 级 import 哈希表时，release 不得 unimport。 */
	bool remote_seg_cached;
	struct spdk_nvme_urma_memory_region *local_region;
	bool local_region_external;
	/* Modified By Yida: cache entry when local_region is cached (NULL if uncached) */
	struct nvmf_urma_reg_entry *cache_entry; /* NULL if uncached */
	enum nvmf_urma_req_state state;
	/* Modified By Yida(v3): staged latency instrumentation */
	uint64_t start_tick;   /* capsule parsed; 0 = not a timed data request */
	uint64_t post_tick;    /* data WR posted (JFC wait start) */
	uint64_t exec_tick;    /* spdk_nvmf_request_exec called; 0 = not timed */
	/* Modified By Yida(v4): index into g_rx_trace for this I/O's per-I/O record */
	uint32_t trace_idx;
	TAILQ_ENTRY(nvmf_urma_req) link;
};

struct nvmf_urma_qpair {
	struct spdk_nvmf_qpair qpair;
	struct nvmf_urma_poll_group *group;
	struct spdk_thread *thread;
	struct nvmf_urma_transport *transport;
	struct spdk_urma_device *device;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	uint32_t jetty_count;
	uint32_t next_jetty;
	uint32_t send_jfc_index;
	int fd;
	char peer_addr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	char local_addr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	char service[SPDK_NVMF_TRSVCID_MAX_LEN + 1];
	uint32_t resource_count;
	uint32_t max_io_size;
	enum spdk_urma_capsule_transport capsule_transport;
	struct nvmf_urma_cqe_ctx capsule_tx_cqe;
	struct nvmf_urma_cmd_rx_slot *capsule_rx_slots;
	struct spdk_nvme_urma_memory_region *capsule_rx_region;
	uint32_t capsule_rx_count;
	struct nvmf_urma_req *reqs;
	/* Modified By Yida(v4): tick of the poll round that first MSG_PEEK'd the
	 * hdr currently at the head of rcvbuf; 0 = none. Kept across rounds while
	 * the capsule body is still in flight so peek->parse covers real queueing. */
	uint64_t pending_peek_tick;
	/* Modified By Yida: target-side registration cache */
	struct nvmf_urma_reg_entry reg_cache[NVMF_URMA_REG_CACHE_SIZE];
	/* Remote segment imports are shared in the transport-level hash table. */
	TAILQ_HEAD(, nvmf_urma_req) free_reqs;
	TAILQ_HEAD(, nvmf_urma_req) working_reqs;
	TAILQ_ENTRY(nvmf_urma_qpair) link;
	spdk_nvmf_transport_qpair_fini_cb fini_cb;
	void *fini_arg;
};

struct nvmf_urma_port {
	struct spdk_nvme_transport_id trid;
	int fd;
	TAILQ_ENTRY(nvmf_urma_port) link;
};

struct nvmf_urma_poll_group {
	struct spdk_nvmf_transport_poll_group group;
	TAILQ_HEAD(, nvmf_urma_qpair) qpairs;
	uint32_t worker_index;
	TAILQ_ENTRY(nvmf_urma_poll_group) link;
};

struct nvmf_urma_pool_region {
	struct spdk_nvme_urma_memory_region *region;
	TAILQ_ENTRY(nvmf_urma_pool_region) link;
};

struct nvmf_urma_transport {
	struct spdk_nvmf_transport transport;
	struct spdk_urma_transport_opts urma_opts;
	struct spdk_urma_device *device;
	struct spdk_poller *accept_poller;
	/* Modified By Yida(v3): optional periodic staged-latency dump */
	struct spdk_poller *dump_poller;
	TAILQ_HEAD(, nvmf_urma_port) ports;
	TAILQ_HEAD(, nvmf_urma_poll_group) poll_groups;
	TAILQ_HEAD(, nvmf_urma_pool_region) pool_regions;
	struct nvmf_urma_import_bucket import_cache[NVMF_URMA_IMPORT_BUCKETS];
	pthread_rwlock_t import_cache_lock;
	uint32_t next_worker_index;
	uint32_t worker_count;
};

struct nvmf_urma_json_opts {
	char *dev_name;
	char *trans_mode;
	char *capsule_transport;
	int32_t active_port;
	uint32_t eid_index;
	uint32_t jfc_count;
	uint32_t send_jfc_count;
	uint32_t recv_jfc_count;
	uint32_t jfc_depth;
	uint32_t jetty_count;
	uint32_t num_jetty_per_ep;
	uint32_t jetty_depth;
	int32_t priority;
	char *tp_type;
	bool bonding_balance;
	bool bonding_multipath;
	bool numa_affinity;
};

static const struct spdk_json_object_decoder g_urma_opts_decoder[] = {
	{"dev_name", offsetof(struct nvmf_urma_json_opts, dev_name), spdk_json_decode_string, true},
	{"trans_mode", offsetof(struct nvmf_urma_json_opts, trans_mode), spdk_json_decode_string, true},
	{"capsule_transport", offsetof(struct nvmf_urma_json_opts, capsule_transport),
	 spdk_json_decode_string, true},
	{"active_port", offsetof(struct nvmf_urma_json_opts, active_port), spdk_json_decode_int32, true},
	{"eid_index", offsetof(struct nvmf_urma_json_opts, eid_index), spdk_json_decode_uint32, true},
	{"jfc_count", offsetof(struct nvmf_urma_json_opts, jfc_count), spdk_json_decode_uint32, true},
	{"send_jfc_count", offsetof(struct nvmf_urma_json_opts, send_jfc_count), spdk_json_decode_uint32, true},
	{"recv_jfc_count", offsetof(struct nvmf_urma_json_opts, recv_jfc_count), spdk_json_decode_uint32, true},
	{"jfc_depth", offsetof(struct nvmf_urma_json_opts, jfc_depth), spdk_json_decode_uint32, true},
	{"jetty_count", offsetof(struct nvmf_urma_json_opts, jetty_count), spdk_json_decode_uint32, true},
	{"num_jetty_per_ep", offsetof(struct nvmf_urma_json_opts, num_jetty_per_ep), spdk_json_decode_uint32, true},
	{"jetty_depth", offsetof(struct nvmf_urma_json_opts, jetty_depth), spdk_json_decode_uint32, true},
	{"priority", offsetof(struct nvmf_urma_json_opts, priority), spdk_json_decode_int32, true},
	{"tp_type", offsetof(struct nvmf_urma_json_opts, tp_type), spdk_json_decode_string, true},
	{"bonding_balance", offsetof(struct nvmf_urma_json_opts, bonding_balance), spdk_json_decode_bool, true},
	{"bonding_multipath", offsetof(struct nvmf_urma_json_opts, bonding_multipath), spdk_json_decode_bool, true},
	{"numa_affinity", offsetof(struct nvmf_urma_json_opts, numa_affinity), spdk_json_decode_bool, true},
};

static inline struct nvmf_urma_qpair *
nvmf_urma_qpair(struct spdk_nvmf_qpair *qpair)
{
	return SPDK_CONTAINEROF(qpair, struct nvmf_urma_qpair, qpair);
}

static inline struct nvmf_urma_req *
nvmf_urma_req(struct spdk_nvmf_request *req)
{
	return SPDK_CONTAINEROF(req, struct nvmf_urma_req, req);
}

static inline uint32_t
nvmf_urma_next_jetty(struct nvmf_urma_qpair *uqpair)
{
	return uqpair->next_jetty++ % uqpair->jetty_count;
}

static uint32_t
nvmf_urma_seg_hash(const urma_seg_t *seg)
{
	const uint8_t *p = (const uint8_t *)seg;
	uint32_t hash = 2166136261u;

	for (size_t i = 0; i < sizeof(*seg); i++) {
		hash = (hash ^ p[i]) * 16777619u;
	}
	return hash % NVMF_URMA_IMPORT_BUCKETS;
}

static struct nvmf_urma_import_entry *
nvmf_urma_import_cache_find(struct nvmf_urma_transport *transport, const urma_seg_t *seg)
{
	struct nvmf_urma_import_entry *entry;
	uint32_t bucket = nvmf_urma_seg_hash(seg);

	LIST_FOREACH(entry, &transport->import_cache[bucket], link) {
		if (memcmp(&entry->seg, seg, sizeof(*seg)) == 0) {
			return entry;
		}
	}
	return NULL;
}

static int
nvmf_urma_write_full(int fd, const void *buf, size_t length)
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
nvmf_urma_read_full(int fd, void *buf, size_t length)
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
nvmf_urma_create_jetty(struct nvmf_urma_qpair *uqpair)
{
	struct spdk_urma_device *device = uqpair->device;
	urma_jfs_cfg_t jfs = {};
	uint32_t capsule_inline_size = sizeof(struct spdk_urma_capsule_cmd_frame);
	uint32_t i;

	uqpair->jetty_count = spdk_min(device->opts.num_jetty_per_ep,
				      (uint32_t)SPDK_URMA_MAX_JETTY_PER_EP);
	if (uqpair->jetty_count == 0) {
		uqpair->jetty_count = 1;
	}
	uqpair->jettys = calloc(uqpair->jetty_count, sizeof(*uqpair->jettys));
	uqpair->target_jettys = calloc(uqpair->jetty_count, sizeof(*uqpair->target_jettys));
	if (uqpair->jettys == NULL || uqpair->target_jettys == NULL) {
		return -ENOMEM;
	}
	uqpair->send_jfc_index = spdk_urma_device_next_send_jfc(device);
	jfs.depth = device->opts.jetty_depth;
	jfs.trans_mode = device->opts.transport_mode;
	jfs.priority = device->priority;
	jfs.max_sge = SPDK_URMA_DEFAULT_MAX_SGE;
	jfs.flag.bs.multi_path = device->opts.bonding_multipath ? 1 : 0;
	if (device->opts.capsule_transport == SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV) {
		if (device->attr.dev_cap.max_jfs_inline_len < capsule_inline_size) {
			SPDK_ERRLOG("URMA capsule SEND requires %u inline bytes, device supports %u\n",
				    capsule_inline_size, device->attr.dev_cap.max_jfs_inline_len);
			return -EMSGSIZE;
		}
		jfs.max_inline_data = capsule_inline_size;
	}
	jfs.rnr_retry = SPDK_URMA_DEFAULT_RNR_RETRY;
	jfs.err_timeout = SPDK_URMA_DEFAULT_ERR_TIMEOUT;
	jfs.jfc = device->send_jfcs[uqpair->send_jfc_index];
	for (i = 0; i < uqpair->jetty_count; i++) {
		urma_jetty_cfg_t cfg = {};
		uint32_t jfr_index = spdk_urma_device_next_jfr(device);

		cfg.jfs_cfg = jfs;
		cfg.flag.bs.share_jfr = 1;
		cfg.shared.jfr = device->jfrs[jfr_index];
		cfg.shared.jfc = device->recv_jfcs[jfr_index];
		uqpair->jettys[i] = urma_create_jetty(device->context, &cfg);
		if (uqpair->jettys[i] == NULL) {
			while (i > 0) {
				urma_delete_jetty(uqpair->jettys[--i]);
			}
			free(uqpair->jettys);
			free(uqpair->target_jettys);
			uqpair->jettys = NULL;
			uqpair->target_jettys = NULL;
			return -EIO;
		}
	}
	return 0;
}

static int
nvmf_urma_post_cmd_receive(struct nvmf_urma_cmd_rx_slot *slot)
{
	urma_jfr_wr_t *bad_wr = NULL;

	return urma_post_jetty_recv_wr(slot->qpair->jettys[
		       (uint32_t)(slot - slot->qpair->capsule_rx_slots) % slot->qpair->jetty_count],
		       &slot->wr, &bad_wr) ==
	       URMA_SUCCESS ? 0 : -EIO;
}

static int
nvmf_urma_capsule_resources_init(struct nvmf_urma_qpair *uqpair)
{
	urma_target_seg_t *tseg;
	int rc;

	if (uqpair->capsule_transport != SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV) {
		return 0;
	}
	uqpair->capsule_rx_count = uqpair->qpair.sq_head_max + 1;
	if (uqpair->capsule_rx_count == 0 ||
	    (uint64_t)uqpair->capsule_rx_count >
	    (uint64_t)uqpair->device->jfrs[0]->jfr_cfg.depth *
	    spdk_min(uqpair->jetty_count, uqpair->device->jfr_count)) {
		return -EINVAL;
	}
	uqpair->capsule_rx_slots = calloc(uqpair->capsule_rx_count,
					  sizeof(*uqpair->capsule_rx_slots));
	if (uqpair->capsule_rx_slots == NULL) {
		return -ENOMEM;
	}
	rc = spdk_nvme_urma_register_memory(uqpair->device->context,
			uqpair->capsule_rx_slots,
			uqpair->capsule_rx_count * sizeof(*uqpair->capsule_rx_slots),
			SPDK_NVME_URMA_MEM_HOST, &uqpair->capsule_rx_region);
	if (rc != 0) {
		return rc;
	}
	tseg = spdk_urma_memory_region_get_tseg(uqpair->capsule_rx_region);
	for (uint32_t i = 0; i < uqpair->capsule_rx_count; i++) {
		struct nvmf_urma_cmd_rx_slot *slot = &uqpair->capsule_rx_slots[i];

		slot->cqe.type = NVMF_URMA_CQE_CAPSULE_RX;
		slot->cqe.owner = slot;
		slot->qpair = uqpair;
		slot->sge.addr = (uint64_t)&slot->frame;
		slot->sge.len = sizeof(slot->frame);
		slot->sge.tseg = tseg;
		slot->wr.src.sge = &slot->sge;
		slot->wr.src.num_sge = 1;
		slot->wr.user_ctx = (uint64_t)&slot->cqe;
		if (nvmf_urma_post_cmd_receive(slot) != 0) {
			return -EIO;
		}
	}
	return 0;
}

static void
nvmf_urma_capsule_resources_fini(struct nvmf_urma_qpair *uqpair)
{
	spdk_nvme_urma_unregister_memory(uqpair->capsule_rx_region);
	uqpair->capsule_rx_region = NULL;
	free(uqpair->capsule_rx_slots);
	uqpair->capsule_rx_slots = NULL;
	uqpair->capsule_rx_count = 0;
}

static int
nvmf_urma_handshake(struct nvmf_urma_qpair *uqpair)
{
	struct spdk_urma_device *device = uqpair->device;
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_endpoint_desc local = {}, remote = {};
	urma_rjetty_t rjetty = {};
	uint32_t max_queue_depth;
	int rc;

	rc = nvmf_urma_read_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc != 0 || hdr.magic != SPDK_URMA_WIRE_MAGIC ||
	    hdr.version != SPDK_URMA_WIRE_VERSION || hdr.type != SPDK_URMA_MSG_HELLO ||
	    hdr.length != sizeof(remote)) {
		return rc != 0 ? rc : -EPROTO;
	}
	if (nvmf_urma_read_full(uqpair->fd, &remote, sizeof(remote)) != 0 ||
	    remote.transport_mode != device->opts.transport_mode ||
	    remote.tp_type != device->opts.tp_type) {
		SPDK_ERRLOG("URMA handshake transport mismatch: local mode=%u tp=%u, "
			    "remote mode=%u tp=%u\n",
			    (unsigned)device->opts.transport_mode, (unsigned)device->opts.tp_type,
			    remote.transport_mode, remote.tp_type);
		return -EPROTONOSUPPORT;
	}
	if (remote.max_queue_depth < 2 || remote.max_io_size == 0 ||
	    remote.jetty_count == 0 || remote.jetty_count != uqpair->jetty_count ||
	    remote.jetty_count > SPDK_URMA_MAX_JETTY_PER_EP) {
		return -EPROTO;
	}
	max_queue_depth = spdk_min(remote.max_queue_depth,
				   spdk_min(uqpair->transport->transport.opts.max_queue_depth,
					    (uint32_t)spdk_min(
						    (uint64_t)device->jfrs[0]->jfr_cfg.depth *
						    spdk_min(uqpair->jetty_count, device->jfr_count),
						    (uint64_t)UINT32_MAX)));
	if (max_queue_depth < 2) {
		return -EINVAL;
	}
	uqpair->qpair.qid = hdr.qid;
	uqpair->qpair.sq_head_max = max_queue_depth - 1;
	uqpair->max_io_size = spdk_min(uqpair->transport->urma_opts.max_io_size,
				      remote.max_io_size);
	if (remote.capsule_transport != device->opts.capsule_transport) {
		SPDK_ERRLOG("URMA handshake capsule mode mismatch: local=%s remote=%s\n",
			    spdk_urma_capsule_transport_name(device->opts.capsule_transport),
			    spdk_urma_capsule_transport_name(remote.capsule_transport));
		return -EPROTONOSUPPORT;
	}
	uqpair->capsule_transport = device->opts.capsule_transport;
	for (uint32_t i = 0; i < uqpair->jetty_count; i++) {
		urma_token_t token = {.token = SPDK_URMA_DEFAULT_TOKEN};
		urma_status_t status;

		memset(&rjetty, 0, sizeof(rjetty));
		rjetty.jetty_id.eid = remote.eid;
		rjetty.jetty_id.id = remote.jetty_ids[i];
		rjetty.trans_mode = remote.transport_mode;
		rjetty.type = URMA_JETTY;
		rjetty.tp_type = remote.tp_type;
		uqpair->target_jettys[i] = urma_import_jetty(device->context, &rjetty, &token);
		if (uqpair->target_jettys[i] == NULL) {
			return -EIO;
		}
		if (device->opts.transport_mode != URMA_TM_RC) {
			continue;
		}
		status = urma_bind_jetty(uqpair->jettys[i], uqpair->target_jettys[i]);
		if (status != URMA_SUCCESS && status != URMA_EEXIST) {
			return -EIO;
		}
	}
	hdr.type = SPDK_URMA_MSG_HELLO_RSP;
	hdr.length = sizeof(local);
	local.eid = device->eid;
	local.jetty_count = uqpair->jetty_count;
	for (uint32_t i = 0; i < uqpair->jetty_count; i++) {
		local.jetty_ids[i] = uqpair->jettys[i]->jetty_id.id;
	}
	local.transport_mode = device->opts.transport_mode;
	local.tp_type = device->opts.tp_type;
	local.max_queue_depth = max_queue_depth;
	local.max_io_size = uqpair->transport->transport.opts.max_io_size;
	local.capsule_transport = uqpair->capsule_transport;
	rc = nvmf_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	return rc == 0 ? nvmf_urma_write_full(uqpair->fd, &local, sizeof(local)) : rc;
}

static int
nvmf_urma_get_socket_addresses(int fd, struct nvmf_urma_qpair *uqpair)
{
	struct sockaddr_storage peer = {}, local = {};
	socklen_t peer_len = sizeof(peer), local_len = sizeof(local);
	char service[NI_MAXSERV];

	if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0 ||
	    getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
	    getnameinfo((struct sockaddr *)&peer, peer_len, uqpair->peer_addr,
			sizeof(uqpair->peer_addr), NULL, 0, NI_NUMERICHOST) != 0 ||
	    getnameinfo((struct sockaddr *)&local, local_len, uqpair->local_addr,
			sizeof(uqpair->local_addr), service, sizeof(service),
			NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
		return -errno;
	}
	snprintf(uqpair->service, sizeof(uqpair->service), "%s", service);
	return 0;
}

static int
nvmf_urma_accept(void *arg)
{
	struct nvmf_urma_transport *transport = arg;
	struct nvmf_urma_port *port;
	int accepted = 0;

	TAILQ_FOREACH(port, &transport->ports, link) {
		while (accepted < 16) {
			struct nvmf_urma_qpair *uqpair;
			int fd = accept4(port->fd, NULL, NULL, SOCK_CLOEXEC);
			if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				break;
			}
			if (fd < 0) {
				break;
			}
			/* Keep the bootstrap and optional TCP capsule path low latency. */
			{
				int flag = 1;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
			}
			uqpair = calloc(1, sizeof(*uqpair));
			if (uqpair == NULL) {
				close(fd);
				break;
			}
			uqpair->fd = fd;
			uqpair->transport = transport;
			uqpair->qpair.transport = &transport->transport;
			uqpair->qpair.state = SPDK_NVMF_QPAIR_UNINITIALIZED;
			uqpair->capsule_tx_cqe.type = NVMF_URMA_CQE_CAPSULE_TX;
			uqpair->capsule_tx_cqe.owner = uqpair;
			TAILQ_INIT(&uqpair->free_reqs);
			TAILQ_INIT(&uqpair->working_reqs);
			/* Modified by Yin: 拆分 accept 复合条件为独立 rc，逐阶段打错误日志便于定位 */
			uqpair->device = transport->device;
			spdk_urma_device_get(uqpair->device);
			int rc_addr = nvmf_urma_get_socket_addresses(fd, uqpair);
			int rc_jetty = rc_addr ? -1 : nvmf_urma_create_jetty(uqpair);
			int rc_hs = rc_jetty ? -1 : nvmf_urma_handshake(uqpair);
			if (rc_addr != 0) {
				SPDK_ERRLOG("accept: get_socket_addresses failed rc=%d\n", rc_addr);
			} else if (rc_jetty != 0) {
				SPDK_ERRLOG("accept: create_jetty failed rc=%d\n", rc_jetty);
			} else if (rc_hs != 0) {
				SPDK_ERRLOG("accept: handshake failed rc=%d\n", rc_hs);
			}
			if (rc_addr != 0 || rc_jetty != 0 || rc_hs != 0) {
				for (uint32_t i = 0; i < uqpair->jetty_count; i++) {
					if (uqpair->target_jettys != NULL && uqpair->target_jettys[i] != NULL) {
						if (uqpair->device->opts.transport_mode == URMA_TM_RC &&
						    uqpair->jettys != NULL && uqpair->jettys[i] != NULL) {
							urma_unbind_jetty(uqpair->jettys[i]);
						}
						urma_unimport_jetty(uqpair->target_jettys[i]);
					}
					if (uqpair->jettys != NULL && uqpair->jettys[i] != NULL) {
						urma_delete_jetty(uqpair->jettys[i]);
					}
				}
				free(uqpair->target_jettys);
				free(uqpair->jettys);
				nvmf_urma_capsule_resources_fini(uqpair);
				spdk_urma_device_close(uqpair->device);
				close(fd);
				free(uqpair);
				continue;
			}
			/* Modified By Yida(v3): 每条新连接视作新一轮测量的开始 */
			nvmf_urma_timing_reset();
			spdk_nvmf_tgt_new_qpair(transport->transport.tgt, &uqpair->qpair);
			accepted++;
		}
	}
	return accepted != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_urma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth = 128;
	opts->max_qpairs_per_ctrlr = 128;
	opts->in_capsule_data_size = 0;
	opts->max_io_size = 131072;
	opts->io_unit_size = 131072;
	opts->max_aq_depth = 128;
	opts->iobuf_small_cache_size = UINT32_MAX;
	opts->iobuf_large_cache_size = UINT32_MAX;
	opts->abort_timeout_sec = 1;
	opts->transport_specific = NULL;
}

static int
nvmf_urma_register_iobuf_chunk(void *cb_arg, void *addr, size_t length, int32_t numa_id)
{
	struct nvmf_urma_transport *transport = cb_arg;
	struct nvmf_urma_pool_region *entry;
	int rc;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return -ENOMEM;
	}
	rc = spdk_nvme_urma_register_memory(transport->device->context, addr, length,
					    SPDK_NVME_URMA_MEM_HOST, &entry->region);
	if (rc != 0) {
		free(entry);
		return rc;
	}
	rc = nvme_urma_region_registry_add(transport->device->context, addr, length, entry->region);
	if (rc != 0) {
		spdk_nvme_urma_unregister_memory(entry->region);
		free(entry);
		return rc;
	}
	TAILQ_INSERT_TAIL(&transport->pool_regions, entry, link);
	SPDK_NOTICELOG("URMA registered iobuf pool chunk addr=%p len=%zu numa=%d\n",
		       addr, length, numa_id);
	return 0;
}

static void
nvmf_urma_unregister_iobuf_pool(struct nvmf_urma_transport *transport)
{
	struct nvmf_urma_pool_region *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &transport->pool_regions, link, tmp) {
		TAILQ_REMOVE(&transport->pool_regions, entry, link);
		spdk_nvme_urma_unregister_memory(entry->region);
		free(entry);
	}
}

static void
nvmf_urma_import_cache_fini(struct nvmf_urma_transport *transport)
{
	for (uint32_t i = 0; i < NVMF_URMA_IMPORT_BUCKETS; i++) {
		struct nvmf_urma_import_entry *entry, *tmp;

		LIST_FOREACH_SAFE(entry, &transport->import_cache[i], link, tmp) {
			LIST_REMOVE(entry, link);
			urma_unimport_seg(entry->tseg);
			free(entry);
		}
	}
}

static struct spdk_nvmf_transport *
nvmf_urma_create(struct spdk_nvmf_transport_opts *opts)
{
	struct nvmf_urma_transport *transport = calloc(1, sizeof(*transport));
	struct nvmf_urma_json_opts json_opts = {};

	if (transport == NULL) {
		return NULL;
	}
	transport->transport.opts = *opts;
	TAILQ_INIT(&transport->pool_regions);
	for (uint32_t i = 0; i < NVMF_URMA_IMPORT_BUCKETS; i++) {
		LIST_INIT(&transport->import_cache[i]);
	}
	if (pthread_rwlock_init(&transport->import_cache_lock, NULL) != 0) {
		free(transport);
		return NULL;
	}
	spdk_urma_opts_init(&transport->urma_opts);
	json_opts.active_port = transport->urma_opts.active_port;
	json_opts.eid_index = transport->urma_opts.eid_index;
	json_opts.send_jfc_count = transport->urma_opts.send_jfc_count;
	json_opts.recv_jfc_count = transport->urma_opts.recv_jfc_count;
	json_opts.jfc_depth = transport->urma_opts.jfc_depth;
	json_opts.num_jetty_per_ep = transport->urma_opts.num_jetty_per_ep;
	json_opts.jetty_depth = transport->urma_opts.jetty_depth;
	json_opts.priority = transport->urma_opts.priority;
	json_opts.bonding_balance = transport->urma_opts.bonding_balance;
	json_opts.bonding_multipath = transport->urma_opts.bonding_multipath;
	json_opts.numa_affinity = transport->urma_opts.numa_affinity;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, g_urma_opts_decoder,
					    SPDK_COUNTOF(g_urma_opts_decoder), &json_opts) != 0) {
		free(json_opts.dev_name);
		free(json_opts.trans_mode);
		free(json_opts.capsule_transport);
		free(json_opts.tp_type);
		pthread_rwlock_destroy(&transport->import_cache_lock);
		free(transport);
		return NULL;
	}
	if (json_opts.dev_name != NULL) {
		snprintf(transport->urma_opts.dev_name, sizeof(transport->urma_opts.dev_name),
			 "%s", json_opts.dev_name);
	}
	if (json_opts.trans_mode != NULL) {
		if (strcasecmp(json_opts.trans_mode, "RC") == 0) {
			transport->urma_opts.transport_mode = URMA_TM_RC;
		} else if (strcasecmp(json_opts.trans_mode, "UM") == 0) {
			transport->urma_opts.transport_mode = URMA_TM_UM;
		} else if (strcasecmp(json_opts.trans_mode, "RM") == 0) {
			transport->urma_opts.transport_mode = URMA_TM_RM;
		} else {
			free(json_opts.dev_name);
			free(json_opts.trans_mode);
			free(json_opts.capsule_transport);
			free(json_opts.tp_type);
			pthread_rwlock_destroy(&transport->import_cache_lock);
			free(transport);
			return NULL;
		}
	}
	if (json_opts.capsule_transport != NULL &&
	    spdk_urma_parse_capsule_transport(json_opts.capsule_transport,
					      &transport->urma_opts.capsule_transport) != 0) {
		SPDK_ERRLOG("Invalid URMA capsule_transport '%s' (expected 'tcp' or 'sendrecv')\n",
			    json_opts.capsule_transport);
		free(json_opts.dev_name);
		free(json_opts.trans_mode);
		free(json_opts.capsule_transport);
		free(json_opts.tp_type);
		pthread_rwlock_destroy(&transport->import_cache_lock);
		free(transport);
		return NULL;
	}
	if (json_opts.jfc_count != 0) {
		json_opts.send_jfc_count = json_opts.jfc_count;
		json_opts.recv_jfc_count = json_opts.jfc_count;
	}
	if (json_opts.jetty_count != 0) {
		json_opts.num_jetty_per_ep = json_opts.jetty_count;
	}
	transport->urma_opts.active_port = json_opts.active_port;
	transport->urma_opts.eid_index = json_opts.eid_index;
	transport->urma_opts.send_jfc_count = json_opts.send_jfc_count;
	transport->urma_opts.recv_jfc_count = json_opts.recv_jfc_count;
	transport->urma_opts.jfc_depth = json_opts.jfc_depth;
	transport->urma_opts.num_jetty_per_ep = json_opts.num_jetty_per_ep;
	transport->urma_opts.jetty_depth = json_opts.jetty_depth;
	transport->urma_opts.priority = json_opts.priority;
	if (json_opts.tp_type != NULL) {
		if (strcasecmp(json_opts.tp_type, "ctp") == 0) {
			transport->urma_opts.tp_type = URMA_CTP;
		} else if (strcasecmp(json_opts.tp_type, "rtp") == 0) {
			transport->urma_opts.tp_type = URMA_RTP;
		} else {
			SPDK_ERRLOG("Invalid URMA tp_type '%s'\n", json_opts.tp_type);
			free(json_opts.dev_name);
			free(json_opts.trans_mode);
			free(json_opts.capsule_transport);
			free(json_opts.tp_type);
			pthread_rwlock_destroy(&transport->import_cache_lock);
			free(transport);
			return NULL;
		}
	}
	transport->urma_opts.bonding_balance = json_opts.bonding_balance;
	transport->urma_opts.bonding_multipath = json_opts.bonding_multipath;
	transport->urma_opts.numa_affinity = json_opts.numa_affinity;
	free(json_opts.dev_name);
	free(json_opts.trans_mode);
	free(json_opts.capsule_transport);
	free(json_opts.tp_type);
	transport->urma_opts.max_io_size = opts->max_io_size;
	if (spdk_urma_device_open(&transport->urma_opts, &transport->device) != 0) {
		/* Modified By Yida(v6): 具体失败阶段由 device_open 内部日志给出，这里带设备名兜底 */
		SPDK_ERRLOG("urma transport create: device open failed for '%s'\n",
			    transport->urma_opts.dev_name);
		pthread_rwlock_destroy(&transport->import_cache_lock);
		free(transport);
		return NULL;
	}
	if (spdk_iobuf_for_each_pool_memory(nvmf_urma_register_iobuf_chunk, transport) != 0) {
		SPDK_ERRLOG("URMA failed to register the complete target iobuf pool\n");
		nvmf_urma_unregister_iobuf_pool(transport);
		spdk_urma_device_close(transport->device);
		pthread_rwlock_destroy(&transport->import_cache_lock);
		free(transport);
		return NULL;
	}
	TAILQ_INIT(&transport->ports);
	TAILQ_INIT(&transport->poll_groups);
	transport->accept_poller = SPDK_POLLER_REGISTER(nvmf_urma_accept, transport, 1000);
	/* Modified By Yida(v3): optional periodic staged-latency dump, e.g.
	 * SPDK_URMA_TARGET_DUMP_SEC=10 ./build/bin/nvmf_tgt ... */
	const char *dump_sec = getenv("SPDK_URMA_TARGET_DUMP_SEC");

	if (dump_sec != NULL) {
		uint64_t period_us = (uint64_t)atoi(dump_sec) * 1000000ULL;

		if (period_us > 0) {
			transport->dump_poller = SPDK_POLLER_REGISTER(nvmf_urma_timing_dump_poller,
							transport, period_us);
		}
	}
	return &transport->transport;
}

static void
nvmf_urma_dump_opts(struct spdk_nvmf_transport *base, struct spdk_json_write_ctx *w)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	const char *mode = transport->urma_opts.transport_mode == URMA_TM_RC ? "RC" :
			   transport->urma_opts.transport_mode == URMA_TM_UM ? "UM" : "RM";

	spdk_json_write_named_string(w, "dev_name", transport->device->dev_name);
	spdk_json_write_named_string(w, "trans_mode", mode);
	spdk_json_write_named_string(w, "capsule_transport",
				     spdk_urma_capsule_transport_name(
					     transport->urma_opts.capsule_transport));
	spdk_json_write_named_int32(w, "active_port", transport->device->active_port);
	spdk_json_write_named_uint32(w, "eid_index", transport->device->eid_index);
	spdk_json_write_named_uint32(w, "send_jfc_count", transport->device->send_jfc_count);
	spdk_json_write_named_uint32(w, "recv_jfc_count", transport->device->recv_jfc_count);
	spdk_json_write_named_uint32(w, "jfc_depth", transport->urma_opts.jfc_depth);
	spdk_json_write_named_uint32(w, "num_jetty_per_ep", transport->urma_opts.num_jetty_per_ep);
	spdk_json_write_named_uint32(w, "jetty_depth", transport->urma_opts.jetty_depth);
	spdk_json_write_named_int32(w, "priority", transport->device->priority);
	spdk_json_write_named_string(w, "tp_type",
				     transport->urma_opts.tp_type == URMA_CTP ? "ctp" : "rtp");
	spdk_json_write_named_bool(w, "bonding_balance", transport->urma_opts.bonding_balance);
	spdk_json_write_named_bool(w, "bonding_multipath", transport->urma_opts.bonding_multipath);
	spdk_json_write_named_bool(w, "numa_affinity", transport->urma_opts.numa_affinity);
}

static void
nvmf_urma_destroy(struct spdk_nvmf_transport *base,
		   spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_port *port, *tmp;

	spdk_poller_unregister(&transport->accept_poller);
	/* Modified By Yida(v3): stop periodic dump and print final staged latencies */
	spdk_poller_unregister(&transport->dump_poller);
	nvmf_urma_timing_dump();
	/* Modified By Yida(v4): final per-I/O rx trace for the last round */
	nvmf_urma_rx_trace_dump();
	TAILQ_FOREACH_SAFE(port, &transport->ports, link, tmp) {
		TAILQ_REMOVE(&transport->ports, port, link);
		close(port->fd);
		free(port);
	}
	nvmf_urma_unregister_iobuf_pool(transport);
	nvmf_urma_import_cache_fini(transport);
	pthread_rwlock_destroy(&transport->import_cache_lock);
	spdk_urma_device_close(transport->device);
	free(transport);
	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

static int
nvmf_urma_listen(struct spdk_nvmf_transport *base, const struct spdk_nvme_transport_id *trid,
		  struct spdk_nvmf_listen_opts *opts)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_port *port;
	struct addrinfo hints = {}, *result = NULL, *it;
	int rc = -EINVAL, one = 1;

	hints.ai_family = trid->adrfam == SPDK_NVMF_ADRFAM_IPV6 ? AF_INET6 : AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(trid->traddr[0] == '\0' ? NULL : trid->traddr, trid->trsvcid,
			&hints, &result) != 0) {
		return -EINVAL;
	}
	port = calloc(1, sizeof(*port));
	if (port == NULL) {
		freeaddrinfo(result);
		return -ENOMEM;
	}
	port->fd = -1;
	for (it = result; it != NULL; it = it->ai_next) {
		port->fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
				  it->ai_protocol);
		if (port->fd < 0) {
			continue;
		}
		setsockopt(port->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(port->fd, it->ai_addr, it->ai_addrlen) == 0 && listen(port->fd, 512) == 0) {
			rc = 0;
			break;
		}
		close(port->fd);
		port->fd = -1;
	}
	freeaddrinfo(result);
	if (rc != 0) {
		free(port);
		return rc;
	}
	port->trid = *trid;
	TAILQ_INSERT_TAIL(&transport->ports, port, link);
	return 0;
}

static void
nvmf_urma_stop_listen(struct spdk_nvmf_transport *base, const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_port *port, *tmp;
	TAILQ_FOREACH_SAFE(port, &transport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(&port->trid, trid) == 0) {
			TAILQ_REMOVE(&transport->ports, port, link);
			close(port->fd);
			free(port);
			return;
		}
	}
}

static struct spdk_nvmf_transport_poll_group *
nvmf_urma_poll_group_create(struct spdk_nvmf_transport *base, struct spdk_nvmf_poll_group *group)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_poll_group *ugroup = calloc(1, sizeof(*ugroup));
	if (ugroup == NULL) {
		return NULL;
	}
	ugroup->group.transport = base;
	ugroup->group.group = group;
	TAILQ_INIT(&ugroup->qpairs);
	ugroup->worker_index = transport->next_worker_index++;
	transport->worker_count++;
	TAILQ_INSERT_TAIL(&transport->poll_groups, ugroup, link);
	return &ugroup->group;
}

static void
nvmf_urma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *base)
{
	struct nvmf_urma_poll_group *group = SPDK_CONTAINEROF(base, struct nvmf_urma_poll_group, group);
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base->transport,
							 struct nvmf_urma_transport, transport);
	struct nvmf_urma_poll_group *it;

	TAILQ_REMOVE(&transport->poll_groups, group, link);
	assert(transport->worker_count > 0);
	transport->worker_count--;
	transport->next_worker_index = 0;
	TAILQ_FOREACH(it, &transport->poll_groups, link) {
		it->worker_index = transport->next_worker_index++;
	}
	free(group);
}

static int
nvmf_urma_poll_group_add(struct spdk_nvmf_transport_poll_group *base, struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_urma_poll_group *group = SPDK_CONTAINEROF(base, struct nvmf_urma_poll_group, group);
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(qpair);
	int rc;

	uqpair->resource_count = base->transport->opts.max_queue_depth;
	uqpair->reqs = calloc(uqpair->resource_count, sizeof(*uqpair->reqs));
	if (uqpair->reqs == NULL) {
		return -ENOMEM;
	}
	for (uint32_t i = 0; i < uqpair->resource_count; i++) {
		struct nvmf_urma_req *ureq = &uqpair->reqs[i];
		ureq->cqe.type = NVMF_URMA_CQE_DATA;
		ureq->cqe.owner = ureq;
		ureq->req.qpair = qpair;
		ureq->req.cmd = &ureq->cmd;
		ureq->req.rsp = &ureq->rsp;
		TAILQ_INSERT_TAIL(&uqpair->free_reqs, ureq, link);
	}
	uqpair->group = group;
	uqpair->thread = spdk_get_thread();
	rc = nvmf_urma_capsule_resources_init(uqpair);
	if (rc != 0) {
		uqpair->thread = NULL;
		uqpair->group = NULL;
		free(uqpair->reqs);
		uqpair->reqs = NULL;
		uqpair->resource_count = 0;
		return rc;
	}
	TAILQ_INSERT_TAIL(&group->qpairs, uqpair, link);
	return 0;
}

static int
nvmf_urma_poll_group_remove(struct spdk_nvmf_transport_poll_group *base, struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(qpair);
	if (uqpair->group != NULL) {
		TAILQ_REMOVE(&uqpair->group->qpairs, uqpair, link);
	}
	return 0;
}

static void nvmf_urma_release_req(struct nvmf_urma_req *ureq);

static void
nvmf_urma_disconnect_qpair(struct nvmf_urma_qpair *uqpair)
{
	int rc;

	if (!spdk_nvmf_qpair_is_active(&uqpair->qpair)) {
		return;
	}
	rc = spdk_nvmf_qpair_disconnect(&uqpair->qpair);
	if (rc != 0 && rc != -EINPROGRESS) {
		SPDK_ERRLOG("Failed to disconnect URMA qpair qid=%u, rc=%d\n",
			    uqpair->qpair.qid, rc);
	}
}

static int
nvmf_urma_send_response(struct nvmf_urma_req *ureq)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(ureq->req.qpair);
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_capsule_rsp rsp = {.cpl = ureq->rsp.nvme_cpl};
	int rc;
	uint64_t t_rsp0 = spdk_get_ticks(); /* Modified By Yida(v3): W10 start */

	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_CAPSULE_RSP;
	hdr.length = sizeof(rsp);
	hdr.qid = uqpair->qpair.qid;
	/* Keep the wire frame contiguous. TCP emits it with one write; SEND/RECV
	 * copies the same bytes inline into the WQE before this stack frame goes
	 * out of scope. */
	if (uqpair->capsule_transport == SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV) {
		struct spdk_urma_capsule_rsp_frame frame = {
			.hdr = hdr,
			.capsule = rsp,
		};
		urma_sge_t sge = {
			.addr = (uint64_t)&frame,
			.len = sizeof(frame),
		};
		urma_jfs_wr_t wr = {}, *bad_wr = NULL;
		uint32_t jetty_index = nvmf_urma_next_jetty(uqpair);

		wr.opcode = URMA_OPC_SEND;
		wr.flag.bs.complete_enable = 1;
		wr.flag.bs.inline_flag = 1;
		wr.tjetty = uqpair->target_jettys[jetty_index];
		wr.user_ctx = (uint64_t)&uqpair->capsule_tx_cqe;
		wr.send.src.sge = &sge;
		wr.send.src.num_sge = 1;
		rc = urma_post_jetty_send_wr(uqpair->jettys[jetty_index], &wr, &bad_wr) ==
		     URMA_SUCCESS ? 0 : -EIO;
	} else {
		uint8_t msg[sizeof(hdr) + sizeof(rsp)];

		memcpy(msg, &hdr, sizeof(hdr));
		memcpy(msg + sizeof(hdr), &rsp, sizeof(rsp));
		rc = nvmf_urma_write_full(uqpair->fd, msg, sizeof(msg));
	}
	/* Modified By Yida(v3): W10 send rsp + target service total (parse -> rsp written) */
	if (ureq->start_tick != 0) {
		uint64_t t_rsp1 = spdk_get_ticks();

		NVMF_URMA_TGT_STAGE(rsp, t_rsp1 - t_rsp0);
		NVMF_URMA_TGT_STAGE(total, t_rsp1 - ureq->start_tick);
		ureq->start_tick = 0;
	}
	/* Modified By Yida(v4): close this I/O's rx trace record with the response
	 * completion tick (per-I/O target service time, even for un-timed reqs). */
	if (ureq->trace_idx != UINT32_MAX) {
		uint64_t idx = ureq->trace_idx % NVMF_URMA_RX_TRACE_SIZE;

		__atomic_store_n(&g_rx_trace[idx].rsp_tick, spdk_get_ticks(), __ATOMIC_RELAXED);
		ureq->trace_idx = UINT32_MAX;
	}
	nvmf_urma_release_req(ureq);
	return rc;
}

/* Modified By Yida(v7): 远端段 import 缓存默认开启；SPDK_URMA_IMPORT_CACHE=0
 * 恢复 per-I/O import/unimport 旧路径（回归对比用）。 */
static int
nvmf_urma_import_cache_enabled(void)
{
	static int enabled = -1;

	if (enabled == -1) {
		const char *v = getenv("SPDK_URMA_IMPORT_CACHE");

		enabled = !(v != NULL && v[0] == '0');
	}
	return enabled;
}

static int
nvmf_urma_post_data(struct nvmf_urma_req *ureq, bool push)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(ureq->req.qpair);
	struct spdk_urma_device *device = uqpair->device;
	urma_import_seg_flag_t import_flag = {};
	urma_token_t token = {.token = SPDK_URMA_DEFAULT_TOKEN};
	urma_sge_t local_sge = {}, remote_sge = {};
	urma_jfs_wr_t wr = {}, *bad_wr = NULL;
	bondp_jfs_wr_t affinity_wr = {};
	uint32_t jetty_index;
	int rc;

	if (ureq->req.iovcnt != 1) {
		return -ENOTSUP;
	}
	import_flag.bs.cacheable = URMA_NON_CACHEABLE;
	import_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
	import_flag.bs.mapping = URMA_SEG_NOMAP;
	/* Modified By Yida(v7): W5 — 远端段 import 缓存：seg 相同的 I/O 复用已
	 * import 的 tseg，只有首次见到的 seg 才真正调 urma_import_seg。
	 * initiator 整池注册时所有 I/O 共享同一 seg → 每个共享 context 仅 1 次 import。 */
	ureq->remote_seg_cached = false;
	if (nvmf_urma_import_cache_enabled()) {
		struct nvmf_urma_transport *transport = uqpair->transport;
		struct nvmf_urma_import_entry *e;

		pthread_rwlock_rdlock(&transport->import_cache_lock);
		e = nvmf_urma_import_cache_find(transport, &ureq->remote_data.seg);
		if (e != NULL) {
			ureq->remote_seg = e->tseg;
			ureq->remote_seg_cached = true;
			pthread_rwlock_unlock(&transport->import_cache_lock);
			NVMF_URMA_TGT_INC(import_hits);
		} else {
			pthread_rwlock_unlock(&transport->import_cache_lock);
			pthread_rwlock_wrlock(&transport->import_cache_lock);
			/* Another worker may have populated the bucket while this worker
			 * upgraded from the shared lookup to the exclusive miss path. */
			e = nvmf_urma_import_cache_find(transport, &ureq->remote_data.seg);
			if (e != NULL) {
				ureq->remote_seg = e->tseg;
				ureq->remote_seg_cached = true;
				NVMF_URMA_TGT_INC(import_hits);
			} else {
				uint64_t t_import0 = spdk_get_ticks();

				/* Serialize the first import for a segment on this shared context. */
				ureq->remote_seg = urma_import_seg(device->context, &ureq->remote_data.seg,
							   &token, 0, import_flag);
				if (ureq->remote_seg != NULL) {
					NVMF_URMA_TGT_STAGE(import, spdk_get_ticks() - t_import0);
					e = calloc(1, sizeof(*e));
					if (e != NULL) {
						e->seg = ureq->remote_data.seg;
						e->tseg = ureq->remote_seg;
						LIST_INSERT_HEAD(&transport->import_cache[
								 nvmf_urma_seg_hash(&e->seg)], e, link);
						ureq->remote_seg_cached = true;
					}
				}
			}
			pthread_rwlock_unlock(&transport->import_cache_lock);
		}
		if (ureq->remote_seg == NULL) {
			return -EIO;
		}
	} else {
		/* Modified By Yida(v3): W5 — urma_import_seg (cache miss only) */
		uint64_t t_import0 = spdk_get_ticks();

		ureq->remote_seg = urma_import_seg(device->context, &ureq->remote_data.seg,
						   &token, 0, import_flag);
		if (ureq->remote_seg == NULL) {
			return -EIO;
		}
		NVMF_URMA_TGT_STAGE(import, spdk_get_ticks() - t_import0);
	}
	/* The iobuf backing allocations are registered once at transport creation. */
	ureq->cache_entry = NULL;
	ureq->local_region_external = false;
	ureq->local_region = nvme_urma_region_registry_find(device->context,
							    (uint64_t)ureq->req.iov[0].iov_base,
							    ureq->req.length);
	if (ureq->local_region != NULL) {
		ureq->local_region_external = true;
	}
	for (int i = 0; ureq->local_region == NULL && i < NVMF_URMA_REG_CACHE_SIZE; i++) {
		struct nvmf_urma_reg_entry *e = &uqpair->reg_cache[i];
		if (e->used && e->va == ureq->req.iov[0].iov_base && e->len == ureq->req.length) {
			ureq->local_region = e->region;
			ureq->cache_entry = e;
			break;
		}
	}
	if (ureq->local_region == NULL) {
		/* Modified By Yida(v3): W6 — register_memory (cache miss only) */
		uint64_t t_reg0 = spdk_get_ticks();

		rc = spdk_nvme_urma_register_memory(device->context, ureq->req.iov[0].iov_base,
				ureq->req.length, SPDK_NVME_URMA_MEM_HOST, &ureq->local_region);
		if (rc != 0) {
			return rc;
		}
		NVMF_URMA_TGT_ADD(reg_ticks, spdk_get_ticks() - t_reg0);
		NVMF_URMA_TGT_INC(reg_misses);
		/* Insert into cache */
		for (int i = 0; i < NVMF_URMA_REG_CACHE_SIZE; i++) {
			struct nvmf_urma_reg_entry *e = &uqpair->reg_cache[i];
			if (!e->used) {
				e->va = ureq->req.iov[0].iov_base;
				e->len = ureq->req.length;
				e->region = ureq->local_region;
				e->used = true;
				ureq->cache_entry = e;
				break;
			}
		}
	} else {
		NVMF_URMA_TGT_INC(reg_hits);
	}
	local_sge.addr = (uint64_t)ureq->req.iov[0].iov_base;
	local_sge.len = ureq->req.length;
	local_sge.tseg = spdk_urma_memory_region_get_tseg(ureq->local_region);
	remote_sge.addr = ureq->remote_data.address;
	remote_sge.len = ureq->req.length;
	remote_sge.tseg = ureq->remote_seg;
	wr.user_ctx = (uint64_t)&ureq->cqe;
	wr.opcode = push ? URMA_OPC_WRITE : URMA_OPC_READ;
	wr.rw.src.sge = push ? &local_sge : &remote_sge;
	wr.rw.src.num_sge = 1;
	wr.rw.dst.sge = push ? &remote_sge : &local_sge;
	wr.rw.dst.num_sge = 1;
	wr.flag.bs.complete_enable = 1;
	jetty_index = nvmf_urma_next_jetty(uqpair);
	wr.tjetty = uqpair->target_jettys[jetty_index];
	ureq->state = push ? NVMF_URMA_REQ_PUSHING : NVMF_URMA_REQ_PULLING;
	/* Modified By Yida(v3): W7 post WR; tick doubles as JFC wait start */
	ureq->post_tick = spdk_get_ticks();
	if (device->opts.numa_affinity && ureq->remote_data.chip_id != 0 &&
	    ureq->remote_data.chip_id != SPDK_URMA_INVALID_CHIP_ID) {
		affinity_wr.base = wr;
		affinity_wr.base.flag.bs.has_drv_ext = 1;
		affinity_wr.src_chip_id = ureq->remote_data.chip_id;
		affinity_wr.dst_chip_id = ureq->remote_data.chip_id;
		if (urma_post_jetty_send_wr(uqpair->jettys[jetty_index], &affinity_wr.base,
					     &bad_wr) != URMA_SUCCESS) {
			return -EIO;
		}
	} else if (urma_post_jetty_send_wr(uqpair->jettys[jetty_index], &wr, &bad_wr) !=
		   URMA_SUCCESS) {
		return -EIO;
	}
	{
		uint64_t t_post1 = spdk_get_ticks();

		NVMF_URMA_TGT_STAGE(post, t_post1 - ureq->post_tick);
		ureq->post_tick = t_post1;
		return 0;
	}
}

static void
nvmf_urma_buffers_ready(struct nvmf_urma_req *ureq)
{
	/* Modified By Yida(v3): W4b — capsule parsed -> buffers ready */
	if (ureq->start_tick != 0) {
		NVMF_URMA_TGT_STAGE(buffer, spdk_get_ticks() - ureq->start_tick);
	}
	if (ureq->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		if (nvmf_urma_post_data(ureq, false) != 0) {
			struct spdk_nvmf_qpair *qpair = ureq->req.qpair;

			ureq->rsp.nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			ureq->rsp.nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			if (nvmf_urma_send_response(ureq) != 0) {
				nvmf_urma_disconnect_qpair(nvmf_urma_qpair(qpair));
			}
		}
	} else {
		ureq->state = NVMF_URMA_REQ_EXECUTING;
		ureq->exec_tick = spdk_get_ticks(); /* Modified By Yida(v3): W9 start (C2H, no pull) */
		spdk_nvmf_request_exec(&ureq->req);
	}
}

static void
nvmf_urma_req_get_buffers_done(struct spdk_nvmf_request *req)
{
	nvmf_urma_buffers_ready(nvmf_urma_req(req));
}

static int
nvmf_urma_handle_capsule(struct nvmf_urma_qpair *uqpair,
			 const struct spdk_urma_msg_hdr *hdr,
			 const struct spdk_urma_capsule_cmd *capsule,
			 uint64_t t_peek, uint64_t t_parse0)
{
	struct nvmf_urma_req *ureq;

	if (capsule->data.length > uqpair->max_io_size || hdr->qid != uqpair->qpair.qid) {
		return capsule->data.length > uqpair->max_io_size ? -EMSGSIZE : -EPROTO;
	}
	ureq = TAILQ_FIRST(&uqpair->free_reqs);
	if (ureq == NULL) {
		return -ENOBUFS;
	}
	TAILQ_REMOVE(&uqpair->free_reqs, ureq, link);
	TAILQ_INSERT_TAIL(&uqpair->working_reqs, ureq, link);
	memset(&ureq->rsp, 0, sizeof(ureq->rsp));
	ureq->start_tick = 0;
	ureq->exec_tick = 0;
	ureq->trace_idx = UINT32_MAX;
	NVMF_URMA_TGT_STAGE(peek_wait, t_parse0 - t_peek);
	if (nvmf_urma_trace_enabled()) {
		uint64_t idx = __atomic_fetch_add(&g_rx_trace_idx, 1, __ATOMIC_RELAXED) %
			       NVMF_URMA_RX_TRACE_SIZE;
		struct nvmf_urma_rx_trace *rec = &g_rx_trace[idx];

		rec->peek_tick = t_peek;
		rec->parse_tick = t_parse0;
		rec->rsp_tick = 0;
		rec->length = capsule->data.length;
		rec->qid = hdr->qid;
		rec->cid = capsule->cmd.cid;
		rec->opcode = capsule->cmd.opc;
		ureq->trace_idx = idx;
	}
	ureq->cmd.nvme_cmd = capsule->cmd;
	ureq->remote_data = capsule->data;
	ureq->req.raw = 0;
	ureq->req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;
	ureq->req.xfer = spdk_nvmf_req_get_xfer(&ureq->req);
	ureq->req.length = capsule->data.length;
	uqpair->qpair.queue_depth++;
	if (ureq->req.xfer == SPDK_NVME_DATA_NONE || ureq->req.length == 0) {
		ureq->state = NVMF_URMA_REQ_EXECUTING;
		spdk_nvmf_request_exec(&ureq->req);
		return 1;
	}
	ureq->state = NVMF_URMA_REQ_NEED_BUFFER;
	ureq->start_tick = spdk_get_ticks();
	if (spdk_nvmf_request_get_buffers(&ureq->req, &uqpair->group->group,
					  &uqpair->transport->transport, ureq->req.length) == 0) {
		nvmf_urma_buffers_ready(ureq);
	}
	return 1;
}

static int
nvmf_urma_receive_capsule(struct nvmf_urma_qpair *uqpair)
{
	struct spdk_urma_msg_hdr hdr;
	struct spdk_urma_capsule_cmd capsule;
	ssize_t rc;

	rc = recv(uqpair->fd, &hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
	if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	}
	if (rc <= 0) {
		uqpair->pending_peek_tick = 0;
		return -ECONNRESET;
	}
	if ((size_t)rc < sizeof(hdr)) {
		return 0;
	}
	/* Modified By Yida(v4): W4p — stamp when the poller FIRST observed this
	 * capsule in the kernel rcvbuf. If the hdr was already seen on an earlier
	 * poll round (capsule body still in flight), reuse that tick: the capsule
	 * has been queueing ever since. */
	uint64_t t_peek;
	if (uqpair->pending_peek_tick != 0) {
		t_peek = uqpair->pending_peek_tick;
	} else {
		t_peek = spdk_get_ticks();
		uqpair->pending_peek_tick = t_peek;
	}
	{
		int available = 0;

		if (ioctl(uqpair->fd, FIONREAD, &available) != 0) {
			return -errno;
		}
		if ((size_t)available < sizeof(hdr) + hdr.length) {
			/* Capsule split across TCP segments: hdr arrived, body not yet.
			 * Keep pending_peek_tick and retry on a later poll round. */
			NVMF_URMA_TGT_INC(partial_n);
			return 0;
		}
	}
	uqpair->pending_peek_tick = 0; /* full capsule arrived; marker consumed */
	uint64_t t_parse0 = spdk_get_ticks(); /* Modified By Yida(v3): W4a parse start */
	if (nvmf_urma_read_full(uqpair->fd, &hdr, sizeof(hdr)) != 0 ||
	    hdr.magic != SPDK_URMA_WIRE_MAGIC || hdr.version != SPDK_URMA_WIRE_VERSION ||
	    hdr.type != SPDK_URMA_MSG_CAPSULE_CMD || hdr.length != sizeof(capsule) ||
	    nvmf_urma_read_full(uqpair->fd, &capsule, sizeof(capsule)) != 0) {
		return -EPROTO;
	}
	NVMF_URMA_TGT_STAGE(capsule, spdk_get_ticks() - t_parse0);
	return nvmf_urma_handle_capsule(uqpair, &hdr, &capsule, t_peek, t_parse0);
}

/* The TCP socket remains the lifetime channel even when NVMe capsules use
 * URMA SEND/RECV.  No payload is expected on it after the handshake in that
 * mode, but observing EOF is required to release the target qpair when the
 * initiator exits. */
static int
nvmf_urma_check_lifetime_socket(struct nvmf_urma_qpair *uqpair)
{
	uint8_t byte;
	ssize_t rc;

	rc = recv(uqpair->fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
	if (rc > 0) {
		/* SEND/RECV mode must not carry capsules on the bootstrap socket. */
		return -EPROTO;
	}
	if (rc == 0) {
		return -ECONNRESET;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return 0;
	}
	return -errno;
}

static int
nvmf_urma_process_cmd_receive(struct nvmf_urma_cmd_rx_slot *slot,
			      const urma_cr_t *cr)
{
	struct nvmf_urma_qpair *uqpair = slot->qpair;
	struct spdk_urma_capsule_cmd_frame frame = slot->frame;
	uint64_t t_parse0 = spdk_get_ticks();
	int rc;

	if (cr->flag.bs.s_r == 0 || cr->opcode != URMA_CR_OPC_SEND ||
	    cr->completion_len != (uint32_t)sizeof(frame)) {
		return -EPROTO;
	}
	rc = nvmf_urma_post_cmd_receive(slot);
	if (rc != 0) {
		return rc;
	}
	if (frame.hdr.magic != SPDK_URMA_WIRE_MAGIC ||
	    frame.hdr.version != SPDK_URMA_WIRE_VERSION ||
	    frame.hdr.type != SPDK_URMA_MSG_CAPSULE_CMD ||
	    frame.hdr.length != sizeof(frame.capsule) ||
	    frame.hdr.qid != uqpair->qpair.qid) {
		return -EPROTO;
	}
	NVMF_URMA_TGT_STAGE(capsule, spdk_get_ticks() - t_parse0);
	return nvmf_urma_handle_capsule(uqpair, &frame.hdr, &frame.capsule,
				       t_parse0, t_parse0);
}

static int
nvmf_urma_handle_completion(const urma_cr_t *completion)
{
	struct nvmf_urma_cqe_ctx *ctx;
	struct nvmf_urma_qpair *uqpair;
	struct nvmf_urma_req *ureq;

	if (spdk_urma_cr_is_fake(completion)) {
		return 0;
	}
	ctx = (void *)completion->user_ctx;
	if (ctx == NULL) {
		return 0;
	}
	if (ctx->type == NVMF_URMA_CQE_CAPSULE_TX) {
		uqpair = ctx->owner;
	} else if (ctx->type == NVMF_URMA_CQE_CAPSULE_RX) {
		uqpair = ((struct nvmf_urma_cmd_rx_slot *)ctx->owner)->qpair;
	} else if (ctx->type == NVMF_URMA_CQE_DATA) {
		ureq = ctx->owner;
		uqpair = nvmf_urma_qpair(ureq->req.qpair);
	} else {
		return -EPROTO;
	}
	if (completion->status != URMA_CR_SUCCESS) {
		SPDK_ERRLOG("poll_group: completion error status=%d type=%d user_ctx=%p\n",
			    completion->status, ctx->type, (void *)completion->user_ctx);
		if (ctx->type != NVMF_URMA_CQE_DATA) {
			nvmf_urma_disconnect_qpair(uqpair);
			return 0;
		}
		ureq = ctx->owner;
		ureq->rsp.nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		ureq->rsp.nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		if (nvmf_urma_send_response(ureq) != 0) {
			nvmf_urma_disconnect_qpair(uqpair);
		}
		return 0;
	}
	if (ctx->type == NVMF_URMA_CQE_CAPSULE_TX) {
		if (completion->flag.bs.s_r != 0) {
			nvmf_urma_disconnect_qpair(uqpair);
		}
		return 1;
	}
	if (ctx->type == NVMF_URMA_CQE_CAPSULE_RX) {
		int rc = nvmf_urma_process_cmd_receive(ctx->owner, completion);

		if (rc < 0) {
			nvmf_urma_disconnect_qpair(uqpair);
			return 0;
		}
		return rc;
	}
	ureq = ctx->owner;
	if (ureq->state == NVMF_URMA_REQ_PULLING) {
		/* Modified By Yida(v3): W8 — WR posted -> JFC completion
		 * (includes poller scheduling latency) */
		NVMF_URMA_TGT_STAGE(jfc, spdk_get_ticks() - ureq->post_tick);
		ureq->exec_tick = spdk_get_ticks();
		ureq->state = NVMF_URMA_REQ_EXECUTING;
		spdk_nvmf_request_exec(&ureq->req);
	} else if (ureq->state == NVMF_URMA_REQ_PUSHING) {
		/* Modified By Yida(v3): C2H push — WR posted -> JFC completion */
		NVMF_URMA_TGT_STAGE(push, spdk_get_ticks() - ureq->post_tick);
		if (nvmf_urma_send_response(ureq) != 0) {
			nvmf_urma_disconnect_qpair(uqpair);
		}
	}
	return 1;
}

struct nvmf_urma_completion_msg {
	urma_cr_t completion;
};

static void
nvmf_urma_completion_msg_fn(void *ctx)
{
	struct nvmf_urma_completion_msg *msg = ctx;

	(void)nvmf_urma_handle_completion(&msg->completion);
	free(msg);
}

static int
nvmf_urma_process_completion(const urma_cr_t *completion)
{
	struct nvmf_urma_cqe_ctx *ctx;
	struct nvmf_urma_qpair *uqpair = NULL;
	struct nvmf_urma_completion_msg *msg;

	if (spdk_urma_cr_is_fake(completion)) {
		return 0;
	}
	ctx = (void *)completion->user_ctx;
	if (ctx == NULL) {
		return 0;
	}
	if (ctx->type == NVMF_URMA_CQE_CAPSULE_TX) {
		uqpair = ctx->owner;
	} else if (ctx->type == NVMF_URMA_CQE_CAPSULE_RX) {
		uqpair = ((struct nvmf_urma_cmd_rx_slot *)ctx->owner)->qpair;
	} else if (ctx->type == NVMF_URMA_CQE_DATA) {
		struct nvmf_urma_req *ureq = ctx->owner;

		uqpair = nvmf_urma_qpair(ureq->req.qpair);
	}
	if (uqpair == NULL || uqpair->thread == NULL || uqpair->thread == spdk_get_thread()) {
		return nvmf_urma_handle_completion(completion);
	}
	msg = malloc(sizeof(*msg));
	if (msg == NULL) {
		nvmf_urma_disconnect_qpair(uqpair);
		return -ENOMEM;
	}
	msg->completion = *completion;
	if (spdk_thread_send_msg(uqpair->thread, nvmf_urma_completion_msg_fn, msg) != 0) {
		free(msg);
		nvmf_urma_disconnect_qpair(uqpair);
		return -EIO;
	}
	return 0;
}

static int
nvmf_urma_poll_group_poll(struct spdk_nvmf_transport_poll_group *base)
{
	struct nvmf_urma_poll_group *group = SPDK_CONTAINEROF(base, struct nvmf_urma_poll_group, group);
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base->transport,
							 struct nvmf_urma_transport, transport);
	struct nvmf_urma_qpair *uqpair, *tmp;
	urma_cr_t completions[64];
	uint32_t stride = spdk_max(transport->worker_count, 1u);
	int total = 0;

	for (uint32_t j = group->worker_index; j < transport->device->send_jfc_count; j += stride) {
		int count = spdk_urma_device_poll_send_jfc(transport->device, j,
				SPDK_COUNTOF(completions), completions);
		if (count < 0) {
			return -EIO;
		}
		for (int i = 0; i < count; i++) {
			int rc = nvmf_urma_process_completion(&completions[i]);
			if (rc < 0) {
				return rc;
			}
			total += rc;
		}
	}
	for (uint32_t j = group->worker_index; j < transport->device->recv_jfc_count; j += stride) {
		int count = spdk_urma_device_poll_recv_jfc(transport->device, j,
				SPDK_COUNTOF(completions), completions);
		if (count < 0) {
			return -EIO;
		}
		for (int i = 0; i < count; i++) {
			int rc = nvmf_urma_process_completion(&completions[i]);
			if (rc < 0) {
				return rc;
			}
			total += rc;
		}
	}

	TAILQ_FOREACH_SAFE(uqpair, &group->qpairs, link, tmp) {
		if (uqpair->capsule_transport == SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV) {
			int rc = nvmf_urma_check_lifetime_socket(uqpair);

			if (rc < 0) {
				SPDK_DEBUGLOG(nvmf_urma, "qpair qid=%u lifetime socket closed, rc=%d\n",
					      uqpair->qpair.qid, rc);
				/* spdk_nvmf_qpair_disconnect() may synchronously remove and free
				 * this qpair, so do not access uqpair after this call. */
				nvmf_urma_disconnect_qpair(uqpair);
				total++;
			}
			continue;
		}
		/* Modified By Yida(v4): drain every fully-arrived capsule instead of
		 * one per poll round — bursty submitters used to back up one capsule
		 * per round in the rcvbuf. Capped at NVMF_URMA_CAPSULE_BATCH so JFC
		 * completions for in-flight data still get polled promptly. */
		for (int n = 0;
		     uqpair->capsule_transport == SPDK_URMA_CAPSULE_TRANSPORT_TCP &&
		     n < NVMF_URMA_CAPSULE_BATCH; n++) {
			int rc = nvmf_urma_receive_capsule(uqpair);

			if (rc < 0) {
				nvmf_urma_disconnect_qpair(uqpair);
				total++;
				break;
			}
			if (rc == 0) {
				break;
			}
			total += rc;
		}
	}
	return total;
}

static void
nvmf_urma_release_req(struct nvmf_urma_req *ureq)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(ureq->req.qpair);
	uint64_t t_rel0 = spdk_get_ticks(); /* Modified By Yida(v3): release start */
	/* Modified By Yida: if local_region was cached, keep it cached for future I/O reuse */
	if (ureq->local_region != NULL && ureq->cache_entry == NULL &&
	    !ureq->local_region_external) {
		spdk_nvme_urma_unregister_memory(ureq->local_region);
	}
	ureq->local_region = NULL;
	ureq->cache_entry = NULL;
	ureq->local_region_external = false;
	/* Modified By Yida(v7): import 缓存保活的段留在缓存里，不 unimport */
	if (ureq->remote_seg != NULL && !ureq->remote_seg_cached) {
		urma_unimport_seg(ureq->remote_seg);
	}
	ureq->remote_seg = NULL;
	ureq->remote_seg_cached = false;
	/* Modified By Yida(v3): release — unregister (uncached) + urma_unimport_seg */
	NVMF_URMA_TGT_STAGE(release, spdk_get_ticks() - t_rel0);
	if (ureq->req.data_from_pool) {
		spdk_nvmf_request_free_buffers(&ureq->req, &uqpair->group->group,
					       &uqpair->transport->transport);
	}
	ureq->req.iovcnt = 0;
	ureq->state = NVMF_URMA_REQ_FREE;
	TAILQ_REMOVE(&uqpair->working_reqs, ureq, link);
	TAILQ_INSERT_TAIL(&uqpair->free_reqs, ureq, link);
	uqpair->qpair.queue_depth--;
}

static void
nvmf_urma_req_free(struct spdk_nvmf_request *req)
{
	nvmf_urma_release_req(nvmf_urma_req(req));
}

static void
nvmf_urma_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_urma_req *ureq = nvmf_urma_req(req);
	struct spdk_nvmf_qpair *qpair = req->qpair;

	/* Modified By Yida(v3): W9 — spdk_nvmf_request_exec -> completion (bdev/SSD) */
	if (ureq->exec_tick != 0) {
		NVMF_URMA_TGT_STAGE(exec, spdk_get_ticks() - ureq->exec_tick);
		ureq->exec_tick = 0;
	}

	ureq->rsp.nvme_cpl.cid = ureq->cmd.nvme_cmd.cid;
	ureq->rsp.nvme_cpl.sqid = qpair->qid;
	if (qpair->ctrlr == NULL || !qpair->ctrlr->sq_flow_control_disabled) {
		qpair->sq_head = qpair->sq_head == qpair->sq_head_max ? 0 : qpair->sq_head + 1;
		ureq->rsp.nvme_cpl.sqhd = qpair->sq_head;
	}
	if (spdk_nvme_cpl_is_success(&ureq->rsp.nvme_cpl) &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST && req->length != 0) {
		if (nvmf_urma_post_data(ureq, true) == 0) {
			return;
		}
		ureq->rsp.nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		ureq->rsp.nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}
	if (nvmf_urma_send_response(ureq) != 0) {
		nvmf_urma_disconnect_qpair(nvmf_urma_qpair(qpair));
	}
}

static void
nvmf_urma_qpair_fini(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(qpair);
	struct nvmf_urma_req *ureq, *tmp;
	TAILQ_FOREACH_SAFE(ureq, &uqpair->working_reqs, link, tmp) {
		nvmf_urma_release_req(ureq);
	}
	for (uint32_t i = 0; i < uqpair->jetty_count; i++) {
		if (uqpair->target_jettys != NULL && uqpair->target_jettys[i] != NULL) {
			if (uqpair->device->opts.transport_mode == URMA_TM_RC) {
				urma_unbind_jetty(uqpair->jettys[i]);
			}
			urma_unimport_jetty(uqpair->target_jettys[i]);
		}
		if (uqpair->jettys != NULL && uqpair->jettys[i] != NULL) {
			urma_delete_jetty(uqpair->jettys[i]);
		}
	}
	free(uqpair->target_jettys);
	free(uqpair->jettys);
	nvmf_urma_capsule_resources_fini(uqpair);
	/* Modified By Yida: unregister all cached target-side memory regions before freeing qpair */
	for (int i = 0; i < NVMF_URMA_REG_CACHE_SIZE; i++) {
		struct nvmf_urma_reg_entry *e = &uqpair->reg_cache[i];
		if (e->used) {
			spdk_nvme_urma_unregister_memory(e->region);
			e->used = false;
			e->region = NULL;
		}
	}
	spdk_urma_device_close(uqpair->device);
	if (uqpair->fd >= 0) {
		close(uqpair->fd);
	}
	free(uqpair->reqs);
	free(uqpair);
	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

static int
nvmf_urma_fill_trid(struct nvmf_urma_qpair *uqpair, struct spdk_nvme_transport_id *trid,
		    bool peer)
{
	memset(trid, 0, sizeof(*trid));
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_URMA);
	trid->adrfam = strchr(peer ? uqpair->peer_addr : uqpair->local_addr, ':') != NULL ?
			 SPDK_NVMF_ADRFAM_IPV6 : SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid->traddr, sizeof(trid->traddr), "%s", peer ? uqpair->peer_addr : uqpair->local_addr);
	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", uqpair->service);
	return 0;
}

static int nvmf_urma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid)
{
	return nvmf_urma_fill_trid(nvmf_urma_qpair(qpair), trid, true);
}
static int nvmf_urma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid)
{
	return nvmf_urma_fill_trid(nvmf_urma_qpair(qpair), trid, false);
}
static int nvmf_urma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid)
{
	return nvmf_urma_fill_trid(nvmf_urma_qpair(qpair), trid, false);
}

static void
nvmf_urma_discover(struct spdk_nvmf_transport *transport,
		    struct spdk_nvme_transport_id *trid,
		    struct spdk_nvmf_discovery_log_page_entry *entry)
{
	/* URMA does not have an assigned 8-bit NVMe-oF TRTYPE yet.  Use the
	 * vendor-specific/reserved value until the standards allocation lands. */
	entry->trtype = 0xff;
	entry->adrfam = trid->adrfam;
	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');
	memcpy(entry->tsas.raw, "URMA", 4);
}
static void nvmf_urma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvmf_request *req)
{
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	spdk_nvmf_request_complete(req);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_urma = {
	.name = "URMA",
	.type = SPDK_NVME_TRANSPORT_URMA,
	.opts_init = nvmf_urma_opts_init,
	.create = nvmf_urma_create,
	.dump_opts = nvmf_urma_dump_opts,
	.destroy = nvmf_urma_destroy,
	.listen = nvmf_urma_listen,
	.stop_listen = nvmf_urma_stop_listen,
	.listener_discover = nvmf_urma_discover,
	.poll_group_create = nvmf_urma_poll_group_create,
	.poll_group_destroy = nvmf_urma_poll_group_destroy,
	.poll_group_add = nvmf_urma_poll_group_add,
	.poll_group_remove = nvmf_urma_poll_group_remove,
	.poll_group_poll = nvmf_urma_poll_group_poll,
	.req_free = nvmf_urma_req_free,
	.req_complete = nvmf_urma_req_complete,
	.req_get_buffers_done = nvmf_urma_req_get_buffers_done,
	.qpair_fini = nvmf_urma_qpair_fini,
	.qpair_get_local_trid = nvmf_urma_qpair_get_local_trid,
	.qpair_get_peer_trid = nvmf_urma_qpair_get_peer_trid,
	.qpair_get_listen_trid = nvmf_urma_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_urma_qpair_abort_request,
};

SPDK_NVMF_TRANSPORT_REGISTER(urma, &spdk_nvmf_transport_urma);
SPDK_LOG_REGISTER_COMPONENT(nvmf_urma)
