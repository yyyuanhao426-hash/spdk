/* SPDX-License-Identifier: BSD-3-Clause */
/* Multiple endpoint qpairs register one application-owned pool. */
#include "nvme_internal.h"
#include "nvme_urma_internal.h"
#include "spdk_internal/urma_lifetime.h"
#define URMA_STAGE_SIDE 1
#include "spdk_internal/urma_stage_buffer.h"

/* REAL_TYPES */

#define POOL_SIZE ((size_t)1 << 30)
#define POOL_ADDR ((void *)(uintptr_t)0xfffd00000000)
#define ENDPOINTS 8

static pthread_mutex_t provider_mutex = PTHREAD_MUTEX_INITIALIZER;
static urma_target_seg_t *active[64];
static unsigned registrations, unregistrations, duplicate_grants, pin_calls, unpin_calls;
static bool fail_registration;
static _Thread_local unsigned fail_calloc_on;

void *__real_calloc(size_t count, size_t size);
void *
__wrap_calloc(size_t count, size_t size)
{
	if (fail_calloc_on != 0 && --fail_calloc_on == 0) {
		errno = ENOMEM;
		return NULL;
	}
	return __real_calloc(count, size);
}

void
spdk_log(enum spdk_log_level level, const char *file, int line, const char *func,
	 const char *format, ...)
{
}

/* Model the captured UMMU rejection of a live duplicate context/grant range.
 * No backing 1GiB allocation or URMA hardware is required. */
urma_target_seg_t *
urma_register_seg(urma_context_t *context, urma_seg_cfg_t *cfg)
{
	pthread_mutex_lock(&provider_mutex);
	if (fail_registration) {
		pthread_mutex_unlock(&provider_mutex);
		errno = ENOENT;
		return NULL;
	}
	for (unsigned i = 0; i < SPDK_COUNTOF(active); i++) {
		if (active[i] && active[i]->urma_ctx == context &&
		    active[i]->seg.ubva.va == cfg->va && active[i]->seg.len == cfg->len) {
			duplicate_grants++;
			pthread_mutex_unlock(&provider_mutex);
			errno = EINVAL;
			return NULL;
		}
	}
	urma_target_seg_t *segment = malloc(sizeof(*segment));
	assert(segment);
	memset(segment, 0, sizeof(*segment));
	segment->urma_ctx = context;
	segment->seg.ubva.va = cfg->va;
	segment->seg.len = cfg->len;
	segment->seg.token_id = ++registrations;
	for (unsigned i = 0; i < SPDK_COUNTOF(active); i++) {
		if (active[i] == NULL) {
			active[i] = segment;
			pthread_mutex_unlock(&provider_mutex);
			return segment;
		}
	}
	abort();
}

urma_status_t
urma_unregister_seg(urma_target_seg_t *segment)
{
	pthread_mutex_lock(&provider_mutex);
	for (unsigned i = 0; i < SPDK_COUNTOF(active); i++) {
		if (active[i] == segment) {
			active[i] = NULL;
			unregistrations++;
			free(segment);
			pthread_mutex_unlock(&provider_mutex);
			return URMA_SUCCESS;
		}
	}
	abort();
}

urma_status_t
urma_get_seg_ctx(urma_target_seg_t *segment, urma_seg_t **out, uint32_t *size)
{
	*size = sizeof(**out) + 64;
	*out = malloc(*size);
	assert(*out);
	memset(*out, 0xa5, *size);
	memcpy(*out, &segment->seg, sizeof(**out));
	return URMA_SUCCESS;
}

void
urma_put_seg_ctx(urma_seg_t *context)
{
	free(context);
}

static int
pin(void *context, void *addr, size_t length, void **handle)
{
	pin_calls++;
	*handle = addr;
	return 0;
}

static void
unpin(void *context, void *handle)
{
	unpin_calls++;
}

/* REAL_FUNCTIONS */

struct pool_fixture {
	urma_context_t context;
	struct spdk_urma_device device;
	struct nvme_urma_qpair qpairs[ENDPOINTS];
};

