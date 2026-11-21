/* SPDX-License-Identifier: BSD-3-Clause */
/* This fixture runs the real target data path and initiator SEND lifetime code. */
#include "nvme_urma_internal.h"
#include "spdk/config.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk_internal/urma_lifetime.h"

static unsigned registrations, context_exports, context_releases, imports;
static char diagnostics[16384];
static int repost_result;
static unsigned disconnect_count, response_releases;
static bool strict_bond_response;
static unsigned unregistrations, fail_registration_at;
static struct spdk_thread *current_thread;
static spdk_msg_fn pending_msg;
static void *pending_msg_arg;
struct spdk_thread *spdk_get_thread(void) { return current_thread; }
int spdk_thread_send_msg(const struct spdk_thread *t, spdk_msg_fn fn, void *arg) {
	assert(pending_msg == NULL);
	pending_msg = fn;
	pending_msg_arg = arg;
	return 0;
}
static uint8_t export_marker = 0xac, local_chip = 1;
static bool require_context = true, use_cache = true, post_error;
static uint64_t last_user_ctx, last_send_addr;
static uint32_t last_length, last_src_chip, last_dst_chip;
static bool last_inline, last_affinity;
static unsigned char last_bytes[sizeof(struct spdk_urma_capsule_cmd_frame)];

void
spdk_log(enum spdk_log_level l, const char *f, const int n, const char *fn, const char *fmt, ...)
{
	va_list args;
	size_t used = strlen(diagnostics);
	va_start(args, fmt);
	vsnprintf(diagnostics + used, sizeof(diagnostics) - used, fmt, args);
	va_end(args);
}
uint64_t
spdk_get_ticks(void)
{
	static uint64_t ticks;
	return ++ticks;
}
uint8_t
__wrap_spdk_urma_memory_chip_id(const void *p)
{
	return local_chip;
}
urma_target_seg_t *
urma_register_seg(urma_context_t *c, urma_seg_cfg_t *cfg)
{
	if (fail_registration_at == registrations + 1) {
		errno = EIO;
		return NULL;
	}
	urma_target_seg_t *s = calloc(1, sizeof(*s));
	assert(s);
	s->seg.ubva.va = cfg->va;
	s->seg.len = cfg->len;
	s->seg.token_id = ++registrations;
	s->seg.ubva.eid = c->eid;
	return s;
}
urma_status_t
urma_unregister_seg(urma_target_seg_t *s)
{
	unregistrations++;
	free(s);
	return URMA_SUCCESS;
}
urma_status_t
urma_get_seg_ctx(urma_target_seg_t *s, urma_seg_t **out, uint32_t *size)
{
	*size = sizeof(**out) + 64;
	*out = malloc(*size);
	assert(*out);
	memcpy(*out, &s->seg, sizeof(**out));
	(*out)->attr.value |= 0x80000000u;
	memset(*out + 1, export_marker, 64);
	context_exports++;
	return URMA_SUCCESS;
}
void
urma_put_seg_ctx(urma_seg_t *s)
{
	context_releases++;
	free(s);
}
struct mock_import {
	urma_target_seg_t seg;
	const urma_seg_t *retained;
	uint8_t marker;
	bool extended;
};
urma_target_seg_t *
urma_import_seg(urma_context_t *c, urma_seg_t *s, urma_token_t *t, uint64_t va,
		urma_import_seg_flag_t f)
{
	bool extended = (s->attr.value & 0x80000000u) != 0;
	if (require_context && !extended) {
		errno = EIO;
		return NULL;
	}
	struct mock_import *m = calloc(1, sizeof(*m));
	assert(m);
	m->seg.seg = *s;
	m->retained = s;
	m->extended = extended;
	if (extended) {
		const uint8_t *p = (const uint8_t *)(s + 1);
		m->marker = p[0];
		for (unsigned i = 0; i < 64; i++)
			assert(p[i] == m->marker);
	}
	imports++;
	return &m->seg;
}
urma_status_t
urma_unimport_seg(urma_target_seg_t *s)
{
	struct mock_import *m = (void *)s;
	if (m->extended) {
		const uint8_t *p = (const uint8_t *)(m->retained + 1);
		for (unsigned i = 0; i < 64; i++)
			assert(p[i] == m->marker);
	}
	free(m);
	return URMA_SUCCESS;
}
urma_status_t
urma_post_jetty_send_wr(urma_jetty_t *j, urma_jfs_wr_t *w, urma_jfs_wr_t **bad)
{
	if (post_error)
		return URMA_FAIL;
	if (w->opcode == URMA_OPC_SEND) {
		const urma_sge_t *s = w->send.src.sge;
		last_length = s->len;
		last_send_addr = s->addr;
		last_inline = w->flag.bs.inline_flag;
		last_user_ctx = w->user_ctx;
		assert(s->len <= sizeof(last_bytes));
		memcpy(last_bytes, (void *)s->addr, s->len);
		if (strict_bond_response && (last_inline || s->tseg == NULL)) {
			*bad = w;
			return URMA_EINVAL;
		}
		if (!last_inline) {
			assert(s->tseg != NULL);
			assert(s->addr >= s->tseg->seg.ubva.va);
			assert(s->addr - s->tseg->seg.ubva.va <= s->tseg->seg.len);
			assert(s->len <= s->tseg->seg.len - (s->addr - s->tseg->seg.ubva.va));
		}
	} else {
		last_affinity = w->flag.bs.has_drv_ext;
		if (last_affinity) {
			const bondp_jfs_wr_t *a = (void *)w;
			last_src_chip = a->src_chip_id;
			last_dst_chip = a->dst_chip_id;
		}
	}
	return URMA_SUCCESS;
}

