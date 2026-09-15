/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

/* NDS: NPU Direct Storage support for urma_perf.
 *
 * Mirrors the CUDA half of urma_perf.c (struct cuda_driver + dlopen libcuda):
 * the AscendCL runtime is dlopen'ed so the binary runs unchanged on machines
 * without CANN. All AscendCL signatures come from the public CANN
 * documentation (aclrt* family); they are redeclared locally so no CANN
 * headers are needed at compile time, exactly like the CUDA path does.
 *
 * Known Phase 1 limitation: AscendCL exposes no public dmabuf export API
 * (no equivalent of cuMemGetHandleForAddressRange), so the NPU provider
 * leaves export_dmabuf NULL and the only direct-registration route is the
 * URMA peer-memory path (is_gpu_seg=1), which requires the Phase 2 kernel
 * NPU bridge module. */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/nvme_urma.h"

#include "urma_perf_npu.h"

#include <dlfcn.h>

/* HBM allocation alignment. CUDA uses 64KB; the NPU page granularity for
 * URMA peer-memory pinning is one of the Phase 2 verification items, keep
 * the value overridable until it is measured on the company machine. */
#ifndef URMA_PERF_NPU_ALIGNMENT
#define URMA_PERF_NPU_ALIGNMENT (64 * 1024)
#endif

/* AscendCL return codes: 0 on success (aclError). */
#define NPU_ACL_SUCCESS 0

/* aclrtMemMallocPolicy: prefer huge pages, fall back to normal. */
#define NPU_ACL_MEM_MALLOC_HUGE_FIRST 0

/* aclrtMemcpyKind. */
#define NPU_ACL_MEMCPY_HOST_TO_DEVICE 1
#define NPU_ACL_MEMCPY_DEVICE_TO_HOST 2

typedef int32_t acl_error_t;
typedef void *aclrt_context_t;

typedef acl_error_t (*acl_init_fn)(const char *config_path);
typedef acl_error_t (*acl_finalize_fn)(void);
typedef acl_error_t (*aclrt_set_device_fn)(int32_t device_id);
typedef acl_error_t (*aclrt_reset_device_fn)(int32_t device_id);
typedef acl_error_t (*aclrt_create_context_fn)(aclrt_context_t *context);
typedef acl_error_t (*aclrt_destroy_context_fn)(aclrt_context_t context);
typedef acl_error_t (*aclrt_set_current_context_fn)(aclrt_context_t context);
typedef acl_error_t (*aclrt_malloc_fn)(void **dev_ptr, size_t size, int policy);
typedef acl_error_t (*aclrt_free_fn)(void *dev_ptr);
typedef acl_error_t (*aclrt_memcpy_fn)(void *dst, size_t dest_max, const void *src,
					size_t count, int kind);

struct npu_driver {
	void *library;
	aclrt_context_t context;
	int32_t device_id;
	bool device_set;
	acl_init_fn init;
	acl_finalize_fn finalize;
	aclrt_set_device_fn set_device;
	aclrt_reset_device_fn reset_device;
	aclrt_create_context_fn create_context;
	aclrt_destroy_context_fn destroy_context;
	aclrt_set_current_context_fn set_current_context;
	aclrt_malloc_fn malloc;
	aclrt_free_fn free;
	aclrt_memcpy_fn memcpy;
};

static struct npu_driver g_npu;

/* Allocation registry: provider pin() needs to answer "which HBM allocation
 * contains [addr, addr+len)". urma_perf uses at most 64 workers. */
#define NPU_ALLOC_REGISTRY_SIZE 64

struct npu_alloc_entry {
	void *addr;
	size_t size;
	bool used;
};

static struct npu_alloc_entry g_npu_allocs[NPU_ALLOC_REGISTRY_SIZE];

struct npu_pin_handle {
	struct npu_alloc_entry *entry;
	uint64_t offset;
};

static size_t
npu_align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static void *
npu_load_symbol(const char *name)
{
	void *symbol = dlsym(g_npu.library, name);

	if (symbol == NULL) {
		fprintf(stderr, "Missing AscendCL symbol %s\n", name);
	}
	return symbol;
}

#define NPU_LOAD_REQUIRED(member, name) \
	do { \
		g_npu.member = (__typeof__(g_npu.member))npu_load_symbol(name); \
		if (g_npu.member == NULL) { \
			goto fail; \
		} \
	} while (0)