static void
fixture_init(struct pool_fixture *fixture)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->device.context = &fixture->context;
	strcpy(fixture->device.dev_name, "bonding_dev_70");
	for (unsigned i = 0; i < ENDPOINTS; i++) {
		fixture->qpairs[i].device = &fixture->device;
		fixture->qpairs[i].qpair.id = 1;
	}
}

static int
register_pool(struct nvme_urma_qpair *qpair, void *addr, size_t size,
	      struct spdk_nvme_urma_memory_region **region)
{
	return spdk_nvme_urma_register_memory_for_qpair(&qpair->qpair, addr, size,
			SPDK_NVME_URMA_MEM_HOST, region);
}

static void
check_sequential(void)
{
	struct pool_fixture fixture;
	struct spdk_nvme_urma_memory_region *regions[ENDPOINTS];
	unsigned before_reg = registrations, before_unreg = unregistrations;
	fixture_init(&fixture);
	for (unsigned i = 0; i < ENDPOINTS; i++) {
		int rc = register_pool(&fixture.qpairs[i], POOL_ADDR, POOL_SIZE, &regions[i]);
		if (rc != 0) {
			fprintf(stderr, "RED: endpoint %u re-registers live 1GiB pool "
				"in shared context: rc=%d\n",
				i + 1, rc);
			exit(1);
		}
		assert(regions[i] == regions[0]);
	}
	assert(registrations == before_reg + 1);
	size_t length = 0;
	assert(spdk_nvme_urma_memory_region_export(regions[7], NULL, &length) == 0);
	assert(length >= sizeof(urma_seg_t));
	uint8_t *blob = malloc(length);
	assert(blob && spdk_nvme_urma_memory_region_export(regions[0], blob, &length) == 0);
	free(blob);
	/* Drop the first endpoint while later endpoint owners still need the pool. */
	for (unsigned i = 0; i < ENDPOINTS - 1; i++) {
		spdk_nvme_urma_unregister_memory(regions[i]);
		assert(unregistrations == before_unreg);
		assert(spdk_nvme_urma_region_registry_find(&fixture.context,
			(uint64_t)POOL_ADDR + 4096, 8388608) == regions[7]);
	}
	spdk_nvme_urma_unregister_memory(regions[7]);
	assert(unregistrations == before_unreg + 1);
	assert(spdk_nvme_urma_region_registry_find(&fixture.context,
		(uint64_t)POOL_ADDR, POOL_SIZE) == NULL);
	/* Reusing the same allocation after all owners release must grant afresh. */
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &regions[0]) == 0);
	assert(registrations == before_reg + 2);
	spdk_nvme_urma_unregister_memory(regions[0]);
	puts("PASS 8 endpoint owners: one 1GiB grant, live until last release, "
	     "fresh re-registration");
}

struct thread_args {
	struct nvme_urma_qpair *qpair;
	struct spdk_nvme_urma_memory_region *region;
	pthread_barrier_t *barrier;
	bool release;
	int rc;
};

static void *
thread_operation(void *arg)
{
	struct thread_args *args = arg;
	pthread_barrier_wait(args->barrier);
	if (args->release) {
		spdk_nvme_urma_unregister_memory(args->region);
	} else {
		args->rc = register_pool(args->qpair, POOL_ADDR, POOL_SIZE, &args->region);
	}
	return NULL;
}

static void
run_threads(struct thread_args *args, unsigned count)
{
	pthread_t threads[ENDPOINTS];
	pthread_barrier_t barrier;
	assert(pthread_barrier_init(&barrier, NULL, count) == 0);
	for (unsigned i = 0; i < count; i++) {
		args[i].barrier = &barrier;
		assert(pthread_create(&threads[i], NULL, thread_operation, &args[i]) == 0);
	}
	for (unsigned i = 0; i < count; i++) {
		assert(pthread_join(threads[i], NULL) == 0);
	}
	assert(pthread_barrier_destroy(&barrier) == 0);
}