#define NVMF_URMA_IMPORT_BUCKETS 256
#define NVMF_URMA_REG_CACHE_SIZE 128
#define NVMF_URMA_TGT_STAGE(a, b) ((void)(b))
#define NVMF_URMA_TGT_ADD(a, b) ((void)(b))
#define NVMF_URMA_TGT_INC(a) ((void)0)
struct nvmf_urma_import_entry {
	urma_seg_t seg;
	void *context;
	uint32_t context_length;
	urma_target_seg_t *tseg;
	LIST_ENTRY(nvmf_urma_import_entry) link;
};
LIST_HEAD(nvmf_urma_import_bucket, nvmf_urma_import_entry);
struct nvmf_urma_transport {
	pthread_rwlock_t import_cache_lock;
	struct nvmf_urma_import_bucket import_cache[NVMF_URMA_IMPORT_BUCKETS];
};
struct nvmf_urma_reg_entry {
	void *va;
	size_t len;
	struct spdk_nvme_urma_memory_region *region;
	bool used;
};
enum { NVMF_URMA_CQE_CAPSULE_TX = 1, NVMF_URMA_CQE_CAPSULE_RX, NVMF_URMA_CQE_DATA, NVMF_URMA_CQE_BUFFERED_TX };
struct nvmf_urma_cqe_ctx {
	int type;
	void *owner;
};
struct nvmf_urma_rsp_tx_slot {
	struct nvmf_urma_cqe_ctx cqe;
	struct nvmf_urma_qpair *qpair;
	bool busy;
	struct spdk_urma_capsule_rsp_frame frame;
};
struct nvmf_urma_qpair {
	struct spdk_thread *thread;
	char peer_addr[64], service[16];
	int fd;
	uint64_t pending_peek_tick;
	struct {
		uint32_t qid, sq_head_max;
	} qpair;
	struct spdk_urma_device *device;
	struct nvmf_urma_transport *transport;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	struct spdk_urma_lifetime lifetime;
	struct nvmf_urma_reg_entry reg_cache[128];
	uint32_t next_jetty, jetty_count;
	urma_jfr_t **jfrs;
	enum spdk_urma_capsule_transport capsule_transport;
	struct nvmf_urma_cqe_ctx capsule_tx_cqe;
	struct nvmf_urma_cmd_rx_slot *capsule_rx_slots;
	struct spdk_nvme_urma_memory_region *capsule_rx_region, *capsule_tx_region;
	struct nvmf_urma_rsp_tx_slot *capsule_tx_slots;
	uint32_t capsule_rx_count, capsule_tx_count, next_tx_slot, resource_count, pending_responses;
	TAILQ_HEAD(, nvmf_urma_req) working_reqs;
};
struct nvmf_urma_req {
	struct {
		void *qpair;
		int iovcnt;
		struct iovec *iov;
		uint32_t length;
	} req;
	struct spdk_urma_data_desc remote_data;
	urma_target_seg_t *remote_seg;
	bool remote_seg_cached;
	void *remote_seg_handle;
	uint32_t remote_seg_ctx_length;
	uint8_t remote_seg_ctx[SPDK_URMA_MAX_SEG_CTX_SIZE];
	struct spdk_nvme_urma_memory_region *local_region;
	bool local_region_external;
	struct nvmf_urma_reg_entry *cache_entry;
	int state;
	struct nvmf_urma_cqe_ctx cqe;
	union { struct spdk_nvme_cpl nvme_cpl; } rsp;
	union { struct spdk_nvme_cmd nvme_cmd; } cmd;
	uint64_t start_tick;
	uint32_t trace_idx;
	TAILQ_ENTRY(nvmf_urma_req) link;
	uint64_t exec_tick;
	uint64_t post_tick;
	struct spdk_urma_stage_trace stage_trace;
};
enum { NVMF_URMA_REQ_PUSHING, NVMF_URMA_REQ_PULLING, NVMF_URMA_REQ_EXECUTING, NVMF_URMA_REQ_WAIT_TX };
static struct nvmf_urma_qpair *
nvmf_urma_qpair(void *p)
{
	return p;
}
static int
nvmf_urma_import_cache_enabled(void)
{
	return use_cache;
}
struct nvmf_urma_cmd_rx_slot {
	struct nvmf_urma_cqe_ctx cqe;
	struct nvmf_urma_qpair *qpair;
	struct spdk_urma_capsule_cmd_frame frame;
	struct spdk_urma_rx_ref rx_ref;
	urma_sge_t sge;
	urma_jfr_wr_t wr;
};
static struct spdk_urma_capsule_cmd parsed;
static unsigned parsed_count;
static int
nvmf_urma_post_cmd_receive(struct nvmf_urma_cmd_rx_slot *slot)
{
	/* A repost may permit immediate DMA overwrite of the same receive buffer. */
	memset(&slot->frame, 0, sizeof(slot->frame));
	return 0;
}
static int
nvmf_urma_handle_capsule(struct nvmf_urma_qpair *q, const struct spdk_urma_msg_hdr *h,
			 const struct spdk_urma_capsule_cmd *c, uint64_t a, uint64_t b)
{
	assert(spdk_urma_validate_cmd(h, c) == 0);
	memcpy(&parsed, c, h->length);
	parsed_count++;
	return 1;
}
static void nvmf_urma_disconnect_qpair(struct nvmf_urma_qpair *q) { disconnect_count++; }
static int nvmf_urma_send_response(struct nvmf_urma_req *r);
static void nvmf_urma_release_req(struct nvmf_urma_req *r) {
	response_releases++;
	if (r->state == NVMF_URMA_REQ_WAIT_TX) {
		struct nvmf_urma_qpair *q = r->req.qpair;
		assert(q->pending_responses > 0);
		q->pending_responses--;
	}
	if (r->link.tqe_prev != NULL) {
		TAILQ_REMOVE(&((struct nvmf_urma_qpair *)r->req.qpair)->working_reqs, r, link);
		r->link.tqe_prev = NULL;
	}
	r->state = 0;
}
#define NVMF_URMA_RX_TRACE_SIZE 1
static struct { uint64_t rsp_tick; } g_rx_trace[NVMF_URMA_RX_TRACE_SIZE];
#define spdk_nvmf_request_exec(r) ((void)(r))
struct nvmf_urma_completion_msg { urma_cr_t completion; };
/* REAL_TARGET_DATA_FUNCTIONS */

