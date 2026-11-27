/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise production Identify preparation before a capsule reaches the target. */
#include "nvme_internal.h"
#include "nvme_urma_internal.h"
#include "spdk_internal/urma_lifetime.h"
#define URMA_STAGE_SIDE 1
#include "spdk_internal/urma_stage_buffer.h"

/* REAL_TYPES */
static char diagnostics[16384];
static int registration_errno;
static bool fail_registration, fail_export;
static unsigned registrations, unregistrations, duplicate_rejections, callbacks;
static bool reject_duplicate;
static bool fail_bounce_alloc;
static urma_target_seg_t *active_segments[256];

int __real_posix_memalign(void **ptr, size_t alignment, size_t size);
int
__wrap_posix_memalign(void **ptr, size_t alignment, size_t size)
{
	return fail_bounce_alloc ? ENOMEM : __real_posix_memalign(ptr, alignment, size);
}

void
spdk_log(enum spdk_log_level level, const char *file, int line, const char *func,
	 const char *format, ...)
{
	if (level != SPDK_LOG_ERROR)
		return; /* embedded master default */
	va_list ap;
	va_start(ap, format);
	size_t used = strlen(diagnostics);
	vsnprintf(diagnostics + used, sizeof(diagnostics) - used, format, ap);
	va_end(ap);
}
uint8_t
__wrap_spdk_urma_memory_chip_id(const void *p)
{
	return SPDK_URMA_INVALID_CHIP_ID;
}
uint64_t
spdk_get_ticks(void)
{
	static uint64_t tick;
	return ++tick;
}
uint64_t
spdk_get_ticks_hz(void)
{
	return 1000000;
}
struct spdk_memory_domain *
spdk_memory_domain_get_system_domain(void)
{
	return NULL;
}
const char *
spdk_memory_domain_get_dma_device_id(struct spdk_memory_domain *domain)
{
	return NULL;
}
static int
nvme_urma_trace_enabled(void)
{
	return 0;
}
static uint64_t
nvme_urma_stage_trace_threshold_ticks(void)
{
	return 0;
}
static int
nvme_urma_send_cmd_capsule(struct nvme_urma_qpair *q,
			   const struct spdk_urma_capsule_cmd_frame *frame)
{
	assert(false);
	return -EIO;
}

urma_target_seg_t *
urma_register_seg(urma_context_t *ctx, urma_seg_cfg_t *cfg)
{
	if (fail_registration) {
		if (registration_errno)
			errno = registration_errno;
		return NULL;
	}
	for (unsigned i = 0; reject_duplicate && i < SPDK_COUNTOF(active_segments); i++) {
		urma_target_seg_t *old = active_segments[i];
		if (old && old->urma_ctx == ctx && old->seg.ubva.va < cfg->va + cfg->len &&
		    cfg->va < old->seg.ubva.va + old->seg.len) {
			duplicate_rejections++;
			errno = EINVAL;
			return NULL;
		}
	}
	urma_target_seg_t *s = calloc(1, sizeof(*s));
	assert(s);
	s->urma_ctx = ctx;
	s->seg.ubva.va = cfg->va;
	s->seg.len = cfg->len;
	s->seg.token_id = ++registrations;
	for (unsigned i = 0; i < SPDK_COUNTOF(active_segments); i++) {
		if (!active_segments[i]) {
			active_segments[i] = s;
			return s;
		}
	}
	abort();
}
urma_status_t
urma_unregister_seg(urma_target_seg_t *seg)
{
	bool found = false;
	for (unsigned i = 0; i < SPDK_COUNTOF(active_segments); i++) {
		if (active_segments[i] == seg) {
			active_segments[i] = NULL;
			found = true;
			break;
		}
	}
	assert(found);
	unregistrations++;
	free(seg);
	return URMA_SUCCESS;
}
urma_status_t
urma_get_seg_ctx(urma_target_seg_t *seg, urma_seg_t **out, uint32_t *size)
{
	if (fail_export)
		return URMA_FAIL;
	*size = sizeof(**out) + 64;
	*out = calloc(1, *size);
	assert(*out);
	memcpy(*out, &seg->seg, sizeof(**out));
	return URMA_SUCCESS;
}
void
urma_put_seg_ctx(urma_seg_t *ctx)
{
	free(ctx);
}
/* REAL_FUNCTIONS */