/* NDS-fix: 非 static——urma_perf.c 经 urma_perf_npu.h 外部调用 */
int
npu_driver_init(int32_t device_id)
{
	const char *lib_name;
	acl_error_t err;

	memset(&g_npu, 0, sizeof(g_npu));
	g_npu.device_id = device_id;

	/* Honour an explicit override so non-default CANN installs are usable. */
	lib_name = getenv("URMA_PERF_NPU_LIB");
	if (lib_name == NULL || lib_name[0] == '\0') {
		lib_name = "libascendcl.so";
	}
	g_npu.library = dlopen(lib_name, RTLD_NOW | RTLD_LOCAL);
	if (g_npu.library == NULL) {
		fprintf(stderr, "Unable to load %s: %s\n", lib_name, dlerror());
		return -ENODEV;
	}
	/* Modified By NDS: 打印实际加载的 CANN 库路径——197 上多版本 CANN
	 * 并存（8.5.0/9.0.1/9.1.0/9.0.T500），507033 排查需确认进程真实加载的是
	 * 哪一份 libascendcl（独立程序正常而 urma_perf 失败，可能是版本混用） */
	{
		Dl_info info;

		if (dladdr(dlsym(g_npu.library, "aclInit"), &info) != 0 &&
		    info.dli_fname != NULL) {
			printf("CANN runtime loaded: %s\n", info.dli_fname);
		}
	}
	NPU_LOAD_REQUIRED(init, "aclInit");
	NPU_LOAD_REQUIRED(set_device, "aclrtSetDevice");
	NPU_LOAD_REQUIRED(create_context, "aclrtCreateContext");
	NPU_LOAD_REQUIRED(malloc, "aclrtMalloc");
	NPU_LOAD_REQUIRED(free, "aclrtFree");
	NPU_LOAD_REQUIRED(memcpy, "aclrtMemcpy");
	/* Optional in older CANN builds; thread binding falls back to
	 * aclrtSetDevice() when absent. */
	g_npu.set_current_context = (aclrt_set_current_context_fn)
				     npu_load_symbol("aclrtSetCurrentContext");
	g_npu.finalize = (acl_finalize_fn)dlsym(g_npu.library, "aclFinalize");
	g_npu.destroy_context = (aclrt_destroy_context_fn)dlsym(g_npu.library, "aclrtDestroyContext");
	g_npu.reset_device = (aclrt_reset_device_fn)dlsym(g_npu.library, "aclrtResetDevice");

	err = g_npu.init(NULL);
	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclInit failed: aclError %d\n", err);
		goto fail;
	}
	err = g_npu.set_device(device_id);
	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtSetDevice(%d) failed: aclError %d\n", device_id, err);
		goto fail;
	}
	g_npu.device_set = true;
	err = g_npu.create_context(&g_npu.context);
	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtCreateContext failed: aclError %d\n", err);
		goto fail;
	}
	return 0;

fail:
	if (g_npu.device_set) {
		if (g_npu.reset_device != NULL) {
			g_npu.reset_device(device_id);
		}
		g_npu.device_set = false;
	}
	if (g_npu.library != NULL) {
		dlclose(g_npu.library);
	}
	memset(&g_npu, 0, sizeof(g_npu));
	return -ENODEV;
}

void
npu_driver_fini(void)
{
	if (g_npu.library == NULL) {
		return;
	}
	if (g_npu.context != NULL && g_npu.destroy_context != NULL) {
		g_npu.destroy_context(g_npu.context);
	}
	if (g_npu.device_set && g_npu.reset_device != NULL) {
		g_npu.reset_device(g_npu.device_id);
	}
	if (g_npu.finalize != NULL) {
		g_npu.finalize();
	}
	dlclose(g_npu.library);
	memset(&g_npu, 0, sizeof(g_npu));
}

int
npu_worker_thread_bind(void)
{
	acl_error_t err;

	if (g_npu.library == NULL) {
		return -ENODEV;
	}
	if (g_npu.set_current_context != NULL) {
		err = g_npu.set_current_context(g_npu.context);
		if (err != NPU_ACL_SUCCESS) {
			fprintf(stderr, "aclrtSetCurrentContext failed: aclError %d\n", err);
			return -EIO;
		}
		return 0;
	}
	/* Fallback: rebinding the device also binds the thread context. */
	err = g_npu.set_device(g_npu.device_id);
	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtSetDevice(worker) failed: aclError %d\n", err);
		return -EIO;
	}
	return 0;
}

