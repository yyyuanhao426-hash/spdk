/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#include "spdk/stdinc.h"

#include "spdk/dma.h"
#include "spdk/env.h"
#include "spdk/histogram_data.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_urma.h"
#include "spdk/string.h"

#include <dlfcn.h>

#define URMA_PERF_DEFAULT_IO_SIZE 4096
#define URMA_PERF_DEFAULT_BATCH_SIZE 16
#define URMA_PERF_DEFAULT_RUN_TIME 10
#define URMA_PERF_GPU_ALIGNMENT (64 * 1024)
#define URMA_PERF_CUDA_SUCCESS 0
#define URMA_PERF_CUDA_DMABUF_HANDLE 1
#define URMA_PERF_CUDA_SYNC_MEMOPS 6

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

typedef CUresult (*cuda_init_fn)(unsigned int flags);
typedef CUresult (*cuda_device_get_fn)(CUdevice *device, int ordinal);
typedef CUresult (*cuda_ctx_create_fn)(CUcontext *context, unsigned int flags, CUdevice device);
typedef CUresult (*cuda_ctx_destroy_fn)(CUcontext context);
typedef CUresult (*cuda_ctx_set_current_fn)(CUcontext context);
typedef CUresult (*cuda_mem_alloc_fn)(CUdeviceptr *ptr, size_t size);
typedef CUresult (*cuda_mem_free_fn)(CUdeviceptr ptr);
typedef CUresult (*cuda_memcpy_h2d_fn)(CUdeviceptr dst, const void *src, size_t size);
typedef CUresult (*cuda_memcpy_d2h_fn)(void *dst, CUdeviceptr src, size_t size);
typedef CUresult (*cuda_pointer_set_attribute_fn)(const void *value, int attribute,
		CUdeviceptr ptr);
typedef CUresult (*cuda_get_dmabuf_fn)(void *handle, CUdeviceptr ptr, size_t size,
		int handle_type, unsigned long long flags);
typedef CUresult (*cuda_get_error_string_fn)(CUresult error, const char **message);

struct cuda_driver {
	void *library;
	CUcontext context;
	cuda_init_fn init;
	cuda_device_get_fn device_get;
	cuda_ctx_create_fn ctx_create;
	cuda_ctx_destroy_fn ctx_destroy;
	cuda_ctx_set_current_fn ctx_set_current;
	cuda_mem_alloc_fn mem_alloc;
	cuda_mem_free_fn mem_free;
	cuda_memcpy_h2d_fn memcpy_h2d;
	cuda_memcpy_d2h_fn memcpy_d2h;
	cuda_pointer_set_attribute_fn pointer_set_attribute;
	cuda_get_dmabuf_fn get_dmabuf;
	cuda_get_error_string_fn get_error_string;
};

struct gpu_allocation {
	void *addr;
	size_t used_size;
	size_t alloc_size;
	int dmabuf_fd;
	uint64_t dmabuf_offset;
};

struct worker;

struct io_task {
	struct worker *worker;
	void *buf;
	uint64_t submit_tsc;
	bool write;
	bool in_flight;
	struct spdk_nvme_ns_cmd_ext_io_opts io_opts;
};

struct worker {
	uint32_t id;
	uint32_t core;
	struct spdk_nvme_qpair *qpair;
	struct gpu_allocation allocation;
	struct io_task *tasks;
	struct spdk_histogram_data *histogram;
	uint64_t range_start_lba;
	uint64_t range_end_lba;
	uint64_t next_lba;
	uint64_t completed;
	uint64_t errors;
	uint64_t latency_tsc;
	uint64_t min_latency_tsc;
	uint64_t max_latency_tsc;
	uint64_t finish_tsc;
	uint32_t outstanding;
	bool measuring;
};

struct cuda_pin_handle {
	struct gpu_allocation *allocation;
	uint64_t offset;
};

struct latency_percentiles {
	uint64_t p50;
	uint64_t p99;
	uint64_t p999;
};

static struct cuda_driver g_cuda;
static struct spdk_nvme_transport_id g_trid;
static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns *g_ns;
static struct spdk_memory_domain *g_cuda_domain;
static struct worker *g_workers;
static bool g_provider_registered;
static uint32_t g_num_workers = 1;
static uint32_t g_batch_size = URMA_PERF_DEFAULT_BATCH_SIZE;
static uint32_t g_io_size = URMA_PERF_DEFAULT_IO_SIZE;
static uint32_t g_run_time_sec = URMA_PERF_DEFAULT_RUN_TIME;
static uint32_t g_nsid = 1;
static uint32_t g_gpu_id;
static uint64_t g_start_lba;
static uint64_t g_start_tsc;
static uint64_t g_stop_tsc;
static bool g_read_workload;
static bool g_require_dmabuf;
static bool g_core_mask_set;
static char g_core_mask[32];
static atomic_uint g_ready_workers;
static atomic_bool g_run_started;
static atomic_bool g_failed;