static void
check_concurrent(void)
{
	struct pool_fixture fixture;
	struct thread_args args[ENDPOINTS] = {};
	unsigned before_reg = registrations, before_unreg = unregistrations;
	fixture_init(&fixture);
	for (unsigned i = 0; i < ENDPOINTS; i++) {
		args[i].qpair = &fixture.qpairs[i];
	}
	run_threads(args, ENDPOINTS);
	for (unsigned i = 0; i < ENDPOINTS; i++) {
		assert(args[i].rc == 0 && args[i].region == args[0].region);
		args[i].release = true;
	}
	assert(registrations == before_reg + 1);
	run_threads(args, ENDPOINTS);
	assert(unregistrations == before_unreg + 1);
	assert(!spdk_nvme_urma_region_registry_find(&fixture.context,
		(uint64_t)POOL_ADDR, POOL_SIZE));
	/* Concurrent last release and reacquire must not grant before ungrant. */
	for (unsigned i = 0; i < 32; i++) {
		struct thread_args race[2] = {{.release = true}, {.qpair = &fixture.qpairs[1]}};
		assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE,
			&race[0].region) == 0);
		run_threads(race, 2);
		assert(race[1].rc == 0 && race[1].region != NULL);
		assert(spdk_nvme_urma_region_registry_find(&fixture.context,
			(uint64_t)POOL_ADDR, POOL_SIZE) == race[1].region);
		spdk_nvme_urma_unregister_memory(race[1].region);
	}
	assert(registrations == unregistrations && duplicate_grants == 0);
	puts("PASS concurrent acquire/release and last-release/reacquire races");
}

static void
check_isolation(void)
{
	struct pool_fixture a, b;
	struct spdk_nvme_urma_memory_region *ra, *rb, *small, *other;
	fixture_init(&a);
	fixture_init(&b);
	unsigned before_reg = registrations;
	assert(register_pool(&a.qpairs[0], POOL_ADDR, POOL_SIZE, &ra) == 0);
	assert(register_pool(&b.qpairs[0], POOL_ADDR, POOL_SIZE, &rb) == 0);
	assert(ra != rb && registrations == before_reg + 2);
	/* A covered subrange is a different registration request, not a new owner
	 * of a region with different get_addr/get_length semantics. */
	assert(register_pool(&a.qpairs[1], POOL_ADDR, 4096, &small) == 0);
	assert(small != ra && spdk_nvme_urma_memory_region_get_length(small) == 4096);
	assert(register_pool(&a.qpairs[2], (void *)0x400000, 4096, &other) == 0);
	assert(other != ra);
	spdk_nvme_urma_unregister_memory(ra);
	assert(spdk_nvme_urma_region_registry_find(&b.context,
		(uint64_t)POOL_ADDR, POOL_SIZE) == rb);
	spdk_nvme_urma_unregister_memory(rb);
	spdk_nvme_urma_unregister_memory(small);
	spdk_nvme_urma_unregister_memory(other);
	/* Low-level temporary registrations remain independently owned. */
	assert(spdk_nvme_urma_register_memory(&a.context, (void *)0x400000, 1024,
		SPDK_NVME_URMA_MEM_HOST, &ra) == 0);
	spdk_nvme_urma_unregister_memory(ra);
	assert(spdk_nvme_urma_register_memory(&a.context, (void *)0x400000, 4096,
		SPDK_NVME_URMA_MEM_HOST, &ra) == 0);
	spdk_nvme_urma_unregister_memory(ra);
	puts("PASS context/address/length isolation and independent temporary registrations");
}