enum nvme_urma_cqe_type {
	NVME_URMA_CQE_CAPSULE_TX = 1,
	NVME_URMA_CQE_CAPSULE_RX,
	NVME_URMA_CQE_BUFFERED_TX
};
struct nvme_urma_cqe_ctx {
	enum nvme_urma_cqe_type type;
	void *owner;
};
struct nvme_urma_qpair;
struct nvme_urma_cmd_tx_slot {
	struct nvme_urma_cqe_ctx cqe;
	struct nvme_urma_qpair *qpair;
	bool busy;
	struct spdk_urma_capsule_cmd_frame frame;
};
struct nvme_urma_rsp_rx_slot {
	struct nvme_urma_cqe_ctx cqe;
	struct nvme_urma_qpair *qpair;
	struct spdk_urma_capsule_rsp_frame frame;
};
struct nvme_urma_qpair {
	struct {
		uint32_t id, queue_depth;
	} qpair;
	struct spdk_urma_device *device;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	uint32_t jetty_count, next_jetty, send_jfc_index, num_entries;
	struct spdk_urma_lifetime lifetime;
	struct nvme_urma_cqe_ctx capsule_tx_cqe;
	struct nvme_urma_cmd_tx_slot *capsule_tx_slots;
	struct spdk_nvme_urma_memory_region *capsule_tx_region;
	uint32_t capsule_tx_slots_cap, next_tx_slot;
	int deferred_error;
};
static struct {
	uint64_t compl_ticks, compl_count;
} g_timing;
static int
nvme_urma_post_rsp_receive(struct nvme_urma_rsp_rx_slot *s)
{
	return repost_result;
}
static int
nvme_urma_queue_response(struct nvme_urma_qpair *q, const struct spdk_urma_capsule_rsp *s)
{
	assert(false);
	return -1;
}
static int
nvme_urma_complete_response(struct nvme_urma_qpair *q, const struct spdk_urma_capsule_rsp *s)
{
	assert(false);
	return -1;
}
/* REAL_INITIATOR_SEND_FUNCTIONS */