static void
cuda_print_error(const char *operation, CUresult result)
{
	const char *message = NULL;

	if (g_cuda.get_error_string != NULL) {
		g_cuda.get_error_string(result, &message);
	}
	fprintf(stderr, "%s failed: CUDA error %d%s%s%s\n", operation, result,
		message != NULL ? " (" : "", message != NULL ? message : "",
		message != NULL ? ")" : "");
}

static void *
cuda_load_symbol(const char *name, const char *fallback)
{
	void *symbol = dlsym(g_cuda.library, name);

	if (symbol == NULL && fallback != NULL) {
		symbol = dlsym(g_cuda.library, fallback);
	}
	return symbol;
}

#define CUDA_LOAD_REQUIRED(member, name, fallback) \
	do { \
		g_cuda.member = (__typeof__(g_cuda.member))cuda_load_symbol(name, fallback); \
		if (g_cuda.member == NULL) { \
			fprintf(stderr, "Missing CUDA symbol %s\n", name); \
			goto fail; \
		} \
	} while (0)

static int
cuda_driver_init(uint32_t gpu_id)
{
	CUdevice device;
	CUresult result;

	g_cuda.library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
	if (g_cuda.library == NULL) {
		fprintf(stderr, "Unable to load libcuda.so.1: %s\n", dlerror());
		return -ENODEV;
	}
	CUDA_LOAD_REQUIRED(init, "cuInit", NULL);
	CUDA_LOAD_REQUIRED(device_get, "cuDeviceGet", NULL);
	CUDA_LOAD_REQUIRED(ctx_create, "cuCtxCreate_v2", "cuCtxCreate");
	CUDA_LOAD_REQUIRED(ctx_destroy, "cuCtxDestroy_v2", "cuCtxDestroy");
	CUDA_LOAD_REQUIRED(ctx_set_current, "cuCtxSetCurrent", NULL);
	CUDA_LOAD_REQUIRED(mem_alloc, "cuMemAlloc_v2", "cuMemAlloc");
	CUDA_LOAD_REQUIRED(mem_free, "cuMemFree_v2", "cuMemFree");
	CUDA_LOAD_REQUIRED(memcpy_h2d, "cuMemcpyHtoD_v2", "cuMemcpyHtoD");
	CUDA_LOAD_REQUIRED(memcpy_d2h, "cuMemcpyDtoH_v2", "cuMemcpyDtoH");
	g_cuda.pointer_set_attribute = (cuda_pointer_set_attribute_fn)
				       cuda_load_symbol("cuPointerSetAttribute", NULL);
	g_cuda.get_dmabuf = (cuda_get_dmabuf_fn)
			      cuda_load_symbol("cuMemGetHandleForAddressRange", NULL);
	g_cuda.get_error_string = (cuda_get_error_string_fn)
				  cuda_load_symbol("cuGetErrorString", NULL);

	result = g_cuda.init(0);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuInit", result);
		goto fail;
	}
	result = g_cuda.device_get(&device, gpu_id);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuDeviceGet", result);
		goto fail;
	}
	result = g_cuda.ctx_create(&g_cuda.context, 0, device);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuCtxCreate", result);
		goto fail;
	}
	return 0;

fail:
	if (g_cuda.library != NULL) {
		dlclose(g_cuda.library);
	}
	memset(&g_cuda, 0, sizeof(g_cuda));
	return -ENODEV;
}

static void
cuda_driver_fini(void)
{
	if (g_cuda.context != NULL) {
		g_cuda.ctx_set_current(g_cuda.context);
		g_cuda.ctx_destroy(g_cuda.context);
	}
	if (g_cuda.library != NULL) {
		dlclose(g_cuda.library);
	}
	memset(&g_cuda, 0, sizeof(g_cuda));
}

