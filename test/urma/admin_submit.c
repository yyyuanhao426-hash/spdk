/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise production Identify preparation before a capsule reaches the target. */
#include "nvme_internal.h"
#include "nvme_urma_internal.h"
#include "spdk_internal/urma_lifetime.h"

/* REAL_TYPES */
static char diagnostics[16384];
static int registration_errno;
static bool fail_registration, fail_export;
static unsigned registrations, unregistrations;

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
	urma_target_seg_t *s = calloc(1, sizeof(*s));
	assert(s);
	s->seg.ubva.va = cfg->va;
	s->seg.len = cfg->len;
	s->seg.token_id = ++registrations;
	return s;
}
urma_status_t
urma_unregister_seg(urma_target_seg_t *seg)
{
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
	while (!TAILQ_EMPTY(&q->outstanding)) {
		struct nvme_urma_req *r = TAILQ_FIRST(&q->outstanding);
		TAILQ_REMOVE(&q->outstanding, r, link);
		free(r);
	}
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
	q.cid_bitmap = calloc(4, 1);
	TAILQ_INIT(&q.outstanding);
	void *buffer = aligned_alloc(4096, 4096);
	assert(buffer);
	struct nvme_request request = {0};
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
	assert(registrations == before + 1 && q.qpair.queue_depth == 1);
	assert(nvme_urma_qpair_submit_request(&q.qpair, &request) == 0);
	assert(registrations == before + 1 && q.qpair.queue_depth == 2);
	assert(recv(fds[1], &frame, sizeof(frame), MSG_DONTWAIT) > 0);
	if (bond) {
		void *other = aligned_alloc(4096, 4096);
		assert(other);
		request.payload.contig_or_cb_arg = other;
		fail_export = true;
		diagnostics[0] = 0;
		assert(nvme_urma_qpair_submit_request(&q.qpair, &request) == -EIO);
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
int
main(void)
{
	identify_test(true);
	identify_test(false);
	assert(registrations == unregistrations);
	puts("PASS production Identify submit: -ENOENT diagnostic, no partial send/CID leak, "
	     "retry, cache reuse, bond context and raw TCP");
	return 0;
}
