/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#ifndef SPDK_NVME_URMA_H
#define SPDK_NVME_URMA_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Memory kinds accepted by the NVMe/URMA registration layer. */
enum spdk_nvme_urma_memory_type {
	SPDK_NVME_URMA_MEM_HOST = 0,
	SPDK_NVME_URMA_MEM_CUDA,
	SPDK_NVME_URMA_MEM_ROCM,
	SPDK_NVME_URMA_MEM_NPU,
	SPDK_NVME_URMA_MEM_XDS,
};

/**
 * Optional accelerator-memory provider.
 *
 * XDS, CUDA, ROCm and NPU integrations can use this interface without adding
 * accelerator SDK headers to SPDK.  pin() must keep the allocation alive until
 * unpin() is called.  export_dmabuf() is optional; a provider may instead rely
 * on URMA peer-memory registration of the returned device virtual address.
 */
struct spdk_nvme_urma_memory_provider {
	const char *name;
	enum spdk_nvme_urma_memory_type type;
	int (*pin)(void *provider_ctx, void *addr, size_t length, void **pin_handle);
	void (*unpin)(void *provider_ctx, void *pin_handle);
	int (*export_dmabuf)(void *provider_ctx, void *pin_handle, int *fd,
			      uint64_t *offset);
	void *provider_ctx;
};

struct spdk_nvme_urma_memory_region;

/** Diagnostic counters for URMA memory registration paths. */
struct spdk_nvme_urma_memory_stats {
	uint64_t host_registrations;
	uint64_t accelerator_registrations;
	uint64_t dmabuf_registrations;
	uint64_t peer_memory_registrations;
	uint64_t registration_failures;
};

/** Register an accelerator-memory provider. Providers are process-global. */
int spdk_nvme_urma_register_memory_provider(
	const struct spdk_nvme_urma_memory_provider *provider);

/** Remove a provider. It must not have outstanding registered regions. */
int spdk_nvme_urma_unregister_memory_provider(enum spdk_nvme_urma_memory_type type);

/**
 * Register host or accelerator memory in an URMA context.
 *
 * urma_context is an opaque urma_context_t pointer.  The returned region owns
 * provider pinning and URMA registration and must be released with
 * spdk_nvme_urma_unregister_memory().
 */
int spdk_nvme_urma_register_memory(void *urma_context, void *addr, size_t length,
				  enum spdk_nvme_urma_memory_type type,
				  struct spdk_nvme_urma_memory_region **region);

void spdk_nvme_urma_unregister_memory(struct spdk_nvme_urma_memory_region *region);

/** Return the local virtual address and size represented by a region. */
void *spdk_nvme_urma_memory_region_get_addr(
	const struct spdk_nvme_urma_memory_region *region);
size_t spdk_nvme_urma_memory_region_get_length(
	const struct spdk_nvme_urma_memory_region *region);

/** Copy the portable URMA segment descriptor for connection exchange. */
int spdk_nvme_urma_memory_region_export(
	const struct spdk_nvme_urma_memory_region *region, void *buf, size_t *length);

/** Read or reset process-wide URMA memory registration diagnostics. */
void spdk_nvme_urma_get_memory_stats(struct spdk_nvme_urma_memory_stats *stats);
void spdk_nvme_urma_reset_memory_stats(void);

/** Modified By Yida: dump per-phase timing breakdown (register/send/completion/release/total) */
void spdk_nvme_urma_dump_timing(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_URMA_H */