static size_t
align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int
cuda_alloc_buffer(struct gpu_allocation *allocation, size_t size)
{
	CUdeviceptr ptr = 0;
	CUresult result;
	unsigned int sync_memops = 1;

	memset(allocation, 0, sizeof(*allocation));
	allocation->dmabuf_fd = -1;
	allocation->used_size = size;
	allocation->alloc_size = align_up(size, URMA_PERF_GPU_ALIGNMENT);
	result = g_cuda.ctx_set_current(g_cuda.context);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuCtxSetCurrent", result);
		return -EIO;
	}
	result = g_cuda.mem_alloc(&ptr, allocation->alloc_size);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuMemAlloc", result);
		return -ENOMEM;
	}
	allocation->addr = (void *)(uintptr_t)ptr;
	if (g_cuda.pointer_set_attribute != NULL) {
		result = g_cuda.pointer_set_attribute(&sync_memops, URMA_PERF_CUDA_SYNC_MEMOPS, ptr);
		if (result != URMA_PERF_CUDA_SUCCESS) {
			cuda_print_error("cuPointerSetAttribute(SYNC_MEMOPS)", result);
			goto fail;
		}
	}
	if (g_cuda.get_dmabuf != NULL) {
		int fd = -1;

		result = g_cuda.get_dmabuf(&fd, ptr, allocation->alloc_size,
					   URMA_PERF_CUDA_DMABUF_HANDLE, 0);
		if (result == URMA_PERF_CUDA_SUCCESS) {
			allocation->dmabuf_fd = fd;
		} else {
			cuda_print_error("cuMemGetHandleForAddressRange", result);
		}
	}
	return 0;

fail:
	g_cuda.mem_free(ptr);
	memset(allocation, 0, sizeof(*allocation));
	allocation->dmabuf_fd = -1;
	return -EIO;
}

static void
cuda_free_buffer(struct gpu_allocation *allocation)
{
	if (allocation->addr != NULL && allocation->dmabuf_fd >= 0) {
		close(allocation->dmabuf_fd);
	}
	if (allocation->addr != NULL) {
		g_cuda.ctx_set_current(g_cuda.context);
		g_cuda.mem_free((CUdeviceptr)(uintptr_t)allocation->addr);
	}
	memset(allocation, 0, sizeof(*allocation));
	allocation->dmabuf_fd = -1;
}

static struct gpu_allocation *
find_gpu_allocation(void *addr, size_t length)
{
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end;
	uint32_t i;

	if (__builtin_add_overflow(start, length, &end)) {
		return NULL;
	}
	for (i = 0; i < g_num_workers; i++) {
		uintptr_t base = (uintptr_t)g_workers[i].allocation.addr;
		uintptr_t limit = base + g_workers[i].allocation.used_size;

		if (start >= base && end <= limit) {
			return &g_workers[i].allocation;
		}
	}
	return NULL;
}

static int
cuda_provider_pin(void *provider_ctx, void *addr, size_t length, void **pin_handle)
{
	struct gpu_allocation *allocation;
	struct cuda_pin_handle *handle;

	(void)provider_ctx;
	allocation = find_gpu_allocation(addr, length);
	if (allocation == NULL) {
		return -EFAULT;
	}
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		return -ENOMEM;
	}
	handle->allocation = allocation;
	handle->offset = (uintptr_t)addr - (uintptr_t)allocation->addr;
	*pin_handle = handle;
	return 0;
}

static void
cuda_provider_unpin(void *provider_ctx, void *pin_handle)
{
	(void)provider_ctx;
	free(pin_handle);
}

static int
cuda_provider_export_dmabuf(void *provider_ctx, void *pin_handle, int *fd, uint64_t *offset)
{
	struct cuda_pin_handle *handle = pin_handle;

	(void)provider_ctx;
	if (handle == NULL || handle->allocation->dmabuf_fd < 0) {
		return -ENOTSUP;
	}
	*fd = handle->allocation->dmabuf_fd;
	*offset = handle->allocation->dmabuf_offset + handle->offset;
	return 0;
}

static const struct spdk_nvme_urma_memory_provider g_cuda_provider = {
	.name = "cuda-urma-perf",
	.type = SPDK_NVME_URMA_MEM_CUDA,
	.pin = cuda_provider_pin,
	.unpin = cuda_provider_unpin,
	.export_dmabuf = cuda_provider_export_dmabuf,
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	(void)cb_ctx;
	(void)trid;
	opts->num_io_queues = g_num_workers;
	opts->io_queue_size = g_batch_size + 1;
	opts->io_queue_requests = spdk_max(g_batch_size * 2, 32U);
	opts->keep_alive_timeout_ms = 0;
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ns *ns;

	(void)cb_ctx;
	(void)trid;
	(void)opts;

	if (g_ctrlr != NULL) {
		fprintf(stderr, "More than one controller matched; using the first one\n");
		spdk_nvme_detach(ctrlr);
		return;
	}
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, g_nsid);
	if (ns == NULL || !spdk_nvme_ns_is_active(ns)) {
		fprintf(stderr, "Namespace %u is not active\n", g_nsid);
		spdk_nvme_detach(ctrlr);
		return;
	}
	g_ctrlr = ctrlr;
	g_ns = ns;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_task *task = arg;
	struct worker *worker = task->worker;
	uint64_t latency = spdk_get_ticks() - task->submit_tsc;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion_ext(worker->qpair, completion,
				task->write ? SPDK_NVME_OPC_WRITE : SPDK_NVME_OPC_READ);
		worker->errors++;
		atomic_store_explicit(&g_failed, true, memory_order_release);
	} else if (worker->measuring) {
		worker->completed++;
		worker->latency_tsc += latency;
		worker->min_latency_tsc = spdk_min(worker->min_latency_tsc, latency);
		worker->max_latency_tsc = spdk_max(worker->max_latency_tsc, latency);
		spdk_histogram_data_tally(worker->histogram, latency);
	}
	task->in_flight = false;
	worker->outstanding--;
}