static struct npu_alloc_entry *
npu_registry_find(void *addr, size_t length)
{
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end;
	size_t i;

	if (__builtin_add_overflow(start, length, &end)) {
		return NULL;
	}
	for (i = 0; i < NPU_ALLOC_REGISTRY_SIZE; i++) {
		struct npu_alloc_entry *e = &g_npu_allocs[i];
		uintptr_t base = (uintptr_t)e->addr;
		uintptr_t limit = base + e->size;

		if (e->used && start >= base && end <= limit) {
			return e;
		}
	}
	return NULL;
}

int
npu_alloc_buffer(struct npu_allocation *allocation, size_t size)
{
	struct npu_alloc_entry *entry = NULL;
	acl_error_t err;
	void *ptr = NULL;
	size_t i;

	if (g_npu.library == NULL) {
		return -ENODEV;
	}
	memset(allocation, 0, sizeof(*allocation));
	allocation->dmabuf_fd = -1;
	allocation->used_size = size;
	allocation->alloc_size = npu_align_up(size, URMA_PERF_NPU_ALIGNMENT);

	err = g_npu.malloc(&ptr, allocation->alloc_size, NPU_ACL_MEM_MALLOC_HUGE_FIRST);
	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtMalloc(%zu) failed: aclError %d\n",
			allocation->alloc_size, err);
		return -ENOMEM;
	}
	for (i = 0; i < NPU_ALLOC_REGISTRY_SIZE; i++) {
		if (!g_npu_allocs[i].used) {
			entry = &g_npu_allocs[i];
			break;
		}
	}
	if (entry == NULL) {
		fprintf(stderr, "NPU allocation registry full (%d entries)\n",
			NPU_ALLOC_REGISTRY_SIZE);
		g_npu.free(ptr);
		return -ENOMEM;
	}
	entry->addr = ptr;
	entry->size = allocation->alloc_size;
	entry->used = true;
	allocation->addr = ptr;
	return 0;
}

void
npu_free_buffer(void *addr)
{
	size_t i;

	if (addr == NULL || g_npu.library == NULL) {
		return;
	}
	for (i = 0; i < NPU_ALLOC_REGISTRY_SIZE; i++) {
		if (g_npu_allocs[i].used && g_npu_allocs[i].addr == addr) {
			g_npu_allocs[i].used = false;
			g_npu_allocs[i].addr = NULL;
			break;
		}
	}
	g_npu.free(addr);
}

int
npu_memcpy_h2d(void *device_dst, const void *host_src, size_t size)
{
	acl_error_t err;

	if (g_npu.library == NULL) {
		return -ENODEV;
	}
	err = g_npu.memcpy(device_dst, size, host_src, size,
				   NPU_ACL_MEMCPY_HOST_TO_DEVICE);

	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtMemcpy(H2D) failed: aclError %d\n", err);
		return -EIO;
	}
	return 0;
}

int
npu_memcpy_d2h(void *host_dst, const void *device_src, size_t size)
{
	acl_error_t err;

	if (g_npu.library == NULL) {
		return -ENODEV;
	}
	err = g_npu.memcpy(host_dst, size, device_src, size,
				   NPU_ACL_MEMCPY_DEVICE_TO_HOST);

	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtMemcpy(D2H) failed: aclError %d\n", err);
		return -EIO;
	}
	return 0;
}

static int
npu_provider_pin(void *provider_ctx, void *addr, size_t length, void **pin_handle)
{
	struct npu_alloc_entry *entry;
	struct npu_pin_handle *handle;

	(void)provider_ctx;
	entry = npu_registry_find(addr, length);
	if (entry == NULL) {
		return -EFAULT;
	}
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		return -ENOMEM;
	}
	handle->entry = entry;
	handle->offset = (uintptr_t)addr - (uintptr_t)entry->addr;
	*pin_handle = handle;
	return 0;
}

static void
npu_provider_unpin(void *provider_ctx, void *pin_handle)
{
	(void)provider_ctx;
	free(pin_handle);
}

/* export_dmabuf is deliberately NULL: AscendCL has no public dmabuf export,
 * so nvme_urma_common.c skips the dmabuf branch and registers through
 * urma_register_seg() with is_gpu_seg=1 (peer-memory route). */
static const struct spdk_nvme_urma_memory_provider g_npu_provider = {
	.name = "npu-urma-perf",
	.type = SPDK_NVME_URMA_MEM_NPU,
	.pin = npu_provider_pin,
	.unpin = npu_provider_unpin,
};

int
npu_provider_register(void)
{
	return spdk_nvme_urma_register_memory_provider(&g_npu_provider);
}

void
npu_provider_unregister(void)
{
	spdk_nvme_urma_unregister_memory_provider(SPDK_NVME_URMA_MEM_NPU);
}
