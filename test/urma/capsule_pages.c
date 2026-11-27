/* SPDX-License-Identifier: BSD-3-Clause */
#include "nvme_internal.h"
#include "nvme_urma_internal.h"
#include "nvmf_internal.h"
#include "spdk_internal/urma_lifetime.h"

#define NVMF_URMA_REG_CACHE_SIZE 128
/* REAL_TYPES */

/* Deterministic ordinary heap: separate live malloc objects can share a 4K
 * boundary page. Metadata and provider allocations do not use this arena. */
static unsigned char arena[8 * 1024 * 1024] __attribute__((aligned(4096)));
static size_t arena_used;
static pthread_mutex_t provider_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool fail_allocation, fail_registration;
static _Thread_local unsigned int fail_post_countdown;
static _Thread_local long host_page_override;
static void capsule_allocation_release(void *p);

static void *
capsule_test_calloc(size_t count, size_t size)
{
	size_t bytes = count * size;
	void *p;

	assert(size == 0 || bytes / size == count);
	if (size == sizeof(struct spdk_urma_capsule_rsp)) {
		return calloc(count, size); /* unregistered response queue metadata */
	}
	pthread_mutex_lock(&provider_lock);
	if (!arena_used) {
		arena_used = 64;
	}
	assert(bytes < sizeof(arena) - arena_used - 32);
	p = arena + arena_used;
	arena_used += (bytes + 15) / 16 * 16 + 16;
	memset(p, 0, bytes);
	pthread_mutex_unlock(&provider_lock);
	return p;
}

static void
capsule_test_free(void *p)
{
	if ((uintptr_t)p >= (uintptr_t)arena &&
	    (uintptr_t)p < (uintptr_t)arena + sizeof(arena)) {
		return;
	}
	capsule_allocation_release(p);
	free(p);
}

/* Only production pool allocations use the controlled heap. */
#define calloc capsule_test_calloc
#define free capsule_test_free
/* REAL_FUNCTIONS */
#undef calloc
#undef free

static urma_target_seg_t *live[512];
static unsigned int overlaps, grants, ungrants, posts;
static struct {
	void *addr;
	size_t bytes;
} allocations[512];

int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
long __real_sysconf(int name);

long
__wrap_sysconf(int name)
{
	return name == _SC_PAGESIZE && host_page_override ? host_page_override : __real_sysconf(name);
}

int
__wrap_posix_memalign(void **out, size_t alignment, size_t bytes)
{
	unsigned int i;
	int rc;

	if (fail_allocation) {
		fail_allocation = false;
		return ENOMEM;
	}
	rc = __real_posix_memalign(out, alignment, bytes);
	if (rc) {
		return rc;
	}
	pthread_mutex_lock(&provider_lock);
	for (i = 0; i < SPDK_COUNTOF(allocations); i++) {
		if (!allocations[i].addr) {
			allocations[i].addr = *out;
			allocations[i].bytes = bytes;
			break;
		}
	}
	assert(i < SPDK_COUNTOF(allocations));
	pthread_mutex_unlock(&provider_lock);
	return rc;
}