static int
submit_io(struct worker *worker, struct io_task *task, bool write, uint64_t lba)
{
	uint32_t sector_size = spdk_nvme_ns_get_sector_size(g_ns);
	uint32_t lba_count = g_io_size / sector_size;
	int rc;

	task->write = write;
	task->submit_tsc = spdk_get_ticks();
	task->in_flight = true;
	worker->outstanding++;
	if (write) {
		rc = spdk_nvme_ns_cmd_write_ext(g_ns, worker->qpair, task->buf, lba, lba_count,
						io_complete, task, &task->io_opts);
	} else {
		rc = spdk_nvme_ns_cmd_read_ext(g_ns, worker->qpair, task->buf, lba, lba_count,
					       io_complete, task, &task->io_opts);
	}
	if (rc != 0) {
		task->in_flight = false;
		worker->outstanding--;
		return rc;
	}
	return 0;
}

static int
wait_for_task(struct worker *worker, struct io_task *task)
{
	while (task->in_flight) {
		int rc = spdk_nvme_qpair_process_completions(worker->qpair, 0);

		if (rc < 0) {
			return rc;
		}
	}
	return atomic_load_explicit(&g_failed, memory_order_acquire) ? -EIO : 0;
}

static int
verify_gpu_to_ssd_path(struct worker *worker)
{
	struct io_task *task = &worker->tasks[0];
	uint8_t *expected = malloc(g_io_size);
	uint8_t *actual = calloc(1, g_io_size);
	CUresult result;
	int rc = -EIO;
	uint32_t i;

	if (expected == NULL || actual == NULL) {
		goto out;
	}
	for (i = 0; i < g_io_size; i++) {
		expected[i] = (uint8_t)(i * 131U + 17U);
	}
	result = g_cuda.ctx_set_current(g_cuda.context);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuCtxSetCurrent", result);
		goto out;
	}
	result = g_cuda.memcpy_h2d((CUdeviceptr)(uintptr_t)task->buf, expected, g_io_size);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuMemcpyHtoD", result);
		goto out;
	}
	if (submit_io(worker, task, true, worker->range_start_lba) != 0 ||
	    wait_for_task(worker, task) != 0) {
		fprintf(stderr, "Verification WRITE failed\n");
		goto out;
	}
	result = g_cuda.memcpy_h2d((CUdeviceptr)(uintptr_t)task->buf, actual, g_io_size);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuMemcpyHtoD(clear)", result);
		goto out;
	}
	if (submit_io(worker, task, false, worker->range_start_lba) != 0 ||
	    wait_for_task(worker, task) != 0) {
		fprintf(stderr, "Verification READ failed\n");
		goto out;
	}
	result = g_cuda.memcpy_d2h(actual, (CUdeviceptr)(uintptr_t)task->buf, g_io_size);
	if (result != URMA_PERF_CUDA_SUCCESS) {
		cuda_print_error("cuMemcpyDtoH", result);
		goto out;
	}
	if (memcmp(expected, actual, g_io_size) != 0) {
		fprintf(stderr, "Verification failed: SSD data does not match the GPU pattern\n");
		goto out;
	}
	printf("Preflight GPU WRITE + READ verification passed at LBA %" PRIu64 "\n",
	       worker->range_start_lba);
	rc = 0;
out:
	free(expected);
	free(actual);
	return rc;
}

static uint64_t
worker_next_lba(struct worker *worker)
{
	uint64_t lba = worker->next_lba;
	uint32_t lba_count = g_io_size / spdk_nvme_ns_get_sector_size(g_ns);

	worker->next_lba += lba_count;
	if (worker->next_lba + lba_count > worker->range_end_lba) {
		worker->next_lba = worker->range_start_lba;
	}
	return lba;
}