static void
check_failures(void)
{
	struct pool_fixture fixture;
	struct spdk_nvme_urma_memory_region *region = NULL;
	fixture_init(&fixture);
	fail_registration = true;
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &region) == -ENOENT);
	assert(region == NULL);
	fail_registration = false;
	fail_calloc_on = 1;
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &region) == -ENOMEM);
	assert(fail_calloc_on == 0 && region == NULL);
	/* First calloc creates the region, second adds its registry entry. */
	unsigned before_reg = registrations, before_unreg = unregistrations;
	fail_calloc_on = 2;
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &region) == -ENOMEM);
	assert(fail_calloc_on == 0 && region == NULL);
	assert(registrations == before_reg + 1 && unregistrations == before_unreg + 1);
	assert(!spdk_nvme_urma_region_registry_find(&fixture.context,
		(uint64_t)POOL_ADDR, POOL_SIZE));
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &region) == 0);
	spdk_nvme_urma_unregister_memory(region);
	assert(register_pool(&fixture.qpairs[0], NULL, POOL_SIZE, &region) == -EINVAL);
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, 0, &region) == -EINVAL);
	assert(register_pool(&fixture.qpairs[0], (void *)(UINTPTR_MAX - 1),
		4096, &region) == -EINVAL);
	assert(spdk_nvme_urma_register_memory_for_qpair(NULL, POOL_ADDR, POOL_SIZE,
		SPDK_NVME_URMA_MEM_HOST, &region) == -EINVAL && region == NULL);
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, NULL) == -EINVAL);
	assert(spdk_nvme_urma_register_memory_for_qpair(&fixture.qpairs[0].qpair,
		POOL_ADDR, POOL_SIZE, (enum spdk_nvme_urma_memory_type)-1, &region) == -EINVAL);
	fixture.device.context = NULL;
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &region) == -EINVAL);
	fixture.qpairs[0].device = NULL;
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &region) == -ENXIO);
	puts("PASS provider failure, registry allocation rollback, retry and invalid ranges");
}

static void
check_memory_type(void)
{
	struct pool_fixture fixture;
	struct spdk_nvme_urma_memory_region *host, *gpu, *gpu2;
	struct spdk_nvme_urma_memory_provider provider = {
		.name = "fixture", .type = SPDK_NVME_URMA_MEM_CUDA, .pin = pin, .unpin = unpin,
	};
	fixture_init(&fixture);
	assert(spdk_nvme_urma_register_memory_provider(&provider) == 0);
	assert(register_pool(&fixture.qpairs[0], POOL_ADDR, POOL_SIZE, &host) == 0);
	/* Same VA/size but wrong memory type must never adopt the HOST handle. */
	assert(spdk_nvme_urma_register_memory_for_qpair(&fixture.qpairs[1].qpair,
		POOL_ADDR, POOL_SIZE, SPDK_NVME_URMA_MEM_CUDA, &gpu) == -EINVAL);
	assert(gpu == NULL && pin_calls == 1 && unpin_calls == 1 && duplicate_grants == 1);
	spdk_nvme_urma_unregister_memory(host);
	assert(spdk_nvme_urma_register_memory_for_qpair(&fixture.qpairs[0].qpair,
		POOL_ADDR, POOL_SIZE, SPDK_NVME_URMA_MEM_CUDA, &gpu) == 0);
	assert(spdk_nvme_urma_register_memory_for_qpair(&fixture.qpairs[1].qpair,
		POOL_ADDR, POOL_SIZE, SPDK_NVME_URMA_MEM_CUDA, &gpu2) == 0);
	assert(gpu == gpu2 && pin_calls == 2);
	spdk_nvme_urma_unregister_memory(gpu);
	assert(unpin_calls == 1);
	assert(spdk_nvme_urma_unregister_memory_provider(provider.type) == -EBUSY);
	spdk_nvme_urma_unregister_memory(gpu2);
	assert(unpin_calls == 2 && spdk_nvme_urma_unregister_memory_provider(provider.type) == 0);
	puts("PASS memory-type isolation, shared accelerator pin and last-owner unpin");
}

int
main(void)
{
	check_sequential();
	check_concurrent();
	check_isolation();
	check_failures();
	check_memory_type();
	/* The only rejected grant was the intentional HOST/CUDA type-isolation control. */
	assert(registrations == unregistrations && duplicate_grants == 1);
	return 0;
}
