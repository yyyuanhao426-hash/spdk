/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#include "spdk/stdinc.h"
#include "spdk/config.h"
#include "nvme_urma_internal.h"

static urma_target_seg_t g_seg;
static urma_seg_cfg_t g_cfg;
static unsigned int g_register_calls, g_unregister_calls, g_pin_calls, g_unpin_calls;
static unsigned int g_export_calls, g_dmabuf_calls;
static int g_register_error, g_pin_error, g_export_error, g_dmabuf_error, g_fd = -1;
static char g_pin_handle;
static bool g_register_without_errno;

void
spdk_log(enum spdk_log_level level, const char *file, int line, const char *func,
	 const char *format, ...)
{
	/* A diagnostic sink must not change the registration error returned. */
	errno = EINVAL;
}


urma_target_seg_t *
urma_register_seg(urma_context_t *ctx, urma_seg_cfg_t *cfg)
{
	assert(ctx != NULL);
	g_register_calls++;
	g_cfg = *cfg;
	if (g_register_without_errno) {
		return NULL;
	}
	if (g_register_error != 0) {
		errno = g_register_error;
		return NULL;
	}
	return &g_seg;
}

urma_status_t
urma_unregister_seg(urma_target_seg_t *seg)
{
	assert(seg == &g_seg);
	g_unregister_calls++;
	return URMA_SUCCESS;
}

#ifdef SPDK_CONFIG_URMA_DMABUF
urma_target_seg_t *
urma_register_seg_dmabuf(urma_context_t *ctx, urma_seg_cfg_t *cfg, int fd,
			 uint64_t offset)
{
	assert(ctx != NULL);
	assert(fd == g_fd && offset == 17);
	g_dmabuf_calls++;
	g_cfg = *cfg;
	if (g_dmabuf_error != 0) {
		errno = g_dmabuf_error;
		return NULL;
	}
	return &g_seg;
}
#endif

static int
provider_pin(void *ctx, void *addr, size_t length, void **handle)
{
	assert(addr != NULL && length != 0);
	g_pin_calls++;
	if (g_pin_error != 0) {
		return g_pin_error;
	}
	*handle = &g_pin_handle;
	return 0;
}

static void
provider_unpin(void *ctx, void *handle)
{
	assert(handle == &g_pin_handle);
	g_unpin_calls++;
}

static int
provider_export(void *ctx, void *handle, int *fd, uint64_t *offset)
{
	assert(handle == &g_pin_handle);
	g_export_calls++;
	if (g_export_error != 0) {
		return g_export_error;
	}
	g_fd = open("/dev/null", O_RDONLY);
	assert(g_fd >= 0);
	*fd = g_fd;
	*offset = 17;
	return 0;
}

static struct spdk_nvme_urma_memory_provider g_provider = {
	.name = "test",
	.type = SPDK_NVME_URMA_MEM_CUDA,
	.pin = provider_pin,
	.unpin = provider_unpin,
	.export_dmabuf = provider_export,
};

static void
reset_counters(void)
{
	g_register_calls = g_unregister_calls = g_pin_calls = g_unpin_calls = 0;
	g_export_calls = g_dmabuf_calls = 0;
	g_register_error = g_pin_error = g_export_error = g_dmabuf_error = 0;
	g_fd = -1;
	spdk_nvme_urma_reset_memory_stats();
}

static void
check_host(void)
{
	urma_context_t ctx = {};
	struct spdk_nvme_urma_memory_region *region;
	struct spdk_nvme_urma_memory_stats stats;

	reset_counters();
	assert(spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_HOST, &region) == 0);
	assert(g_register_calls == 1 && g_pin_calls == 0 && g_dmabuf_calls == 0);
	assert(g_cfg.va == 0x100000 && g_cfg.len == 8192);
#ifdef SPDK_CONFIG_URMA_PEERMEM
	assert(g_cfg.is_gpu_seg == 0);
#endif
	spdk_nvme_urma_unregister_memory(region);
	assert(g_unregister_calls == 1 && g_unpin_calls == 0);

	reset_counters();
	g_register_error = EFAULT;
	assert(spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_HOST, &region) == -EFAULT);
	assert(region == NULL);
	spdk_nvme_urma_get_memory_stats(&stats);
	assert(stats.registration_failures == 1);

	/* A provider may fail without setting errno. Ambient ENOENT must not
	 * turn an Identify registration failure into "No such file". */
	reset_counters();
	g_register_without_errno = true;
	errno = ENOENT;
	assert(spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_HOST, &region) == -EIO);
	assert(region == NULL);
	g_register_without_errno = false;
}