static int
work_fn(void *arg)
{
	struct worker *worker = arg;
	struct spdk_nvme_io_qpair_opts qpair_opts;
	uint32_t i;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qpair_opts, sizeof(qpair_opts));
	qpair_opts.io_queue_size = g_batch_size + 1;
	qpair_opts.io_queue_requests = spdk_max(g_batch_size * 2, 32U);
	worker->qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &qpair_opts, sizeof(qpair_opts));
	if (worker->qpair == NULL) {
		fprintf(stderr, "Worker %u could not allocate an I/O qpair\n", worker->id);
		atomic_store_explicit(&g_failed, true, memory_order_release);
	}
	atomic_fetch_add_explicit(&g_ready_workers, 1, memory_order_acq_rel);

	if (worker->id == 0) {
		while (atomic_load_explicit(&g_ready_workers, memory_order_acquire) != g_num_workers) {
			spdk_pause();
		}
		if (!atomic_load_explicit(&g_failed, memory_order_acquire) &&
		    verify_gpu_to_ssd_path(worker) != 0) {
			atomic_store_explicit(&g_failed, true, memory_order_release);
		}
		if (!atomic_load_explicit(&g_failed, memory_order_acquire)) {
			spdk_nvme_urma_reset_memory_stats();
			g_start_tsc = spdk_get_ticks();
			g_stop_tsc = g_start_tsc + g_run_time_sec * spdk_get_ticks_hz();
		}
		atomic_store_explicit(&g_run_started, true, memory_order_release);
	} else {
		while (!atomic_load_explicit(&g_run_started, memory_order_acquire)) {
			spdk_pause();
		}
	}

	if (atomic_load_explicit(&g_failed, memory_order_acquire)) {
		goto out;
	}
	worker->measuring = true;
	while (spdk_get_ticks() < g_stop_tsc &&
	       !atomic_load_explicit(&g_failed, memory_order_acquire)) {
		for (i = 0; i < g_batch_size; i++) {
			struct io_task *task = &worker->tasks[i];

			if (!task->in_flight && submit_io(worker, task, !g_read_workload,
							 worker_next_lba(worker)) != 0) {
				fprintf(stderr, "Worker %u failed to submit I/O\n", worker->id);
				atomic_store_explicit(&g_failed, true, memory_order_release);
				break;
			}
		}
		if (spdk_nvme_qpair_process_completions(worker->qpair, 0) < 0) {
			atomic_store_explicit(&g_failed, true, memory_order_release);
			break;
		}
	}
	while (worker->outstanding != 0) {
		if (spdk_nvme_qpair_process_completions(worker->qpair, 0) < 0) {
			atomic_store_explicit(&g_failed, true, memory_order_release);
			break;
		}
	}
	worker->finish_tsc = spdk_get_ticks();
out:
	if (worker->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(worker->qpair);
		worker->qpair = NULL;
	}
	return 0;
}

static void
collect_percentiles(void *ctx, uint64_t start, uint64_t end, uint64_t count,
		    uint64_t total, uint64_t so_far)
{
	struct latency_percentiles *percentiles = ctx;

	(void)start;

	if (count == 0 || total == 0) {
		return;
	}
	if (percentiles->p50 == 0 && so_far * 1000 >= total * 500) {
		percentiles->p50 = end;
	}
	if (percentiles->p99 == 0 && so_far * 1000 >= total * 990) {
		percentiles->p99 = end;
	}
	if (percentiles->p999 == 0 && so_far * 1000 >= total * 999) {
		percentiles->p999 = end;
	}
}

static void
print_results(void)
{
	struct spdk_histogram_data *aggregate = spdk_histogram_data_alloc();
	struct spdk_nvme_urma_memory_stats memory_stats;
	struct latency_percentiles percentiles = {};
	uint64_t total_ios = 0, total_latency = 0, min_latency = UINT64_MAX, max_latency = 0;
	uint64_t finish_tsc = g_stop_tsc;
	double ticks_hz = spdk_get_ticks_hz();
	double seconds, bandwidth_mib, iops;
	uint32_t i;

	if (aggregate == NULL) {
		fprintf(stderr, "Unable to allocate aggregate latency histogram\n");
		return;
	}
	for (i = 0; i < g_num_workers; i++) {
		struct worker *worker = &g_workers[i];

		printf("thread=%u core=%u completed=%" PRIu64 " errors=%" PRIu64 "\n",
		       worker->id, worker->core, worker->completed, worker->errors);
		total_ios += worker->completed;
		total_latency += worker->latency_tsc;
		min_latency = spdk_min(min_latency, worker->min_latency_tsc);
		max_latency = spdk_max(max_latency, worker->max_latency_tsc);
		finish_tsc = spdk_max(finish_tsc, worker->finish_tsc);
		spdk_histogram_data_merge(aggregate, worker->histogram);
	}
	seconds = (finish_tsc - g_start_tsc) / ticks_hz;
	bandwidth_mib = seconds > 0 ? (double)total_ios * g_io_size / (1024 * 1024) / seconds : 0;
	iops = seconds > 0 ? total_ios / seconds : 0;
	spdk_histogram_data_iterate(aggregate, collect_percentiles, &percentiles);
	spdk_nvme_urma_get_memory_stats(&memory_stats);

	printf("\nNVMe/URMA GPU -> remote SSD result\n");
	printf("operation=%s io_size=%u threads=%u batch_size=%u elapsed=%.6f s\n",
	       g_read_workload ? "read" : "write", g_io_size, g_num_workers,
	       g_batch_size, seconds);
	printf("completed=%" PRIu64 " bandwidth=%.2f MiB/s IOPS=%.2f\n",
	       total_ios, bandwidth_mib, iops);
	if (total_ios != 0) {
		printf("latency_us avg=%.3f min=%.3f p50=%.3f p99=%.3f p99.9=%.3f max=%.3f\n",
		       total_latency * 1000000.0 / ticks_hz / total_ios,
		       min_latency * 1000000.0 / ticks_hz,
		       percentiles.p50 * 1000000.0 / ticks_hz,
		       percentiles.p99 * 1000000.0 / ticks_hz,
		       percentiles.p999 * 1000000.0 / ticks_hz,
		       max_latency * 1000000.0 / ticks_hz);
	}
	printf("memory_registration accelerator=%" PRIu64 " dmabuf=%" PRIu64
	       " peer_memory_fallback=%" PRIu64 " failures=%" PRIu64 "\n",
	       memory_stats.accelerator_registrations, memory_stats.dmabuf_registrations,
	       memory_stats.peer_memory_registrations, memory_stats.registration_failures);
	spdk_histogram_data_free(aggregate);
}