static void
request_done(struct nvmf_urma_req *q)
{
	if (!q->remote_seg_cached) {
		urma_unimport_seg(q->remote_seg);
		free(q->remote_seg_handle);
	}
	spdk_urma_lifetime_put(&((struct nvmf_urma_qpair *)q->req.qpair)->lifetime);
}
static void
target_data_test(void)
{
	urma_context_t ctx = {0};
	ctx.eid.raw[0] = 1;
	void *remote = aligned_alloc(4096, 4096), *local = aligned_alloc(4096, 4096);
	assert(remote && local);
	struct spdk_nvme_urma_memory_region *region, *local_region;
	assert(spdk_nvme_urma_register_memory(&ctx, remote, 4096, SPDK_NVME_URMA_MEM_HOST,
					      &region) == 0);
	assert(spdk_nvme_urma_register_memory(&ctx, local, 4096, SPDK_NVME_URMA_MEM_HOST,
					      &local_region) == 0);
	unsigned before = context_exports;
	size_t length = 0;
	assert(spdk_nvme_urma_memory_region_export(region, NULL, &length) == 0 &&
	       length == sizeof(urma_seg_t) + 64);
	uint8_t blob[SPDK_URMA_MAX_SEG_CTX_SIZE];
	size_t cap = sizeof(blob);
	assert(spdk_nvme_urma_memory_region_export(region, blob, &cap) == 0 && cap == length &&
	       context_exports == before);
	cap = length - 1;
	assert(spdk_nvme_urma_memory_region_export(region, blob, &cap) == -ENOSPC && cap == length);
	assert(context_exports == context_releases);
	struct spdk_urma_device d = {0};
	strcpy(d.dev_name, "bonding_dev_75");
	d.context = &ctx;
	d.opts.numa_affinity = true;
	struct nvmf_urma_transport t = {0};
	assert(pthread_rwlock_init(&t.import_cache_lock, NULL) == 0);
	urma_jetty_t *jettys[1] = {0};
	urma_target_jetty_t *targets[1] = {0};
	struct nvmf_urma_qpair qp = {.device = &d,
				     .transport = &t,
				     .jettys = jettys,
				     .target_jettys = targets,
				     .jetty_count = 1};
	spdk_urma_lifetime_init(&qp.lifetime);
	struct iovec iov = {local, 4096};
	assert(spdk_nvme_urma_region_registry_add(&ctx, local, 4096, local_region) == 0);
	struct nvmf_urma_req req = {0};
	req.req.qpair = &qp;
	req.req.iovcnt = 1;
	req.req.iov = &iov;
	req.req.length = 4096;
	req.remote_data.seg = spdk_urma_memory_region_get_tseg(region)->seg;
	req.remote_data.address = (uint64_t)remote;
	req.remote_data.length = 4096;
	req.remote_data.chip_id = 2;
	req.remote_seg_ctx_length = length;
	memcpy(req.remote_seg_ctx, blob, length);
	unsigned first = imports;
	assert(nvmf_urma_post_data(&req, false) == 0 && imports == first + 1);
	assert(last_affinity && last_src_chip == 1 && last_dst_chip == 2);
	request_done(&req);
	assert(nvmf_urma_post_data(&req, true) == 0 && imports == first + 1);
	assert(last_affinity && last_src_chip == 1 && last_dst_chip == 2);
	request_done(&req);
	d.opts.electrical_network = true;
	assert(nvmf_urma_post_data(&req, true) == 0);
	assert(last_src_chip == 2 && last_dst_chip == 2);
	request_done(&req);
	d.opts.electrical_network = false;
	local_chip = SPDK_URMA_INVALID_CHIP_ID;
	assert(nvmf_urma_post_data(&req, false) == 0 && !last_affinity);
	request_done(&req);
	local_chip = 1;
	memset(req.remote_seg_ctx + sizeof(urma_seg_t), 0xbd, 64);
	assert(nvmf_urma_post_data(&req, false) == 0 && imports == first + 2);
	request_done(&req);
	use_cache = false;
	assert(nvmf_urma_post_data(&req, false) == 0 && imports == first + 3);
	request_done(&req);
	use_cache = true;
	void *retained;
	urma_target_seg_t *target;
	struct spdk_urma_data_desc bad = req.remote_data;
	assert(spdk_urma_import_segment(&d, &bad, blob, 0, &target, &retained) == -EPROTO);
	bad.address--;
	assert(spdk_urma_import_segment(&d, &bad, blob, length, &target, &retained) == -EPROTO);
	bad = req.remote_data;
	bad.length = 4097;
	assert(spdk_urma_import_segment(&d, &bad, blob, length, &target, &retained) == -EPROTO);
	bad = req.remote_data;
	bad.seg.token_id++;
	assert(spdk_urma_import_segment(&d, &bad, blob, length, &target, &retained) == -EPROTO);
	use_cache = false;
	require_context = false;
	strcpy(d.dev_name, "udmac0d1e3");
	req.remote_seg_ctx_length = 0;
	assert(nvmf_urma_post_data(&req, false) == 0 && !last_affinity);
	request_done(&req);
	use_cache = true;
	require_context = true;
	nvmf_urma_import_cache_fini(&t);
	pthread_rwlock_destroy(&t.import_cache_lock);
	spdk_nvme_urma_unregister_memory(local_region);
	spdk_nvme_urma_unregister_memory(region);
	free(local);
	free(remote);
}
static void
decode_test(void)
{
	struct spdk_urma_capsule_cmd_frame f = {0};
	f.hdr.magic = SPDK_URMA_WIRE_MAGIC;
	f.hdr.version = SPDK_URMA_WIRE_VERSION;
	f.hdr.type = SPDK_URMA_MSG_CAPSULE_CMD;
	f.capsule.data.length = 4096;
	f.capsule.seg_ctx_length = sizeof(urma_seg_t) + 64;
	memset(f.capsule.seg_ctx, 0x7a, f.capsule.seg_ctx_length);
	f.hdr.length = SPDK_URMA_CMD_BASE_SIZE + f.capsule.seg_ctx_length;
	size_t size = sizeof(f.hdr) + f.hdr.length;
	struct nvmf_urma_qpair q = {0};
	spdk_urma_lifetime_init(&q.lifetime);
	struct nvmf_urma_cmd_rx_slot slot = {.qpair = &q};
	memcpy(&slot.frame, &f, size);
	urma_cr_t cr = {0};
	cr.flag.bs.s_r = 1;
	cr.opcode = URMA_CR_OPC_SEND;
	cr.completion_len = size;
	unsigned before = parsed_count;
	assert(nvmf_urma_process_cmd_receive(&slot, &cr) == 1 && parsed_count == before + 1);
	assert(parsed.seg_ctx_length == f.capsule.seg_ctx_length &&
	       memcmp(parsed.seg_ctx, f.capsule.seg_ctx, parsed.seg_ctx_length) == 0);
	int fd[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	q.fd = fd[1];
	assert(send(fd[0], &f, size - 1, 0) == (ssize_t)size - 1);
	assert(nvmf_urma_receive_capsule(&q) == 0);
	assert(send(fd[0], ((uint8_t *)&f) + size - 1, 1, 0) == 1);
	assert(nvmf_urma_receive_capsule(&q) == 1);
	assert(parsed.seg_ctx_length == f.capsule.seg_ctx_length &&
	       memcmp(parsed.seg_ctx, f.capsule.seg_ctx, parsed.seg_ctx_length) == 0);
	f.hdr.length = sizeof(f.capsule) + 1;
	assert(send(fd[0], &f.hdr, sizeof(f.hdr), 0) == sizeof(f.hdr));
	assert(nvmf_urma_receive_capsule(&q) == -EPROTO);
	close(fd[0]);
	close(fd[1]);
}
static void
send_test(void)
{
	urma_context_t ctx = {0};
	struct spdk_urma_device d = {.context = &ctx};
	strcpy(d.dev_name, "bonding_dev_75");
	urma_jetty_t *jettys[1] = {0};
	urma_target_jetty_t *targets[1] = {0};
	struct nvme_urma_qpair q = {.device = &d,
				    .jettys = jettys,
				    .target_jettys = targets,
				    .jetty_count = 1,
				    .num_entries = 1};
	q.capsule_tx_cqe.type = NVME_URMA_CQE_CAPSULE_TX;
	q.capsule_tx_cqe.owner = &q;
	spdk_urma_lifetime_init(&q.lifetime);
	struct spdk_urma_capsule_cmd_frame frame = {0};
	frame.hdr.magic = SPDK_URMA_WIRE_MAGIC;
	frame.hdr.version = SPDK_URMA_WIRE_VERSION;
	frame.hdr.type = SPDK_URMA_MSG_CAPSULE_CMD;
	frame.hdr.length = SPDK_URMA_CMD_BASE_SIZE;
	assert(spdk_urma_validate_cmd(&frame.hdr, &frame.capsule) == 0);
	assert(nvme_urma_send_cmd_capsule(&q, &frame) == 0 && last_inline &&
	       last_length == SPDK_URMA_CMD_BASE_FRAME_SIZE);
	uint32_t done = 0;
	urma_cr_t cr = {0};
	cr.user_ctx = last_user_ctx;
	cr.status = URMA_CR_SUCCESS;
	assert(nvme_urma_process_capsule_cr(&q, &cr, &done) == 0 &&
	       spdk_urma_lifetime_refs(&q.lifetime) == 0);
	frame.capsule.data.length = 4096;
	frame.capsule.seg_ctx_length = sizeof(urma_seg_t) + 64;
	memset(frame.capsule.seg_ctx, 0x5e, frame.capsule.seg_ctx_length);
	frame.hdr.length = SPDK_URMA_CMD_BASE_SIZE + frame.capsule.seg_ctx_length;
	assert(spdk_urma_validate_cmd(&frame.hdr, &frame.capsule) == 0);
	assert(nvme_urma_send_cmd_capsule(&q, &frame) == 0 && !last_inline &&
	       last_length == sizeof(frame.hdr) + frame.hdr.length);
	assert(last_send_addr != (uint64_t)&frame &&
	       memcmp(last_bytes, (void *)last_send_addr, last_length) == 0);
	uint64_t tx = last_user_ctx;
	assert(nvme_urma_send_cmd_capsule(&q, &frame) == -EAGAIN);
	struct nvme_urma_qpair other = {0};
	spdk_urma_lifetime_init(&other.lifetime);
	cr.user_ctx = tx;
	assert(nvme_urma_process_capsule_cr(&other, &cr, &done) == 0 &&
	       spdk_urma_lifetime_refs(&q.lifetime) == 0);
	assert(nvme_urma_send_cmd_capsule(&q, &frame) == 0);
	cr.user_ctx = last_user_ctx;
	cr.status = URMA_CR_WR_FLUSH_ERR;
	assert(nvme_urma_process_capsule_cr(&other, &cr, &done) == 0 && q.deferred_error == -EIO &&
	       !q.capsule_tx_slots[0].busy);
	assert(strstr(diagnostics, "role=initiator stage=cr_error"));
	q.deferred_error = 0;
	post_error = true;
	assert(nvme_urma_send_cmd_capsule(&q, &frame) == -EIO && !q.capsule_tx_slots[0].busy);
	post_error = false;
	assert(nvme_urma_send_cmd_capsule(&q, &frame) == 0);
	cr.user_ctx = last_user_ctx;
	spdk_urma_lifetime_close(&q.lifetime);
	assert(nvme_urma_process_capsule_cr(&other, &cr, &done) == 0 &&
	       !q.capsule_tx_slots[0].busy && spdk_urma_lifetime_refs(&q.lifetime) == 0);
	frame.hdr.length--;
	assert(spdk_urma_validate_cmd(&frame.hdr, &frame.capsule) == -EPROTO);
	frame.capsule.seg_ctx_length = SPDK_URMA_MAX_SEG_CTX_SIZE + 1;
	assert(spdk_urma_validate_cmd(&frame.hdr, &frame.capsule) == -EPROTO);
	spdk_nvme_urma_unregister_memory(q.capsule_tx_region);
	free(q.capsule_tx_slots);
}
static void
diagnostics_test(void)
{
	struct nvmf_urma_qpair target = {0};
	struct nvmf_urma_cqe_ctx ctx = {NVMF_URMA_CQE_CAPSULE_TX, &target};
	urma_cr_t cr = {.status = URMA_CR_SUCCESS, .user_ctx = (uint64_t)&ctx};
	disconnect_count = 0;
	diagnostics[0] = 0;
	assert(nvmf_urma_handle_completion_inner(&cr) == 1 && disconnect_count == 0);
	assert(!strstr(diagnostics, "tx_direction"));
	cr.flag.bs.s_r = 1;
	assert(nvmf_urma_handle_completion_inner(&cr) == 1 && disconnect_count == 1);
	assert(strstr(diagnostics, "role=target stage=tx_direction"));
	cr.status = URMA_CR_WR_FLUSH_ERR;
	assert(nvmf_urma_handle_completion_inner(&cr) == 0 && disconnect_count == 2);
	assert(strstr(diagnostics, "role=target stage=cr_error"));

	struct nvme_urma_qpair owner = {0}, poller = {0};
	spdk_urma_lifetime_init(&owner.lifetime);
	struct nvme_urma_cqe_ctx tx = {NVME_URMA_CQE_CAPSULE_TX, &owner};
	cr = (urma_cr_t){.status = URMA_CR_SUCCESS, .user_ctx = (uint64_t)&tx};
	cr.flag.bs.s_r = 1;
	uint32_t completed = 0;
	bool transferred = false;
	diagnostics[0] = 0;
	assert(nvme_urma_process_capsule_cr_inner(&poller, &cr, &completed, &transferred) == 0);
	assert(owner.deferred_error == -ECONNRESET);
	assert(strstr(diagnostics, "role=initiator stage=tx_direction"));

	struct nvme_urma_rsp_rx_slot rx = {.qpair = &owner};
	rx.cqe.type = NVME_URMA_CQE_CAPSULE_RX;
	rx.cqe.owner = &rx;
	cr.user_ctx = (uint64_t)&rx.cqe;
	cr.opcode = URMA_CR_OPC_SEND;
	cr.completion_len = sizeof(rx.frame);
	owner.deferred_error = 0;
	repost_result = -EIO;
	diagnostics[0] = 0;
	assert(nvme_urma_process_capsule_cr_inner(&poller, &cr, &completed, &transferred) == 0);
	assert(owner.deferred_error == -EIO);
	assert(strstr(diagnostics, "stage=recv_repost"));
	assert(!strstr(diagnostics, "stage=cr_error"));
	repost_result = 0;
	puts("PASS real target TX-direction guard and CR errors, initiator owner routing and successful-CR receive-repost diagnostics");
}
static void
init_response_request(struct nvmf_urma_req *request, struct nvmf_urma_qpair *q, uint16_t cid)
{
	memset(request, 0, sizeof(*request));
	request->req.qpair = q;
	request->trace_idx = UINT32_MAX;
	request->cmd.nvme_cmd.cid = cid;
	request->cmd.nvme_cmd.opc = SPDK_NVME_OPC_FABRIC;
	request->state = NVMF_URMA_REQ_EXECUTING;
	TAILQ_INSERT_TAIL(&q->working_reqs, request, link);
}

static void
response_test(void)
{
	urma_context_t context = {0};
	struct spdk_urma_device device = {.context = &context};
	strcpy(device.dev_name, "bonding_dev_75");
	urma_jfr_t jfr = {.jfr_cfg = {.depth = 8}};
	urma_jfr_t *jfrs[] = {&jfr};
	urma_jetty_t *jettys[] = {NULL};
	urma_target_jetty_t *peers[] = {NULL};
	struct nvmf_urma_qpair q = {.device = &device, .jfrs = jfrs,
		.jettys = jettys, .target_jettys = peers, .jetty_count = 1,
		.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_URMA, .resource_count = 2};
	q.qpair.sq_head_max = 1;
	q.capsule_tx_cqe = (struct nvmf_urma_cqe_ctx){NVMF_URMA_CQE_CAPSULE_TX, &q};
	TAILQ_INIT(&q.working_reqs);
	spdk_urma_lifetime_init(&q.lifetime);
	assert(nvmf_urma_capsule_resources_init(&q) == 0);
	struct nvmf_urma_req request, second, waiting;
	init_response_request(&request, &q, 17);
	strict_bond_response = true;
	diagnostics[0] = 0;
	unsigned released = response_releases;
	int rc = nvmf_urma_send_response(&request);
	if (rc != 0) fprintf(stderr, "%s", diagnostics);
	assert(rc == 0 && !last_inline);
	assert(response_releases == released + 1);
	uint64_t first_ctx = last_user_ctx, first_addr = last_send_addr;
	assert(((struct spdk_urma_capsule_rsp_frame *)first_addr)->capsule.cpl.cid == 17);
	memset(&request, 0xa5, sizeof(request));
	assert(((struct spdk_urma_capsule_rsp_frame *)first_addr)->capsule.cpl.cid == 17);

	init_response_request(&second, &q, 18);
	assert(nvmf_urma_send_response(&second) == 0);
	uint64_t second_ctx = last_user_ctx;
	assert(first_ctx != second_ctx && first_addr != last_send_addr);
	assert(spdk_urma_lifetime_refs(&q.lifetime) == 2 && !nvmf_urma_qpair_drained(&q));
	init_response_request(&waiting, &q, 19);
	assert(nvmf_urma_send_response(&waiting) == 0);
	assert(q.pending_responses == 1 && response_releases == released + 2);
	assert(nvmf_urma_retry_responses(&q) == 0 && q.pending_responses == 1);

	/* A shared-JFC poller must retain both the slot and qpair until the
	 * completion message actually runs on this qpair's owner thread. */
	q.thread = (void *)(uintptr_t)2;
	current_thread = (void *)(uintptr_t)1;
	urma_cr_t cr = {.status = URMA_CR_SUCCESS, .user_ctx = second_ctx};
	assert(nvmf_urma_process_completion(&cr) == 0 && pending_msg != NULL);
	assert(spdk_urma_lifetime_refs(&q.lifetime) == 2);
	assert(((struct nvmf_urma_rsp_tx_slot *)((struct nvmf_urma_cqe_ctx *)second_ctx)->owner)->busy);
	spdk_msg_fn fn = pending_msg;
	pending_msg = NULL;
	current_thread = q.thread;
	fn(pending_msg_arg);
	assert(spdk_urma_lifetime_refs(&q.lifetime) == 1);
	assert(nvmf_urma_retry_responses(&q) == 1 && q.pending_responses == 0);
	assert(response_releases == released + 3 && last_user_ctx == second_ctx);
	assert(((struct spdk_urma_capsule_rsp_frame *)first_addr)->capsule.cpl.cid == 17);
	assert(((struct spdk_urma_capsule_rsp_frame *)last_send_addr)->capsule.cpl.cid == 19);
	assert(spdk_urma_lifetime_refs(&q.lifetime) == 2);

	cr.user_ctx = first_ctx;
	assert(nvmf_urma_handle_completion(&cr) == 1);
	init_response_request(&request, &q, 20);
	post_error = true;
	assert(nvmf_urma_send_response(&request) == -EIO);
	assert(spdk_urma_lifetime_refs(&q.lifetime) == 1);
	post_error = false;
	init_response_request(&request, &q, 21);
	assert(nvmf_urma_send_response(&request) == 0);
	uint64_t final_ctx = last_user_ctx;
	init_response_request(&waiting, &q, 22);
	assert(nvmf_urma_send_response(&waiting) == 0 && q.pending_responses == 1);
	spdk_urma_lifetime_close(&q.lifetime);
	assert(nvmf_urma_retry_responses(&q) == 1 && q.pending_responses == 0);
	assert(TAILQ_EMPTY(&q.working_reqs) && !nvmf_urma_qpair_drained(&q));
	cr.status = URMA_CR_WR_FLUSH_ERR;
	cr.user_ctx = second_ctx;
	assert(nvmf_urma_handle_completion(&cr) == 0);
	cr.user_ctx = final_ctx;
	assert(nvmf_urma_handle_completion(&cr) == 0);
	assert(spdk_urma_lifetime_refs(&q.lifetime) == 0 && nvmf_urma_qpair_drained(&q));
	unsigned before_unreg = unregistrations;
	nvmf_urma_capsule_resources_fini(&q);
	assert(unregistrations == before_unreg + 2);
	assert(q.capsule_tx_slots == NULL && q.capsule_tx_region == NULL);
	strict_bond_response = false;

	/* Partial initialization failure must release RX and TX allocations. */
	spdk_urma_lifetime_init(&q.lifetime);
	fail_registration_at = registrations + 2;
	before_unreg = unregistrations;
	assert(nvmf_urma_capsule_resources_init(&q) != 0);
	nvmf_urma_capsule_resources_fini(&q);
	assert(unregistrations == before_unreg + 1);
	fail_registration_at = 0;

	/* Raw retains the inline path; TCP retains its wire format and needs no
	 * registered response pool or SEND-completion lifetime reference. */
	strcpy(device.dev_name, "udmac0d1e3");
	assert(nvmf_urma_capsule_resources_init(&q) == 0 && q.capsule_tx_slots == NULL);
	q.qpair.qid = 3;
	init_response_request(&request, &q, 23);
	assert(nvmf_urma_send_response(&request) == 0 && last_inline);
	assert(last_user_ctx == (uint64_t)&q.capsule_tx_cqe);
	cr = (urma_cr_t){.status = URMA_CR_SUCCESS, .user_ctx = last_user_ctx};
	assert(nvmf_urma_handle_completion(&cr) == 1 && nvmf_urma_qpair_drained(&q));
	nvmf_urma_capsule_resources_fini(&q);
	int fds[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	q.fd = fds[0];
	q.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
	init_response_request(&request, &q, 24);
	assert(nvmf_urma_send_response(&request) == 0);
	struct spdk_urma_capsule_rsp_frame frame;
	assert(recv(fds[1], &frame, sizeof(frame), MSG_WAITALL) == sizeof(frame));
	assert(frame.hdr.magic == SPDK_URMA_WIRE_MAGIC && frame.hdr.qid == 3);
	assert(frame.capsule.cpl.cid == 24 && spdk_urma_lifetime_refs(&q.lifetime) == 0);
	close(fds[0]);
	close(fds[1]);
	puts("PASS real registered target responses, request reuse, out-of-order/cross-thread completions, pool backpressure, post/registration failure, close/flush, raw inline and TCP");
}
int
main(void)
{
	target_data_test();
	decode_test();
	send_test();
	diagnostics_test();
	response_test();
	puts("PASS full segment export/import lifetime, whole-pool cache identity, real "
	     "optical/electrical WRs, raw inline and non-inline SEND ownership");
	return 0;
}
