/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#ifndef URMA_PERF_NPU_H
#define URMA_PERF_NPU_H

#include <stddef.h>
#include <stdint.h>

/* NDS: NPU HBM allocation descriptor, layout-compatible with the fields
 * urma_perf.c consumes from struct gpu_allocation (addr/used_size/alloc_size).
 * dmabuf_fd stays -1: AscendCL has no public dmabuf export API, so the only
 * direct-registration route is the URMA peer-memory path (is_gpu_seg=1). */
struct npu_allocation {
	void *addr;
	size_t used_size;
	size_t alloc_size;
	int dmabuf_fd;
};

/* Load libascendcl.so dynamically and bind the device. device_id maps the
 * urma_perf -g option. Returns -ENODEV when the CANN runtime is absent. */
int npu_driver_init(int32_t device_id);

/* Release the CANN runtime (drop device + dlclose). Safe to call uninitialised. */
void npu_driver_fini(void);

/* Bind the calling thread to the NPU context so worker threads can issue
 * aclrtMemcpy (mirrors cuCtxSetCurrent for the staged route). */
int npu_worker_thread_bind(void);

/* Allocate size bytes of NPU HBM, 64KB-aligned (URMA_PERF_NPU_ALIGNMENT),
 * and record it in the module allocation registry for provider pin(). */
int npu_alloc_buffer(struct npu_allocation *allocation, size_t size);

/* Free an HBM allocation by address and drop it from the registry. */
void npu_free_buffer(void *addr);

/* Synchronous HBM <-> host copies (mirror cuMemcpyHtoD/cuMemcpyDtoH). */
int npu_memcpy_h2d(void *device_dst, const void *host_src, size_t size);
int npu_memcpy_d2h(void *host_dst, const void *device_src, size_t size);

/* Register/unregister the NPU memory provider (type SPDK_NVME_URMA_MEM_NPU)
 * with the URMA registration layer. export_dmabuf is intentionally NULL:
 * the registration layer then takes the is_gpu_seg=1 peer-memory branch. */
int npu_provider_register(void);
void npu_provider_unregister(void);

#endif /* URMA_PERF_NPU_H */