static int
parse_u64(const char *value, uint64_t min, uint64_t max, uint64_t *result)
{
	char *end = NULL;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
		return -EINVAL;
	}
	*result = parsed;
	return 0;
}

static void
usage(const char *program)
{
	printf("%s options:\n", program);
	printf("  -r, --transport <trid>  required, for example: trtype:URMA adrfam:IPv4 "
	       "traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1\n");
	printf("  -w, --workload <op>     read or write (default: write)\n");
	printf("  -o, --io-size <bytes>   I/O size (default: %u)\n", URMA_PERF_DEFAULT_IO_SIZE);
	printf("  -T, --threads <count>   worker/qpair count (default: 1, maximum: 64)\n");
	printf("  -b, --batch-size <n>    maximum outstanding I/O per worker (default: %u)\n",
	       URMA_PERF_DEFAULT_BATCH_SIZE);
	printf("  -t, --time <seconds>    measurement time (default: %u)\n",
	       URMA_PERF_DEFAULT_RUN_TIME);
	printf("  -n, --nsid <id>         namespace ID (default: 1)\n");
	printf("  -g, --gpu <id>          CUDA GPU ordinal (default: 0)\n");
	printf("  -l, --start-lba <lba>   first destructive test LBA (default: 0)\n");
	printf("  -m, --core-mask <mask>  SPDK core mask; default selects the first T cores\n");
	printf("      --require-dmabuf    fail if any timed GPU registration uses peer-memory fallback\n");
	printf("  -h, --help              show this help\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	static const struct option options[] = {
		{"transport", required_argument, NULL, 'r'},
		{"workload", required_argument, NULL, 'w'},
		{"io-size", required_argument, NULL, 'o'},
		{"threads", required_argument, NULL, 'T'},
		{"batch-size", required_argument, NULL, 'b'},
		{"time", required_argument, NULL, 't'},
		{"nsid", required_argument, NULL, 'n'},
		{"gpu", required_argument, NULL, 'g'},
		{"start-lba", required_argument, NULL, 'l'},
		{"core-mask", required_argument, NULL, 'm'},
		{"require-dmabuf", no_argument, NULL, 256},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	bool trid_set = false;
	int option;
	uint64_t value;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_URMA);
	while ((option = getopt_long(argc, argv, "r:w:o:T:b:t:n:g:l:m:h", options, NULL)) != -1) {
		switch (option) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				return -EINVAL;
			}
			trid_set = true;
			break;
		case 'w':
			if (strcmp(optarg, "read") == 0) {
				g_read_workload = true;
			} else if (strcmp(optarg, "write") == 0) {
				g_read_workload = false;
			} else {
				return -EINVAL;
			}
			break;
		case 'o':
			if (parse_u64(optarg, 512, UINT32_MAX, &value) != 0) {
				return -EINVAL;
			}
			g_io_size = (uint32_t)value;
			break;
		case 'T':
			if (parse_u64(optarg, 1, 64, &value) != 0) {
				return -EINVAL;
			}
			g_num_workers = (uint32_t)value;
			break;
		case 'b':
			if (parse_u64(optarg, 1, UINT16_MAX - 1, &value) != 0) {
				return -EINVAL;
			}
			g_batch_size = (uint32_t)value;
			break;
		case 't':
			if (parse_u64(optarg, 1, UINT32_MAX, &value) != 0) {
				return -EINVAL;
			}
			g_run_time_sec = (uint32_t)value;
			break;
		case 'n':
			if (parse_u64(optarg, 1, UINT32_MAX, &value) != 0) {
				return -EINVAL;
			}
			g_nsid = (uint32_t)value;
			break;
		case 'g':
			if (parse_u64(optarg, 0, INT_MAX, &value) != 0) {
				return -EINVAL;
			}
			g_gpu_id = (uint32_t)value;
			break;
		case 'l':
			if (parse_u64(optarg, 0, UINT64_MAX, &g_start_lba) != 0) {
				return -EINVAL;
			}
			break;
		case 'm':
			env_opts->core_mask = optarg;
			g_core_mask_set = true;
			break;
		case 256:
			g_require_dmabuf = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			return -EINVAL;
		}
	}
	if (!trid_set || g_trid.trtype != SPDK_NVME_TRANSPORT_URMA || g_trid.subnqn[0] == '\0') {
		fprintf(stderr, "A URMA transport ID including subnqn is required\n");
		return -EINVAL;
	}
	if (!g_core_mask_set) {
		uint64_t mask = g_num_workers == 64 ? UINT64_MAX : (1ULL << g_num_workers) - 1;

		snprintf(g_core_mask, sizeof(g_core_mask), "0x%" PRIx64, mask);
		env_opts->core_mask = g_core_mask;
	}
	return 0;
}