static void
cleanup(struct nvme_urma_qpair *q)
{
	nvme_urma_qpair_abort_reqs(&q->qpair, 1);
	for (unsigned i = 0; i < NVME_URMA_REG_CACHE_SIZE; i++)
		if (q->reg_cache[i].used)
			spdk_nvme_urma_unregister_memory(q->reg_cache[i].region);
	free(q->cid_bitmap);
}
static void
identify_test(bool bond)
{
	urma_context_t context = {0};
	struct spdk_urma_device device = {.context = &context};
	strcpy(device.dev_name, bond ? "bonding_dev_14" : "udmac0d1e4");
	struct spdk_nvme_ctrlr ctrlr = {0};
	strcpy(ctrlr.trid.traddr, "192.168.65.20");
	strcpy(ctrlr.trid.trsvcid, "4420");
	int fds[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	struct nvme_urma_qpair q = {.device = &device,
				    .fd = fds[0],
				    .num_entries = 32,
				    .next_cid = 7,
				    .capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP};
	q.qpair.ctrlr = &ctrlr;
	q.qpair.id = bond ? 1 : 0;
	q.cid_bitmap = calloc(4, 1);
	TAILQ_INIT(&q.outstanding);
	void *buffer = aligned_alloc(4096, 4096);
	assert(buffer);
	struct nvme_request request = {.qpair_reserved = true};
	request.cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	request.payload.contig_or_cb_arg = buffer;
	request.payload.size = 4096;
	request.payload_type = NVME_PAYLOAD_TYPE_CONTIG;

	fail_registration = true;
	registration_errno = ENOENT;
	diagnostics[0] = 0;
	int rc = nvme_urma_qpair_submit_request(&q.qpair, &request);
	assert(rc == -ENOENT && q.qpair.queue_depth == 0 && TAILQ_EMPTY(&q.outstanding));
	assert(q.cid_bitmap[0] == 0);
	if (!strstr(diagnostics, "stage=memory_register")) {
		fprintf(stderr,
			"RED: Identify registration returned -2 without ERROR-level stage: %s\n",
			diagnostics);
		abort();
	}
	assert(strstr(diagnostics, "cid=7 opcode=6") && strstr(diagnostics, "data_len=4096"));
	char byte;
	assert(recv(fds[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);

	fail_registration = false;
	unsigned before = registrations;
	rc = nvme_urma_qpair_submit_request(&q.qpair, &request);
	if (rc != 0)
		fprintf(stderr, "retry rc=%d: %s\n", rc, diagnostics);
	assert(rc == 0);
	struct spdk_urma_capsule_cmd_frame frame;
	ssize_t n = recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT);
	assert(n >= (ssize_t)SPDK_URMA_CMD_BASE_FRAME_SIZE);
	assert(frame.capsule.cmd.opc == SPDK_NVME_OPC_IDENTIFY);
	assert(frame.capsule.data.length == 4096);
	assert((frame.capsule.seg_ctx_length > sizeof(urma_seg_t)) == bond);
	assert(((frame.capsule.data.flags & SPDK_URMA_DATA_TRANSIENT) != 0) == bond);
	assert(registrations == before + 1 && q.qpair.queue_depth == 1);
	struct nvme_request second = request;
	assert(nvme_urma_qpair_submit_request(&q.qpair, &second) == 0);
	assert(registrations == before + 1 && q.qpair.queue_depth == 2);
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	if (bond) {
		void *other = aligned_alloc(4096, 4096);
		assert(other);
		struct nvme_request failed = request;
		failed.payload.contig_or_cb_arg = other;
		fail_export = true;
		diagnostics[0] = 0;
		assert(nvme_urma_qpair_submit_request(&q.qpair, &failed) == -EIO);
		assert(strstr(diagnostics, "stage=segment_context"));
		assert(q.qpair.queue_depth == 2);
		assert(recv(fds[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
		fail_export = false;
		cleanup(&q);
		free(other);
	} else {
		cleanup(&q);
	}
	free(buffer);
	close(fds[0]);
	close(fds[1]);
}

static void
request_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	(void)cpl;
	if (arg) {
		*(unsigned *)arg = unregistrations;
	}
	callbacks++;
}

static void
complete_request(struct nvme_urma_qpair *q, struct nvme_request *request, bool error)
{
	struct spdk_urma_capsule_rsp response = {};

	response.cpl.cid = request->cmd.cid;
	response.cpl.sqid = q->qpair.id;
	response.cpl.status.sc = error ? SPDK_NVME_SC_INTERNAL_DEVICE_ERROR : 0;
	assert(nvme_urma_complete_response(q, &response) == 1);
}

static void
bond_admin_lifecycle_test(void)
{
	urma_context_t context = {};
	struct spdk_urma_device device = {.context = &context};
	struct spdk_nvme_ctrlr ctrlr = {};
	int fds[2];
	struct spdk_urma_capsule_cmd_frame frame;
	unsigned base_reg = registrations, base_unreg = unregistrations;
	unsigned base_callbacks = callbacks, callback_unregistrations = 0;

	strcpy(device.dev_name, "bonding_dev_15");
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	struct nvme_urma_qpair q = {.device = &device,
				    .fd = fds[0],
				    .num_entries = 32,
				    .capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP};
	q.qpair.ctrlr = &ctrlr;
	q.cid_bitmap = calloc(4, 1);
	assert(q.cid_bitmap);
	TAILQ_INIT(&q.outstanding);
	void *buffer = aligned_alloc(4096, 4096);
	assert(buffer);
	struct nvme_request connect = {.payload_type = NVME_PAYLOAD_TYPE_CONTIG,
				       .qpair_reserved = true,
				       .cb_fn = request_done,
				       .cb_arg = &callback_unregistrations};
	connect.cmd.opc = SPDK_NVME_OPC_FABRIC;
	((struct spdk_nvmf_fabric_connect_cmd *)&connect.cmd)->fctype =
	    SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	connect.payload.contig_or_cb_arg = buffer;
	connect.payload.size = 1024;
	reject_duplicate = true;
	assert(nvme_urma_qpair_submit_request(&q.qpair, &connect) == 0);
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.length == 1024 && frame.capsule.data.seg.len == 4096);
	assert(unregistrations == base_unreg); /* Keep the grant while DMA is in flight. */
	complete_request(&q, &connect, false);

	struct nvme_request identify = {.payload_type = NVME_PAYLOAD_TYPE_CONTIG,
					.qpair_reserved = true,
					.cb_fn = request_done,
					.cb_arg = &callback_unregistrations};
	identify.cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	identify.payload.contig_or_cb_arg = buffer; /* Deliberate allocator reuse. */
	identify.payload.size = 4096;
	int rc = nvme_urma_qpair_submit_request(&q.qpair, &identify);
	if (rc != 0) {
		fprintf(stderr,
			"RED: Connect completed but its grant remained; "
			"same-page Identify rejected rc=%d duplicate_rejections=%u\n",
			rc, duplicate_rejections);
		abort();
	}
	assert(duplicate_rejections == 0);
	assert(unregistrations == base_unreg + 1);
	assert(callback_unregistrations == base_unreg + 1);
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.length == 4096 && frame.capsule.data.seg.len == 4096);
	complete_request(&q, &identify, true); /* Error completions release too. */
	assert(unregistrations == base_unreg + 2);
	assert(callback_unregistrations == base_unreg + 2);
	assert(q.qpair.queue_depth == 0 && TAILQ_EMPTY(&q.outstanding));
	for (unsigned i = 0; i < NVME_URMA_REG_CACHE_SIZE; i++) {
		assert(!q.reg_cache[i].used);
	}

	/* Fail after registration/context export: no leaked region, CID or request. */
	q.fd = -1;
	diagnostics[0] = 0;
	assert(nvme_urma_qpair_submit_request(&q.qpair, &identify) == -EBADF);
	assert(strstr(diagnostics, "stage=capsule_send"));
	assert(unregistrations == base_unreg + 3);
	assert(q.qpair.queue_depth == 0 && TAILQ_EMPTY(&q.outstanding));
	q.fd = fds[0];
	fail_export = true;
	assert(nvme_urma_qpair_submit_request(&q.qpair, &identify) == -EIO);
	assert(unregistrations == base_unreg + 4);
	fail_export = false;
	assert(q.cid_bitmap[0] == 0 && q.cid_bitmap[1] == 0);

	/* A disconnect abort releases the payload before its completion callback. */
	assert(nvme_urma_qpair_submit_request(&q.qpair, &identify) == 0);
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(unregistrations == base_unreg + 4);
	nvme_urma_qpair_abort_reqs(&q.qpair, 1);
	assert(unregistrations == base_unreg + 5);
	assert(callback_unregistrations == base_unreg + 5);
	assert(callbacks == base_callbacks + 3);
	assert(q.qpair.queue_depth == 0 && TAILQ_EMPTY(&q.outstanding));

	/* An application-owned covering region retains its original ownership. */
	struct spdk_nvme_urma_memory_region *external = NULL;
	assert(spdk_nvme_urma_register_memory(&context, buffer, 4096, SPDK_NVME_URMA_MEM_HOST,
					      &external) == 0);
	assert(spdk_nvme_urma_region_registry_add(&context, buffer, 4096, external) == 0);
	unsigned external_reg = registrations, external_unreg = unregistrations;
	assert(nvme_urma_qpair_submit_request(&q.qpair, &identify) == 0);
	assert(registrations == external_reg);
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert((frame.capsule.data.flags & SPDK_URMA_DATA_TRANSIENT) == 0);
	complete_request(&q, &identify, false);
	assert(unregistrations == external_unreg);
	assert(callback_unregistrations == external_unreg);
	assert(spdk_nvme_urma_region_registry_find(&context, (uint64_t)buffer, 4096) == external);
	spdk_nvme_urma_unregister_memory(external);
	assert(!spdk_nvme_urma_region_registry_find(&context, (uint64_t)buffer, 4096));
	assert(registrations == base_reg + 6 && unregistrations == base_unreg + 6);
	reject_duplicate = false;
	cleanup(&q);
	free(buffer);
	close(fds[0]);
	close(fds[1]);
	puts("PASS bond Admin: same-page Connect->Identify, in-flight lifetime, completion/error/"
	     "abort cleanup, external ownership; no duplicate grant");
}

/* Both queue types use the same ephemeral Fabrics Connect allocation. Model
 * reuse after its completion, first by a 512-byte probe READ and then by a
 * different controller's Admin Connect sharing the same device context. */
static void
bond_io_connect_lifetime_test(bool cross_qpair)
{
	urma_context_t context = {};
	struct spdk_urma_device device = {.context = &context};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_urma_qpair io = {.device = &device,
				     .num_entries = 32,
				     .capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP};
	struct nvme_urma_qpair admin = {.device = &device,
					.num_entries = 32,
					.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP};
	struct spdk_urma_capsule_cmd_frame frame;
	unsigned base_reg = registrations, base_unreg = unregistrations;
	unsigned callback_unreg = 0;
	int io_fds[2], admin_fds[2];

	strcpy(device.dev_name, "bonding_dev_80");
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, io_fds) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, admin_fds) == 0);
	io.fd = io_fds[0];
	admin.fd = admin_fds[0];
	io.qpair.id = 1;
	io.qpair.ctrlr = admin.qpair.ctrlr = &ctrlr;
	io.cid_bitmap = calloc(4, 1);
	admin.cid_bitmap = calloc(4, 1);
	assert(io.cid_bitmap && admin.cid_bitmap);
	TAILQ_INIT(&io.outstanding);
	TAILQ_INIT(&admin.outstanding);
	void *buffer = aligned_alloc(4096, 4096);
	assert(buffer);
	struct nvme_request connect = {.payload_type = NVME_PAYLOAD_TYPE_CONTIG,
				       .qpair_reserved = true,
				       .cb_fn = request_done,
				       .cb_arg = &callback_unreg};
	connect.cmd.opc = SPDK_NVME_OPC_FABRIC;
	((struct spdk_nvmf_fabric_connect_cmd *)&connect.cmd)->fctype =
	    SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	connect.payload.contig_or_cb_arg = buffer;
	connect.payload.size = 1024;
	reject_duplicate = true;
	assert(nvme_urma_qpair_submit_request(&io.qpair, &connect) == 0);
	assert(recv(io_fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(unregistrations == base_unreg);
	complete_request(&io, &connect, false);

	struct nvme_request next = connect;
	struct nvme_urma_qpair *next_q = cross_qpair ? &admin : &io;
	int next_fd = cross_qpair ? admin_fds[1] : io_fds[1];
	if (!cross_qpair) {
		next.cmd.opc = SPDK_NVME_OPC_READ;
		next.payload.size = 512;
	}
	int rc = nvme_urma_qpair_submit_request(&next_q->qpair, &next);
	if (rc != 0) {
		fprintf(stderr,
			"RED: qid=1 Connect retained its 4KB grant; %s "
			"at the same VA rejected rc=%d\n",
			cross_qpair ? "another qpair's Connect" : "qid=1 READ(512B)", rc);
		abort();
	}
	assert(duplicate_rejections == 0);
	assert(unregistrations == base_unreg + 1 && callback_unreg == base_unreg + 1);
	assert(!nvme_urma_reg_cache_lookup(&io, buffer, 1024));
	assert(recv(next_fd, &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	complete_request(next_q, &next, false);
	if (cross_qpair) {
		assert(unregistrations == base_unreg + 2 && callback_unreg == base_unreg + 2);
		assert(!nvme_urma_reg_cache_lookup(&admin, buffer, 1024));
	} else {
		/* Ordinary HOST payloads now have request lifetime as well. */
		assert(!nvme_urma_reg_cache_lookup(&io, buffer, 512));
		assert(unregistrations == base_unreg + 2);
		assert(nvme_urma_qpair_submit_request(&io.qpair, &next) == 0);
		assert(registrations == base_reg + 3);
		assert(recv(io_fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
		complete_request(&io, &next, false);
		assert(unregistrations == base_unreg + 3);
	}
	cleanup(&io);
	cleanup(&admin);
	assert(registrations == base_reg + (cross_qpair ? 2 : 3));
	assert(registrations - base_reg == unregistrations - base_unreg);
	reject_duplicate = false;
	free(buffer);
	close(io_fds[0]);
	close(io_fds[1]);
	close(admin_fds[0]);
	close(admin_fds[1]);
	puts(cross_qpair ? "PASS bond I/O Connect -> another Admin Connect: shared-context VA reuse"
			 : "PASS bond I/O Connect -> 512B READ: both grants released "
			   "before callbacks");
}

/* Doc 74: basic reuses a 256KiB buffer on another lane after a completed PUT. */
static void
bond_data_lifetime_test(void)
{
	urma_context_t context = {};
	struct spdk_urma_device device = {.context = &context};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_urma_qpair qs[2] = {};
	struct spdk_urma_capsule_cmd_frame frame;
	int fds[2][2];
	unsigned base_reg = registrations, base_unreg = unregistrations;
	void *buffer = aligned_alloc(4096, 262144);

	assert(buffer);
	strcpy(device.dev_name, "bonding_dev_40");
	reject_duplicate = true;
	for (unsigned i = 0; i < 2; i++) {
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
		qs[i].device = &device;
		qs[i].fd = fds[i][0];
		qs[i].num_entries = 32;
		qs[i].capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
		qs[i].qpair.ctrlr = &ctrlr;
		qs[i].qpair.id = 2 + i;
		qs[i].cid_bitmap = calloc(4, 1);
		TAILQ_INIT(&qs[i].outstanding);
	}
	struct nvme_request req = {
		.qpair_reserved = true,
		.cb_fn = request_done,
		.payload_type = NVME_PAYLOAD_TYPE_CONTIG,
	};
	req.cmd.opc = SPDK_NVME_OPC_WRITE;
	req.payload.contig_or_cb_arg = buffer;
	req.payload.size = 262144;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &req) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	complete_request(&qs[0], &req, false);
	int rc = nvme_urma_qpair_submit_request(&qs[1].qpair, &req);
	if (rc != 0) {
		fprintf(stderr, "RED Doc74: completed qid=2 WRITE retains grant; "
			"qid=3 WRITE same 256KiB buffer rc=%d\n", rc);
		abort();
	}
	assert(recv(fds[1][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.flags & SPDK_URMA_DATA_TRANSIENT);
	complete_request(&qs[1], &req, false);
	assert(registrations == base_reg + 2 && unregistrations == base_unreg + 2);

	/* Two in-flight users must share ownership, including different logical
	 * lengths that round to the same 4KiB grant and a contained subrange. */
	struct nvme_request next = req;
	next.payload.offset = 64;
	next.payload.size = 512;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &req) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(nvme_urma_qpair_submit_request(&qs[1].qpair, &next) == 0);
	assert(recv(fds[1][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(registrations == base_reg + 3);
	complete_request(&qs[0], &req, false);
	assert(unregistrations == base_unreg + 2);
	nvme_urma_qpair_abort_reqs(&qs[1].qpair, 1);
	assert(unregistrations == base_unreg + 3);
	assert(duplicate_rejections == 0);
	for (unsigned i = 0; i < 2; i++) {
		cleanup(&qs[i]);
		close(fds[i][0]);
		close(fds[i][1]);
	}
	reject_duplicate = false;
	free(buffer);
	puts("PASS bond data: cross-qpair sequential VA reuse, covered concurrent IO, "
	     "final abort release");
}

/* Hold a small grant while a larger, partially overlapping request arrives. */
static void
bond_overlap_test(uint8_t opcode, unsigned failure)
{
	urma_context_t context = {};
	struct spdk_urma_device device = {.context = &context};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_urma_qpair qs[2] = {};
	struct spdk_urma_capsule_cmd_frame frame;
	int fds[2][2];
	unsigned before_reg = registrations, before_unreg = unregistrations;
	uint8_t *buffer = aligned_alloc(4096, 16384);

	assert(buffer);
	memset(buffer, 0x19, 16384);
	strcpy(device.dev_name, "bonding_dev_40");
	reject_duplicate = true;
	for (unsigned i = 0; i < 2; i++) {
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
		qs[i].device = &device;
		qs[i].fd = fds[i][0];
		qs[i].num_entries = 32;
		qs[i].capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
		qs[i].qpair.ctrlr = &ctrlr;
		qs[i].qpair.id = 2 + i;
		qs[i].cid_bitmap = calloc(4, 1);
		TAILQ_INIT(&qs[i].outstanding);
	}
	struct nvme_request first = {.qpair_reserved = true, .cb_fn = request_done,
		.payload_type = NVME_PAYLOAD_TYPE_CONTIG};
	first.cmd.opc = SPDK_NVME_OPC_WRITE;
	first.payload.contig_or_cb_arg = buffer;
	first.payload.size = 512;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &first) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	struct nvme_request next = first;
	next.cmd.opc = opcode;
	next.payload.offset = 64;
	next.payload.size = 8192;
	if (failure == 3) {
		fail_export = true;
	} else if (failure == 4) {
		assert(shutdown(fds[1][0], SHUT_WR) == 0);
	} else if (failure == 5) {
		fail_bounce_alloc = true;
	} else if (failure == 6) {
		fail_registration = true;
		registration_errno = ENOENT;
	}
	int rc = nvme_urma_qpair_submit_request(&qs[1].qpair, &next);
	fail_export = fail_bounce_alloc = fail_registration = false;
	registration_errno = 0;
	if (failure >= 3) {
		assert(rc != 0 && qs[1].qpair.queue_depth == 0);
		assert(qs[1].cid_bitmap[0] == 0 && TAILQ_EMPTY(&qs[1].outstanding));
		char byte;
		assert(recv(fds[1][1], &byte, 1, MSG_DONTWAIT) <= 0);
	} else {
		assert(rc == 0);
		assert(recv(fds[1][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
		assert(frame.capsule.data.address != (uint64_t)(buffer + 64));
		assert(frame.capsule.data.length == 8192);
		void *staged = (void *)(uintptr_t)frame.capsule.data.address;
		assert((uintptr_t)staged % 4096 == 0);
		if (opcode == SPDK_NVME_OPC_WRITE) {
			assert(memcmp(staged, buffer + 64, 8192) == 0);
		}
		memset(staged, 0x7b, 8192); /* Emulate a controller-to-host DMA. */
		if (failure == 2) {
			nvme_urma_qpair_abort_reqs(&qs[1].qpair, 1);
		} else {
			complete_request(&qs[1], &next, failure == 1);
		}
		for (unsigned i = 64; i < 64 + 8192; i++) {
			uint8_t expected = (opcode == SPDK_NVME_OPC_READ && failure == 0) ? 0x7b : 0x19;
			assert(buffer[i] == expected);
		}
		assert(buffer[63] == 0x19 && buffer[64 + 8192] == 0x19);
	}
	/* No temporary grant survives a rejected, completed or aborted request. */
	assert(registrations - before_reg == unregistrations - before_unreg + 1);
	complete_request(&qs[0], &first, false);
	for (unsigned i = 0; i < 2; i++) {
		cleanup(&qs[i]);
		close(fds[i][0]);
		close(fds[i][1]);
	}
	assert(registrations - before_reg == unregistrations - before_unreg);
	assert(duplicate_rejections == 0);
	reject_duplicate = false;
	free(buffer);
}

/* Doc 77: amortize registration across 32 sequential 256KiB children on
 * alternating lanes while preserving the application buffer's lifetime. */
static void
bond_staging_reuse_test(void)
{
	urma_context_t context = {};
	struct spdk_urma_device device = {.context = &context};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_urma_qpair qs[2] = {};
	struct spdk_urma_capsule_cmd_frame frame;
	unsigned base_reg = registrations, base_unreg = unregistrations;
	int fds[2][2];
	uint8_t *buffer = aligned_alloc(4096, 262144);

	assert(buffer);
	assert(setenv("SPDK_URMA_HOST_STAGING_SLOTS", "4", 1) == 0);
	assert(setenv("SPDK_URMA_HOST_STAGING_SLOT_BYTES", "262144", 1) == 0);
	strcpy(device.dev_name, "bonding_dev_40");
	reject_duplicate = true;
	for (unsigned i = 0; i < 2; i++) {
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
		qs[i].device = &device;
		qs[i].fd = fds[i][0];
		qs[i].num_entries = 32;
		qs[i].capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
		qs[i].qpair.ctrlr = &ctrlr;
		qs[i].qpair.id = 2 + i;
		qs[i].cid_bitmap = calloc(4, 1);
		TAILQ_INIT(&qs[i].outstanding);
	}
	struct nvme_request req = {
		.qpair_reserved = true, .cb_fn = request_done,
		.payload_type = NVME_PAYLOAD_TYPE_CONTIG,
	};
	req.cmd.opc = SPDK_NVME_OPC_READ;
	req.payload.contig_or_cb_arg = buffer;
	req.payload.size = 262144;
	for (unsigned i = 0; i < 32; i++) {
		unsigned lane = i % 2;
		assert(nvme_urma_qpair_submit_request(&qs[lane].qpair, &req) == 0);
		assert(recv(fds[lane][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
		memset((void *)(uintptr_t)frame.capsule.data.address, i, 262144);
		complete_request(&qs[lane], &req, false);
		for (unsigned j = 0; j < 262144; j++) {
			assert(buffer[j] == i);
		}
	}
	if (registrations != base_reg + 1 || unregistrations != base_unreg) {
		fprintf(stderr, "RED Doc77: 32 children caused %u registrations and %u "
			"unregistrations; expected one SPDK-owned persistent pool\n",
			registrations - base_reg, unregistrations - base_unreg);
		abort();
	}
	/* WRITE copies before submit; failed/aborted READs must not copy back. */
	struct spdk_urma_host_memory_stats memory_stats;
	spdk_urma_get_host_memory_stats(&memory_stats);
	assert(memory_stats.staging_hits == 32 && memory_stats.staging_registers == 1);
	assert(memory_stats.staging_unregisters == 0 && memory_stats.staging_releases == 32);
	memset(buffer, 0x73, 262144);
	req.cmd.opc = SPDK_NVME_OPC_WRITE;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &req) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.address != (uint64_t)buffer);
	assert(frame.capsule.data.flags == 0);
	assert(memcmp((void *)(uintptr_t)frame.capsule.data.address, buffer, 262144) == 0);
	complete_request(&qs[0], &req, false);
	struct nvme_request pending[5];
	uint8_t *buffers = aligned_alloc(4096, 5 * 262144);
	assert(buffers);
	memset(buffers, 0, 5 * 262144);
	for (unsigned i = 0; i < 5; i++) {
		pending[i] = req;
		pending[i].cmd.opc = SPDK_NVME_OPC_READ;
		pending[i].payload.contig_or_cb_arg = buffers + i * 262144;
		assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &pending[i]) == 0);
		assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
		assert((frame.capsule.data.flags == 0) == (i < 4));
		memset((void *)(uintptr_t)frame.capsule.data.address, i + 1, 262144);
	}
	assert(registrations == base_reg + 2); /* Only the fifth request falls back. */
#ifdef SPDK_URMA_HOST_STAGING_DEFAULT_SLOTS
	assert(!spdk_urma_host_staging_fini(&device));
#endif
	complete_request(&qs[0], &pending[4], false);
	complete_request(&qs[0], &pending[3], false);
	complete_request(&qs[0], &pending[2], false);
	complete_request(&qs[0], &pending[1], true);
	nvme_urma_qpair_abort_reqs(&qs[0].qpair, 1);
	for (unsigned i = 0; i < 5; i++) {
		for (unsigned j = 0; j < 262144; j++) {
			assert(buffers[i * 262144 + j] == (i < 2 ? 0 : i + 1));
		}
	}
	assert(unregistrations == base_unreg + 1);
	struct nvme_request subrange = req;
	subrange.cmd.opc = SPDK_NVME_OPC_READ;
	subrange.payload.contig_or_cb_arg = buffers;
	subrange.payload.offset = 64;
	subrange.payload.size = 8192;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &subrange) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	memset((void *)(uintptr_t)frame.capsule.data.address, 0x9c, 8192);
	complete_request(&qs[0], &subrange, false);
	assert(buffers[63] == 0 && buffers[64 + 8192] == 0);
	for (unsigned i = 64; i < 64 + 8192; i++) {
		assert(buffers[i] == 0x9c);
	}
	subrange.cmd.opc = SPDK_NVME_OPC_WRITE;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &subrange) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(memcmp((void *)(uintptr_t)frame.capsule.data.address, buffers + 64, 8192) == 0);
	complete_request(&qs[0], &subrange, false);
	/* Oversized payloads retain the transient path without enlarging the pool. */
	req.payload.contig_or_cb_arg = buffers;
	req.payload.size = 524288;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &req) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.flags & SPDK_URMA_DATA_TRANSIENT);
	complete_request(&qs[0], &req, false);
	/* Explicit registration still has priority and never gets copied. */
	struct spdk_nvme_urma_memory_region *external;
	assert(spdk_nvme_urma_register_memory(&context, buffer, 262144,
			SPDK_NVME_URMA_MEM_HOST, &external) == 0);
	assert(spdk_nvme_urma_region_registry_add(&context, buffer, 262144, external) == 0);
	req.payload.contig_or_cb_arg = buffer;
	req.payload.size = 262144;
	assert(nvme_urma_qpair_submit_request(&qs[0].qpair, &req) == 0);
	assert(recv(fds[0][1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.address == (uint64_t)buffer && frame.capsule.data.flags == 0);
	complete_request(&qs[0], &req, false);
	spdk_nvme_urma_unregister_memory(external);
	assert(shutdown(fds[1][0], SHUT_WR) == 0);
	assert(nvme_urma_qpair_submit_request(&qs[1].qpair, &req) != 0);
	assert(TAILQ_EMPTY(&qs[1].outstanding));
	unsigned before_fini = unregistrations;
#ifdef SPDK_URMA_HOST_STAGING_DEFAULT_SLOTS
	assert(spdk_urma_host_staging_fini(&device));
#endif
	for (unsigned i = 0; i < 2; i++) {
		cleanup(&qs[i]);
		close(fds[i][0]);
		close(fds[i][1]);
	}
	assert(unregistrations == before_fini + 1 && duplicate_rejections == 0);
	reject_duplicate = false;
	free(buffers);
	free(buffer);
	assert(setenv("SPDK_URMA_HOST_STAGING_SLOTS", "0", 1) == 0);
	puts("PASS bond staging: 32 children/one grant, write/read copies, "
	     "pool-full/large fallback, error/abort/send cleanup, "
	     "busy drain and explicit pool priority");
}

static void
bond_staging_init_failure_test(bool export_error)
{
	urma_context_t context = {};
	struct spdk_urma_device device = {.context = &context};
	struct spdk_nvme_ctrlr ctrlr = {};
	int fds[2];
	struct spdk_urma_capsule_cmd_frame frame;
	uint8_t *buffer = aligned_alloc(4096, 4096);
	unsigned before_reg = registrations, before_unreg = unregistrations;

	assert(buffer);
	assert(setenv("SPDK_URMA_HOST_STAGING_SLOTS", "4", 1) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	strcpy(device.dev_name, "bonding_dev_40");
	struct nvme_urma_qpair q = {
		.device = &device, .fd = fds[0], .num_entries = 32,
		.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP,
	};
	q.qpair.ctrlr = &ctrlr;
	q.qpair.id = 2;
	q.cid_bitmap = calloc(4, 1);
	TAILQ_INIT(&q.outstanding);
	struct nvme_request req = {
		.qpair_reserved = true, .cb_fn = request_done,
		.payload_type = NVME_PAYLOAD_TYPE_CONTIG,
	};
	req.cmd.opc = SPDK_NVME_OPC_READ;
	req.payload.contig_or_cb_arg = buffer;
	req.payload.size = 512;
	fail_export = export_error;
	fail_bounce_alloc = !export_error;
	int rc = nvme_urma_qpair_submit_request(&q.qpair, &req);
	fail_export = fail_bounce_alloc = false;
	if (export_error) {
		assert(rc == -EIO && TAILQ_EMPTY(&q.outstanding));
		assert(registrations == before_reg + 2 && unregistrations == before_unreg + 1);
		/* The unusable registered pool stays owned until checked teardown. */
		assert(nvme_urma_qpair_submit_request(&q.qpair, &req) == 0);
	} else {
		assert(rc == 0);
	}
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	assert(frame.capsule.data.address == (uint64_t)buffer);
	assert(frame.capsule.data.flags & SPDK_URMA_DATA_TRANSIENT);
	complete_request(&q, &req, false);
	assert(spdk_urma_host_staging_fini(&device));
	cleanup(&q);
	assert(registrations - before_reg == unregistrations - before_unreg);
	free(buffer);
	close(fds[0]);
	close(fds[1]);
	assert(setenv("SPDK_URMA_HOST_STAGING_SLOTS", "0", 1) == 0);
}

int
main(void)
{
	bond_staging_reuse_test();
	bond_staging_init_failure_test(false);
	bond_staging_init_failure_test(true);
	bond_data_lifetime_test();
	for (unsigned failure = 0; failure < 7; failure++) {
		bond_overlap_test(SPDK_NVME_OPC_READ, failure);
		bond_overlap_test(SPDK_NVME_OPC_WRITE, failure);
	}
	puts("PASS bond overlapping data: write staging, read copyback, "
	     "error/abort/export/send/allocation/provider cleanup");
	bond_io_connect_lifetime_test(false);
	bond_io_connect_lifetime_test(true);
	bond_admin_lifecycle_test();
	identify_test(true);
	identify_test(false);
	assert(registrations == unregistrations);
	puts("PASS production Identify submit: -ENOENT diagnostic, no partial send/CID leak, "
	     "retry, cache reuse, bond context and raw TCP");
	return 0;
}