static void
capsule_allocation_release(void *p)
{
	unsigned int i, j;

	pthread_mutex_lock(&provider_lock);
	for (i = 0; i < SPDK_COUNTOF(allocations); i++) {
		if (allocations[i].addr == p) {
			for (j = 0; j < SPDK_COUNTOF(live); j++) {
				assert(!live[j] || live[j]->seg.ubva.va != (uintptr_t)p);
			}
			allocations[i].addr = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&provider_lock);
}

void
spdk_log(enum spdk_log_level level, const char *file, int line, const char *func,
	 const char *format, ...)
{
	va_list args;

	if (level <= SPDK_LOG_ERROR) {
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
	}
}

urma_target_seg_t *
urma_register_seg(urma_context_t *context, urma_seg_cfg_t *cfg)
{
	unsigned int i;
	urma_target_seg_t *seg;
	bool owned = false;

	if (fail_registration) {
		fail_registration = false;
		errno = EIO;
		return NULL;
	}
	pthread_mutex_lock(&provider_lock);
	for (i = 0; i < SPDK_COUNTOF(allocations); i++) {
		if (allocations[i].addr == (void *)(uintptr_t)cfg->va) {
			/* The provider's expanded grant must fit INSIDE the allocated
			 * object, including padding. Alignment alone is insufficient. */
			assert(cfg->len <= allocations[i].bytes);
			owned = true;
			break;
		}
	}

	assert(owned || (cfg->va >= (uintptr_t)arena &&
			cfg->va < (uintptr_t)arena + sizeof(arena)));
	/* Model the provider's page-interval rule using the actual rounded cfg
	 * produced by spdk_nvme_urma_register_memory, not a test-side rounding. */
	for (i = 0; i < SPDK_COUNTOF(live); i++) {
		if (live[i] && cfg->va < live[i]->seg.ubva.va + live[i]->seg.len &&
		    live[i]->seg.ubva.va < cfg->va + cfg->len) {
			overlaps++;
			fprintf(stderr, "[UMMU] Address range overlap: new=%#" PRIx64 "+%" PRIu64
				" live=%#" PRIx64 "+%" PRIu64 "\n", cfg->va, cfg->len,
				live[i]->seg.ubva.va, live[i]->seg.len);
			errno = EINVAL;
			pthread_mutex_unlock(&provider_lock);
			return NULL;
		}
	}
	seg = calloc(1, sizeof(*seg));
	assert(seg);
	seg->urma_ctx = context;
	seg->seg.ubva.va = cfg->va;
	seg->seg.len = cfg->len;
	for (i = 0; i < SPDK_COUNTOF(live); i++) {
		if (!live[i]) {
			live[i] = seg;
			grants++;
			pthread_mutex_unlock(&provider_lock);
			return seg;
		}
	}
	assert(false);
	return NULL;
}

urma_status_t
urma_unregister_seg(urma_target_seg_t *seg)
{
	unsigned int i;

	pthread_mutex_lock(&provider_lock);
	for (i = 0; i < SPDK_COUNTOF(live); i++) {
		if (live[i] == seg) {
			live[i] = NULL;
			ungrants++;
			free(seg);
			pthread_mutex_unlock(&provider_lock);
			return URMA_SUCCESS;
		}
	}
	assert(false);
	return URMA_FAIL;
}

urma_status_t
urma_post_jetty_recv_wr(urma_jetty_t *jetty, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad)
{
	assert(wr->src.num_sge == 1 && wr->src.sge->tseg);
	if (fail_post_countdown && --fail_post_countdown == 0) {
		return URMA_FAIL;
	}
	pthread_mutex_lock(&provider_lock);
	posts++;
	pthread_mutex_unlock(&provider_lock);
	return URMA_SUCCESS;
}

static void
target_init(struct nvmf_urma_qpair *q, struct spdk_urma_device *device,
	    urma_jfr_t **jfrs, urma_jetty_t **jettys, unsigned int count)
{
	memset(q, 0, sizeof(*q));
	q->device = device;
	q->jetty_count = 1;
	q->jettys = jettys;
	q->jfrs = jfrs;
	q->resource_count = count;
	q->qpair.sq_head_max = count - 1;
	q->capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_URMA;
	spdk_urma_lifetime_init(&q->lifetime);
}

static void
target_fini(struct nvmf_urma_qpair *q)
{
	unsigned int i;

	for (i = 0; i < q->capsule_rx_count; i++) {
		spdk_urma_rx_ref_retire(&q->capsule_rx_slots[i].rx_ref, &q->lifetime);
	}
	assert(!spdk_urma_lifetime_refs(&q->lifetime));
	nvmf_urma_capsule_resources_fini(q);
}

static int
target_test(bool tx, unsigned int count)
{
	urma_context_t ctx = {};
	struct spdk_urma_device device = {.context = &ctx};
	urma_jfr_t jfr = {.jfr_cfg.depth = 128};
	urma_jfr_t *jfrs[] = {&jfr};
	urma_jetty_t *jettys[] = {NULL};
	struct nvmf_urma_qpair q[48] = {};
	unsigned int i;
	int rc = 0;

	strcpy(device.dev_name, "bonding_dev_test");
	for (i = 0; i < SPDK_COUNTOF(q); i++) {
		target_init(&q[i], &device, jfrs, jettys, count);
		rc = tx ? nvmf_urma_rsp_tx_pool_init(&q[i]) : nvmf_urma_capsule_rx_post(&q[i], count);
		if (rc) {
			fprintf(stderr, "FAIL target-%s qpair=%u rc=%d overlaps=%u\n",
				tx ? "tx" : "rx", i, rc, overlaps);
			break;
		}
	}
	for (i = 0; i < SPDK_COUNTOF(q); i++) {
		target_fini(&q[i]);
	}
	if (!rc) {
		printf("PASS target-%s: 48 live qpair pools, slots=%u, no page-grant overlap\n",
		       tx ? "tx" : "rx", count);
	}
	return rc;
}

static void
initiator_init(struct nvme_urma_qpair *q, struct spdk_urma_device *device,
	       urma_jfr_t **jfrs, urma_jetty_t **jettys, unsigned int count)
{
	memset(q, 0, sizeof(*q));
	q->device = device;
	q->jetty_count = 1;
	q->jettys = jettys;
	q->jfrs = jfrs;
	q->num_entries = count;
	q->capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_URMA;
	spdk_urma_lifetime_init(&q->lifetime);
}

static void
initiator_disconnect(struct nvme_urma_qpair *q)
{
	unsigned int i;

	for (i = 0; q->capsule_rx_slots && i < q->capsule_rx_count; i++) {
		spdk_urma_rx_ref_retire(&q->capsule_rx_slots[i].rx_ref, &q->lifetime);
	}
	assert(!spdk_urma_lifetime_refs(&q->lifetime));
	nvme_urma_capsule_resources_fini(q);
}

static void
initiator_fini(struct nvme_urma_qpair *q)
{
	initiator_disconnect(q);
	/* Production delete_io_qpair releases these only after quiescing CRs. */
	capsule_test_free(q->capsule_rx_slots);
	capsule_test_free(q->capsule_tx_slots);
	q->capsule_rx_slots = NULL;
	q->capsule_tx_slots = NULL;
}

static int
initiator_test(bool tx, unsigned int count)
{
	urma_context_t ctx = {};
	struct spdk_urma_device device = {.context = &ctx};
	urma_jfr_t jfr = {.jfr_cfg.depth = 128};
	urma_jfr_t *jfrs[] = {&jfr};
	urma_jetty_t *jettys[] = {NULL};
	struct nvme_urma_qpair q[48] = {};
	unsigned int i;
	int rc = 0;

	strcpy(device.dev_name, "bonding_dev_test");
	for (i = 0; i < SPDK_COUNTOF(q); i++) {
		initiator_init(&q[i], &device, jfrs, jettys, count);
		rc = tx ? nvme_urma_tx_pool_init(&q[i]) : nvme_urma_capsule_resources_init(&q[i]);
		if (rc) {
			fprintf(stderr, "FAIL initiator-%s qpair=%u rc=%d overlaps=%u\n",
				tx ? "tx" : "rx", i, rc, overlaps);
			break;
		}
	}
	for (i = 0; i < SPDK_COUNTOF(q); i++) {
		initiator_fini(&q[i]);
	}
	if (!rc) {
		printf("PASS initiator-%s: 48 live qpair pools, slots=%u, no page-grant overlap\n",
		       tx ? "tx" : "rx", count);
	}
	return rc;
}

static void
allocator_test(void)
{
	unsigned int i;
	void *p;

	assert(!spdk_urma_alloc_capsule_slots(0, 16));
	assert(!spdk_urma_alloc_capsule_slots(16, 0));
	assert(!spdk_urma_alloc_capsule_slots(SIZE_MAX, 2));
	assert(!spdk_urma_alloc_capsule_slots(1, SIZE_MAX - 15));
	for (host_page_override = 4096; host_page_override <= 65536; host_page_override *= 16) {
		p = spdk_urma_alloc_capsule_slots(1, 17);
		assert(p && (uintptr_t)p % host_page_override == 0);
		for (i = 0; i < (unsigned int)host_page_override; i++) {
			assert(((unsigned char *)p)[i] == 0);
		}
		capsule_test_free(p);
	}
	host_page_override = 0;
	fail_allocation = true;
	assert(!spdk_urma_alloc_capsule_slots(2, 512) && errno == ENOMEM);
	puts("PASS page allocator: complete zeroed 4K/64K pages, overflow and allocation failure");
}

static void
reconnect_and_failure_test(void)
{
	urma_context_t ctx = {};
	struct spdk_urma_device device = {.context = &ctx};
	urma_jfr_t jfr = {.jfr_cfg.depth = 128};
	urma_jfr_t *jfrs[] = {&jfr};
	urma_jetty_t *jettys[] = {NULL};
	struct nvmf_urma_qpair target;
	struct nvme_urma_qpair initiator;
	void *rx, *tx;
	unsigned int before_posts, before_grants;
	unsigned int failure, i;

	strcpy(device.dev_name, "bonding_dev_test");
	/* The real target posts RX at HELLO, then allocates its TX pool at
	 * poll_group_add. RX and TX must own distinct pages within one qpair. */
	target_init(&target, &device, jfrs, jettys, 2);
	assert(!nvmf_urma_capsule_rx_post(&target, 2));
	before_posts = posts;
	before_grants = grants;
	assert(!nvmf_urma_capsule_rx_post(&target, 2));
	assert(posts == before_posts && grants == before_grants);
	assert(!nvmf_urma_capsule_resources_init(&target));
	assert(posts == before_posts && grants == before_grants + 1);
	target_fini(&target);

	initiator_init(&initiator, &device, jfrs, jettys, 2);
	assert(!nvme_urma_capsule_resources_init(&initiator));
	assert(!nvme_urma_tx_pool_init(&initiator));
	rx = initiator.capsule_rx_slots;
	tx = initiator.capsule_tx_slots;
	for (i = 0; i < 10; i++) {
		before_grants = grants;
		assert(!nvme_urma_tx_pool_init(&initiator));
		assert(grants == before_grants);
		initiator_disconnect(&initiator);
		assert(initiator.capsule_rx_slots == rx && initiator.capsule_tx_slots == tx);
		assert(!nvme_urma_capsule_resources_init(&initiator));
		assert(!nvme_urma_tx_pool_init(&initiator));
		assert(initiator.capsule_rx_slots == rx && initiator.capsule_tx_slots == tx);
	}
	initiator_fini(&initiator);

	for (failure = 0; failure < 3; failure++) {
		target_init(&target, &device, jfrs, jettys, 2);
		fail_allocation = failure == 0;
		fail_registration = failure == 1;
		fail_post_countdown = failure == 2 ? 2 : 0;
		assert(nvmf_urma_capsule_rx_post(&target, 2) < 0);
		target_fini(&target);

		initiator_init(&initiator, &device, jfrs, jettys, 2);
		fail_allocation = failure == 0;
		fail_registration = failure == 1;
		fail_post_countdown = failure == 2 ? 2 : 0;
		assert(nvme_urma_capsule_resources_init(&initiator) < 0);
		initiator_fini(&initiator);
	}
	for (failure = 0; failure < 2; failure++) {
		target_init(&target, &device, jfrs, jettys, 2);
		fail_allocation = failure == 0;
		fail_registration = failure == 1;
		assert(nvmf_urma_rsp_tx_pool_init(&target) < 0);
		target_fini(&target);
		initiator_init(&initiator, &device, jfrs, jettys, 2);
		fail_allocation = failure == 0;
		fail_registration = failure == 1;
		assert(nvme_urma_tx_pool_init(&initiator) < 0);
		initiator_fini(&initiator);
	}

	/* Raw and TCP mode selection remains unchanged. */
	target_init(&target, &device, jfrs, jettys, 2);
	strcpy(device.dev_name, "udmac0d1e3");
	assert(!nvmf_urma_capsule_resources_init(&target));
	assert(!target.capsule_tx_slots && target.capsule_rx_slots);
	target_fini(&target);
	target_init(&target, &device, jfrs, jettys, 2);
	target.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
	assert(!nvmf_urma_capsule_resources_init(&target));
	assert(!target.capsule_rx_slots && !target.capsule_tx_slots);
	target_fini(&target);
	strcpy(device.dev_name, "bonding_dev_test");
	target_init(&target, &device, jfrs, jettys, 2);
	target.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
	assert(!nvmf_urma_capsule_resources_init(&target));
	assert(!target.capsule_rx_slots && target.capsule_tx_slots);
	target_fini(&target);
	puts("PASS capsule lifecycle: HELLO idempotence, RX/TX separation, ten reconnects, "
	     "allocation/registration/partial-post failure, raw and TCP selection");
}

struct stress_context {
	struct spdk_urma_device device;
	urma_jfr_t jfr;
	unsigned int iterations;
};

static void *
stress_worker(void *arg)
{
	struct stress_context *ctx = arg;
	urma_jfr_t *jfrs[] = {&ctx->jfr};
	urma_jetty_t *jettys[] = {NULL};
	struct nvmf_urma_qpair target;
	struct nvme_urma_qpair initiator;
	unsigned int i;

	for (i = 0; i < ctx->iterations; i++) {
		target_init(&target, &ctx->device, jfrs, jettys, 2);
		initiator_init(&initiator, &ctx->device, jfrs, jettys, 2);
		assert(!nvmf_urma_capsule_rx_post(&target, 2));
		assert(!nvmf_urma_capsule_resources_init(&target));
		assert(!nvme_urma_capsule_resources_init(&initiator));
		assert(!nvme_urma_tx_pool_init(&initiator));
		target_fini(&target);
		initiator_fini(&initiator);
	}
	return NULL;
}

static void
concurrent_test(void)
{
	urma_context_t context = {};
	struct stress_context ctx = {
		.device.context = &context, .jfr.jfr_cfg.depth = 128, .iterations = 20,
	};
	pthread_t threads[48];
	unsigned int i;

	strcpy(ctx.device.dev_name, "bonding_dev_test");
	for (i = 0; i < SPDK_COUNTOF(threads); i++) {
		assert(!pthread_create(&threads[i], NULL, stress_worker, &ctx));
	}
	for (i = 0; i < SPDK_COUNTOF(threads); i++) {
		assert(!pthread_join(threads[i], NULL));
	}
	assert(!overlaps && grants == ungrants);
	puts("PASS 48 concurrent threads: 960 paired qpair lifecycles, four disjoint pools per pair");
}

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "all";
	unsigned int counts[] = {1, 2, 32, 128};
	unsigned int i;
	bool all = !strcmp(name, "all");
	int rc = 0;

	for (i = 0; i < SPDK_COUNTOF(counts) && !rc; i++) {
		if (all || !strcmp(name, "target-rx")) {
			rc = target_test(false, counts[i]);
		}
		if (!rc && (all || !strcmp(name, "target-tx"))) {
			rc = target_test(true, counts[i]);
		}
		if (!rc && (all || !strcmp(name, "initiator-rx"))) {
			rc = initiator_test(false, counts[i]);
		}
		if (!rc && (all || !strcmp(name, "initiator-tx"))) {
			rc = initiator_test(true, counts[i]);
		}
	}
	if (!rc && all) {
		allocator_test();
		reconnect_and_failure_test();
		concurrent_test();
	}

	assert(grants == ungrants);
	for (i = 0; i < SPDK_COUNTOF(allocations); i++) {
		assert(!allocations[i].addr);
	}
	return rc ? 1 : 0;
}