static int
prepare_workers(void)
{
	struct spdk_memory_domain_ctx domain_ctx = {};
	uint64_t total_sectors, available_sectors, sectors_per_worker, lbas_per_io;
	uint32_t core, main_core, worker_count = 1, i, j;
	int rc;

	g_workers = calloc(g_num_workers, sizeof(*g_workers));
	if (g_workers == NULL) {
		return -ENOMEM;
	}
	main_core = spdk_env_get_current_core();
	g_workers[0].id = 0;
	g_workers[0].core = main_core;
	SPDK_ENV_FOREACH_CORE(core) {
		if (worker_count == g_num_workers) {
			break;
		}
		if (core == main_core) {
			continue;
		}
		g_workers[worker_count].id = worker_count;
		g_workers[worker_count].core = core;
		worker_count++;
	}
	if (worker_count != g_num_workers) {
		fprintf(stderr, "Core mask provides %u cores, but %u threads were requested\n",
			worker_count, g_num_workers);
		return -EINVAL;
	}
	if (g_io_size % spdk_nvme_ns_get_sector_size(g_ns) != 0 ||
	    g_io_size > spdk_nvme_ns_get_max_io_xfer_size(g_ns)) {
		fprintf(stderr, "I/O size must be sector-aligned and no greater than %u\n",
			spdk_nvme_ns_get_max_io_xfer_size(g_ns));
		return -EINVAL;
	}
	total_sectors = spdk_nvme_ns_get_num_sectors(g_ns);
	lbas_per_io = g_io_size / spdk_nvme_ns_get_sector_size(g_ns);
	if (g_start_lba >= total_sectors) {
		return -ERANGE;
	}
	available_sectors = total_sectors - g_start_lba;
	sectors_per_worker = available_sectors / g_num_workers;
	sectors_per_worker -= sectors_per_worker % lbas_per_io;
	if (sectors_per_worker < lbas_per_io) {
		fprintf(stderr, "Namespace range is too small for the requested thread count\n");
		return -ENOSPC;
	}

	for (i = 0; i < g_num_workers; i++) {
		struct worker *worker = &g_workers[i];

		worker->range_start_lba = g_start_lba + i * sectors_per_worker;
		worker->range_end_lba = worker->range_start_lba + sectors_per_worker;
		worker->next_lba = worker->range_start_lba;
		worker->min_latency_tsc = UINT64_MAX;
		worker->tasks = calloc(g_batch_size, sizeof(*worker->tasks));
		worker->histogram = spdk_histogram_data_alloc();
		if (worker->tasks == NULL || worker->histogram == NULL ||
		    cuda_alloc_buffer(&worker->allocation, (size_t)g_io_size * g_batch_size) != 0) {
			return -ENOMEM;
		}
		for (j = 0; j < g_batch_size; j++) {
			worker->tasks[j].worker = worker;
			worker->tasks[j].buf = (void *)((uintptr_t)worker->allocation.addr +
							     (size_t)j * g_io_size);
			worker->tasks[j].io_opts.size = sizeof(worker->tasks[j].io_opts);
		}
	}

	domain_ctx.size = sizeof(domain_ctx);
	domain_ctx.user_ctx = &g_cuda;
	domain_ctx.user_ctx_size = sizeof(&g_cuda);
	rc = spdk_memory_domain_create(&g_cuda_domain, SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START,
				       &domain_ctx, "cuda:spdk-urma-perf");
	if (rc != 0) {
		return rc;
	}
	for (i = 0; i < g_num_workers; i++) {
		for (j = 0; j < g_batch_size; j++) {
			g_workers[i].tasks[j].io_opts.memory_domain = g_cuda_domain;
		}
	}
	rc = spdk_nvme_urma_register_memory_provider(&g_cuda_provider);
	if (rc == 0) {
		g_provider_registered = true;
	}
	return rc;
}