static void
check_accelerator(void)
{
	urma_context_t ctx = {};
	struct spdk_nvme_urma_memory_region *region;
	int rc;

	reset_counters();
	rc = spdk_nvme_urma_register_memory_provider(&g_provider);
#if !defined(SPDK_CONFIG_URMA_PEERMEM) && !defined(SPDK_CONFIG_URMA_DMABUF)
	assert(rc == -ENOTSUP);
	assert(spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_CUDA, &region) == -ENOTSUP);
	assert(region == NULL && g_pin_calls == 0 && g_register_calls == 0);
	assert(spdk_nvme_urma_unregister_memory_provider(g_provider.type) == -ENOTSUP);
#else
	assert(rc == 0);
	assert(spdk_nvme_urma_register_memory_provider(&g_provider) == -EEXIST);
	assert(spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_CUDA, &region) == 0);
	assert(g_pin_calls == 1);
	assert(spdk_nvme_urma_unregister_memory_provider(g_provider.type) == -EBUSY);
#ifdef SPDK_CONFIG_URMA_DMABUF
	assert(g_dmabuf_calls == 1 && g_register_calls == 0);
	assert(g_cfg.va == 0x100040);
#else
	assert(g_export_calls == 0 && g_dmabuf_calls == 0 && g_register_calls == 1);
	assert(g_cfg.va == 0x100000 && g_cfg.is_gpu_seg == 1);
#endif
	spdk_nvme_urma_unregister_memory(region);
	assert(g_unpin_calls == 1 && g_unregister_calls == 1);
	if (g_fd >= 0) {
		assert(fcntl(g_fd, F_GETFD) == -1 && errno == EBADF);
	}

	/* A failed export may fall back only when peermem was compiled. */
	reset_counters();
	g_export_error = -EOPNOTSUPP;
	rc = spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_CUDA, &region);
#ifdef SPDK_CONFIG_URMA_PEERMEM
	assert(rc == 0 && g_cfg.is_gpu_seg == 1 && g_register_calls == 1);
	spdk_nvme_urma_unregister_memory(region);
#else
	assert(rc == -EOPNOTSUPP && region == NULL && g_register_calls == 0);
#endif
	assert(g_unpin_calls == 1);

#ifdef SPDK_CONFIG_URMA_DMABUF
	/* An unsuccessful DMA-BUF registration must not register a GPU VA as HOST. */
	reset_counters();
	g_dmabuf_error = EOPNOTSUPP;
	rc = spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_CUDA, &region);
#ifdef SPDK_CONFIG_URMA_PEERMEM
	assert(rc == 0 && g_register_calls == 1 && g_cfg.is_gpu_seg == 1);
	spdk_nvme_urma_unregister_memory(region);
#else
	assert(rc == -EOPNOTSUPP && region == NULL && g_register_calls == 0);
#endif
	assert(g_unpin_calls == 1);
	assert(fcntl(g_fd, F_GETFD) == -1 && errno == EBADF);
#endif

	reset_counters();
	g_pin_error = -ENOMEM;
	assert(spdk_nvme_urma_register_memory(&ctx, (void *)0x100040, 7000,
					    SPDK_NVME_URMA_MEM_CUDA, &region) == -ENOMEM);
	assert(region == NULL && g_unpin_calls == 0 && g_register_calls == 0);
	assert(spdk_nvme_urma_unregister_memory_provider(g_provider.type) == 0);

	g_provider.export_dmabuf = NULL;
	rc = spdk_nvme_urma_register_memory_provider(&g_provider);
#ifdef SPDK_CONFIG_URMA_PEERMEM
	assert(rc == 0);
	assert(spdk_nvme_urma_unregister_memory_provider(g_provider.type) == 0);
#else
	assert(rc == -ENOTSUP);
#endif
#endif
}

int
main(void)
{
	check_host();
	check_accelerator();
	puts("URMA memory registration passed");
	return 0;
}