static void
cleanup_workers(void)
{
	uint32_t i;

	if (g_provider_registered) {
		spdk_nvme_urma_unregister_memory_provider(SPDK_NVME_URMA_MEM_CUDA);
		g_provider_registered = false;
	}
	if (g_cuda_domain != NULL) {
		spdk_memory_domain_destroy(g_cuda_domain);
		g_cuda_domain = NULL;
	}
	if (g_workers == NULL) {
		return;
	}
	for (i = 0; i < g_num_workers; i++) {
		cuda_free_buffer(&g_workers[i].allocation);
		spdk_histogram_data_free(g_workers[i].histogram);
		free(g_workers[i].tasks);
	}
	free(g_workers);
	g_workers = NULL;
}

static int
run_workers(void)
{
	struct worker *main_worker = NULL;
	uint32_t main_core = spdk_env_get_current_core();
	uint32_t i;

	for (i = 0; i < g_num_workers; i++) {
		if (g_workers[i].core == main_core) {
			main_worker = &g_workers[i];
		} else if (spdk_env_thread_launch_pinned(g_workers[i].core, work_fn,
							  &g_workers[i]) != 0) {
			fprintf(stderr, "Unable to launch worker on core %u\n", g_workers[i].core);
			atomic_store_explicit(&g_failed, true, memory_order_release);
			atomic_store_explicit(&g_run_started, true, memory_order_release);
			spdk_env_thread_wait_all();
			return -EIO;
		}
	}
	if (main_worker == NULL) {
		fprintf(stderr, "The main SPDK core is not assigned to a worker\n");
		atomic_store_explicit(&g_failed, true, memory_order_release);
		atomic_store_explicit(&g_run_started, true, memory_order_release);
		spdk_env_thread_wait_all();
		return -EINVAL;
	}
	work_fn(main_worker);
	spdk_env_thread_wait_all();
	return atomic_load_explicit(&g_failed, memory_order_acquire) ? -EIO : 0;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts env_opts;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;
	struct spdk_nvme_urma_memory_stats memory_stats;
	int rc;

	env_opts.opts_size = sizeof(env_opts);
	spdk_env_opts_init(&env_opts);
	if (parse_args(argc, argv, &env_opts) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	env_opts.name = "nvme_urma_gpu_perf";
	if (spdk_env_init(&env_opts) < 0) {
		fprintf(stderr, "Unable to initialize the SPDK environment\n");
		return EXIT_FAILURE;
	}
	if (cuda_driver_init(g_gpu_id) != 0) {
		rc = EXIT_FAILURE;
		goto out_env;
	}
	printf("Connecting to %s:%s, subnqn=%s\n", g_trid.traddr, g_trid.trsvcid,
	       g_trid.subnqn);
	if (spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL) != 0 || g_ctrlr == NULL) {
		fprintf(stderr, "Unable to attach the NVMe/URMA controller\n");
		rc = EXIT_FAILURE;
		goto out_cuda;
	}
	if (prepare_workers() != 0) {
		rc = EXIT_FAILURE;
		cleanup_workers();
		goto out_ctrlr;
	}
	printf("WARNING: this test overwrites namespace %u beginning at LBA %" PRIu64 "\n",
	       g_nsid, g_start_lba);
	rc = run_workers();
	if (g_start_tsc != 0) {
		print_results();
	}
	spdk_nvme_urma_get_memory_stats(&memory_stats);
	if (g_require_dmabuf &&
	    (memory_stats.dmabuf_registrations == 0 || memory_stats.peer_memory_registrations != 0)) {
		fprintf(stderr, "--require-dmabuf failed: the run used peer-memory fallback\n");
		rc = -ENOTSUP;
	}
	cleanup_workers();
out_ctrlr:
	spdk_nvme_detach_async(g_ctrlr, &detach_ctx);
	if (detach_ctx != NULL) {
		spdk_nvme_detach_poll(detach_ctx);
	}
out_cuda:
	cuda_driver_fini();
out_env:
	spdk_env_fini();
	return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
