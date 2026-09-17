/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#include "nvme_urma_internal.h"

#define SPDK_URMA_PROVIDER_COUNT (SPDK_NVME_URMA_MEM_XDS + 1)

struct spdk_nvme_urma_memory_region {
	void *addr;
	size_t length;
	enum spdk_nvme_urma_memory_type type;
	urma_target_seg_t *target_seg;
	const struct spdk_nvme_urma_memory_provider *provider;
	void *pin_handle;
	int dmabuf_fd;
};

static pthread_mutex_t g_provider_mutex = PTHREAD_MUTEX_INITIALIZER;
static const struct spdk_nvme_urma_memory_provider *g_providers[SPDK_URMA_PROVIDER_COUNT];
static uint32_t g_provider_refs[SPDK_URMA_PROVIDER_COUNT];
static pthread_mutex_t g_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_runtime_refs;
static bool g_runtime_owned;
static struct spdk_nvme_urma_memory_stats g_memory_stats;
static pthread_mutex_t g_device_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_urma_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static pthread_once_t g_numa_chip_once = PTHREAD_ONCE_INIT;
static uint8_t *g_numa_chip_ids;
static size_t g_numa_chip_id_count;

#define SPDK_URMA_STAT_INC(member) \
	__atomic_fetch_add(&g_memory_stats.member, 1, __ATOMIC_RELAXED)

static int
spdk_urma_runtime_get(void)
{
	int rc = 0;

	pthread_mutex_lock(&g_runtime_mutex);
	if (g_runtime_refs == 0) {
		urma_status_t status = urma_init(NULL);
		if (status != URMA_SUCCESS && status != URMA_EEXIST) {
			rc = -EIO;
		} else {
			g_runtime_owned = status == URMA_SUCCESS;
		}
	}
	if (rc == 0) {
		g_runtime_refs++;
	}
	pthread_mutex_unlock(&g_runtime_mutex);
	return rc;
}

static void
spdk_urma_runtime_put(void)
{
	pthread_mutex_lock(&g_runtime_mutex);
	assert(g_runtime_refs > 0);
	if (--g_runtime_refs == 0 && g_runtime_owned) {
		urma_uninit();
		g_runtime_owned = false;
	}
	pthread_mutex_unlock(&g_runtime_mutex);
}

static urma_transport_mode_t
spdk_urma_parse_mode(const char *value)
{
	if (value != NULL && strcasecmp(value, "RC") == 0) {
		return URMA_TM_RC;
	}
	if (value != NULL && strcasecmp(value, "UM") == 0) {
		return URMA_TM_UM;
	}
	return URMA_TM_RM;
}

static uint32_t
spdk_urma_env_u32(const char *name, uint32_t default_value)
{
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return default_value;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
		SPDK_WARNLOG("Ignoring invalid %s=%s\n", name, value);
		return default_value;
	}
	return (uint32_t)parsed;
}

static int32_t
spdk_urma_env_i32(const char *name, int32_t default_value)
{
	const char *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (value == NULL || value[0] == '\0') {
		return default_value;
	}
	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) {
		SPDK_WARNLOG("Ignoring invalid %s=%s\n", name, value);
		return default_value;
	}
	return (int32_t)parsed;
}

static bool
spdk_urma_env_bool(const char *name, const char *compat_name, bool default_value)
{
	const char *value = getenv(name);

	if ((value == NULL || value[0] == '\0') && compat_name != NULL) {
		value = getenv(compat_name);
	}
	if (value == NULL || value[0] == '\0') {
		return default_value;
	}
	if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
	    strcasecmp(value, "on") == 0) {
		return true;
	}
	if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
	    strcasecmp(value, "off") == 0) {
		return false;
	}
	SPDK_WARNLOG("Ignoring invalid %s=%s\n", name, value);
	return default_value;
}

void
spdk_urma_opts_init(struct spdk_urma_transport_opts *opts)
{
	const char *value;

	memset(opts, 0, sizeof(*opts));
	opts->active_port = -1;
	value = getenv("SPDK_URMA_TRANS_MODE");
	if (value == NULL || value[0] == '\0') {
		value = getenv("MC_URMA_TRANS_MODE");
	}
	opts->transport_mode = spdk_urma_parse_mode(value);
	opts->eid_index = spdk_urma_env_u32("SPDK_URMA_EID_INDEX", 0);
	opts->send_jfc_count = spdk_urma_env_u32("SPDK_URMA_SEND_JFC_COUNT",
			      spdk_urma_env_u32("SPDK_URMA_JFC_COUNT", SPDK_URMA_DEFAULT_JFC_COUNT));
	opts->recv_jfc_count = spdk_urma_env_u32("SPDK_URMA_RECV_JFC_COUNT",
			      spdk_urma_env_u32("SPDK_URMA_JFC_COUNT", SPDK_URMA_DEFAULT_JFC_COUNT));
	opts->jfc_depth = spdk_urma_env_u32("SPDK_URMA_JFC_DEPTH", SPDK_URMA_DEFAULT_JFC_DEPTH);
	opts->num_jetty_per_ep = spdk_urma_env_u32("SPDK_URMA_NUM_JETTY_PER_EP",
				spdk_urma_env_u32("SPDK_URMA_JETTY_COUNT",
						  SPDK_URMA_DEFAULT_JETTY_COUNT));
	opts->jetty_depth = spdk_urma_env_u32("SPDK_URMA_JETTY_DEPTH",
			    SPDK_URMA_DEFAULT_JETTY_DEPTH);
	opts->max_io_size = spdk_urma_env_u32("SPDK_URMA_MAX_IO_SIZE", 131072);
	opts->priority = spdk_urma_env_i32("SPDK_URMA_JETTY_PRIORITY", -1);
	opts->tp_type = URMA_CTP;
	value = getenv("SPDK_URMA_TP_TYPE");
	if (value != NULL && strcasecmp(value, "rtp") == 0) {
		opts->tp_type = URMA_RTP;
	} else if (value != NULL && value[0] != '\0' && strcasecmp(value, "ctp") != 0) {
		SPDK_WARNLOG("Ignoring invalid SPDK_URMA_TP_TYPE=%s (expected ctp or rtp)\n", value);
	}
	opts->capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
	value = getenv("SPDK_URMA_CAPSULE_TRANSPORT");
	if (value != NULL && value[0] != '\0' &&
	    spdk_urma_parse_capsule_transport(value, &opts->capsule_transport) != 0) {
		SPDK_WARNLOG("Ignoring invalid SPDK_URMA_CAPSULE_TRANSPORT=%s\n", value);
		opts->capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
	}
	opts->bonding_balance = spdk_urma_env_bool("SPDK_URMA_BONDING_BALANCE",
				"MC_URMA_BONDING_BALANCE", false);
	opts->bonding_multipath = spdk_urma_env_bool("SPDK_URMA_BONDING_MULTIPATH_ENABLE",
				  "MC_URMA_BONDING_MULTIPATH_ENABLE", false);
	opts->numa_affinity = spdk_urma_env_bool("SPDK_URMA_NUMA_AFFINITY_ENABLE",
			      "MC_UB_NUMA_AFFINITY_ENABLE", false);

	value = getenv("SPDK_URMA_ACTIVE_PORT");
	if (value == NULL || value[0] == '\0') {
		value = getenv("MC_URMA_ACTIVE_PORT");
	}
	if (value != NULL && value[0] != '\0') {
		char *end = NULL;
		long port = strtol(value, &end, 10);
		if (end != value && *end == '\0' && port >= 0 && port < MAX_PORT_CNT) {
			opts->active_port = (int32_t)port;
		}
	}
	value = getenv("SPDK_URMA_DEV_NAME");
	if (value != NULL) {
		snprintf(opts->dev_name, sizeof(opts->dev_name), "%s", value);
	}
}

int
spdk_urma_parse_capsule_transport(const char *value,
				   enum spdk_urma_capsule_transport *transport)
{
	if (value == NULL || transport == NULL) {
		return -EINVAL;
	}
	if (strcasecmp(value, "tcp") == 0) {
		*transport = SPDK_URMA_CAPSULE_TRANSPORT_TCP;
		return 0;
	}
	if (strcasecmp(value, "sendrecv") == 0 ||
	    strcasecmp(value, "send_recv") == 0 ||
	    strcasecmp(value, "urma") == 0) {
		*transport = SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV;
		return 0;
	}
	return -EINVAL;
}

const char *
spdk_urma_capsule_transport_name(enum spdk_urma_capsule_transport transport)
{
	switch (transport) {
	case SPDK_URMA_CAPSULE_TRANSPORT_TCP:
		return "tcp";
	case SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV:
		return "sendrecv";
	default:
		return "unknown";
	}
}

int
spdk_nvme_urma_register_memory_provider(const struct spdk_nvme_urma_memory_provider *provider)
{
	int rc = 0;

	if (provider == NULL || provider->name == NULL || provider->pin == NULL ||
	    provider->unpin == NULL || provider->type <= SPDK_NVME_URMA_MEM_HOST ||
	    provider->type >= SPDK_URMA_PROVIDER_COUNT) {
		return -EINVAL;
	}
	pthread_mutex_lock(&g_provider_mutex);
	if (g_providers[provider->type] != NULL) {
		rc = -EEXIST;
	} else {
		g_providers[provider->type] = provider;
	}
	pthread_mutex_unlock(&g_provider_mutex);
	return rc;
}

int
spdk_nvme_urma_unregister_memory_provider(enum spdk_nvme_urma_memory_type type)
{
	int rc = 0;

	if (type <= SPDK_NVME_URMA_MEM_HOST || type >= SPDK_URMA_PROVIDER_COUNT) {
		return -EINVAL;
	}
	pthread_mutex_lock(&g_provider_mutex);
	if (g_provider_refs[type] != 0) {
		rc = -EBUSY;
	} else {
		g_providers[type] = NULL;
	}
	pthread_mutex_unlock(&g_provider_mutex);
	return rc;
}

/* Modified By Yida(v7): 整池注册表 —— 由 initiator 预注册缓冲或 target iobuf
 * 整池注册填充；I/O 提交路径用 find() 采纳覆盖本 I/O 缓冲的 region，跳过
 * per-I/O register，capsule 携带全区 seg，对端可整池 import 一次。
 * 按共享 urma_context 键控；使用动态表避免 target iobuf chunk 数超过固定槽位。 */

struct nvme_urma_region_entry {
	void *context;
	uintptr_t start;
	uintptr_t end;   /* start + length */
	struct spdk_nvme_urma_memory_region *region;
	TAILQ_ENTRY(nvme_urma_region_entry) link;
};

static TAILQ_HEAD(, nvme_urma_region_entry) g_region_registry =
	TAILQ_HEAD_INITIALIZER(g_region_registry);
static pthread_mutex_t g_region_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

int
nvme_urma_region_registry_add(void *urma_context, void *addr, size_t length,
			      struct spdk_nvme_urma_memory_region *region)
{
	struct nvme_urma_region_entry *e;
	uintptr_t start = (uintptr_t)addr;

	if (urma_context == NULL || addr == NULL || length == 0 || region == NULL ||
	    length > UINTPTR_MAX - start) {
		return -EINVAL;
	}
	e = calloc(1, sizeof(*e));

	if (e == NULL) {
		SPDK_ERRLOG("Unable to add URMA memory region to registry\n");
		return -ENOMEM;
	}
	e->context = urma_context;
	e->start = start;
	e->end = start + length;
	e->region = region;
	pthread_mutex_lock(&g_region_registry_mutex);
	TAILQ_INSERT_TAIL(&g_region_registry, e, link);
	pthread_mutex_unlock(&g_region_registry_mutex);
	return 0;
}

void
nvme_urma_region_registry_remove(struct spdk_nvme_urma_memory_region *region)
{
	struct nvme_urma_region_entry *e, *tmp;

	if (region == NULL) {
		return;
	}
	pthread_mutex_lock(&g_region_registry_mutex);
	TAILQ_FOREACH_SAFE(e, &g_region_registry, link, tmp) {
		if (e->region == region) {
			TAILQ_REMOVE(&g_region_registry, e, link);
			free(e);
		}
	}
	pthread_mutex_unlock(&g_region_registry_mutex);
}

struct spdk_nvme_urma_memory_region *
nvme_urma_region_registry_find(void *urma_context, uint64_t addr, size_t length)
{
	struct spdk_nvme_urma_memory_region *found = NULL;
	struct nvme_urma_region_entry *e;

	if (length > UINT64_MAX - addr) {
		return NULL;
	}
	pthread_mutex_lock(&g_region_registry_mutex);
	TAILQ_FOREACH(e, &g_region_registry, link) {
		if (e->context == urma_context &&
		    addr >= e->start && addr + length <= e->end) {
			found = e->region;
			break;
		}
	}
	pthread_mutex_unlock(&g_region_registry_mutex);
	return found;
}

int
spdk_nvme_urma_register_memory(void *urma_context, void *addr, size_t length,
				  enum spdk_nvme_urma_memory_type type,
				  struct spdk_nvme_urma_memory_region **region_out)
{
	struct spdk_nvme_urma_memory_region *region;
	urma_seg_cfg_t cfg = {};
	int rc;

	if (urma_context == NULL || addr == NULL || length == 0 || region_out == NULL ||
	    type >= SPDK_URMA_PROVIDER_COUNT) {
		return -EINVAL;
	}
	region = calloc(1, sizeof(*region));
	if (region == NULL) {
		return -ENOMEM;
	}
	region->addr = addr;
	region->length = length;
	region->type = type;
	region->dmabuf_fd = -1;
	if (type == SPDK_NVME_URMA_MEM_HOST) {
		SPDK_URMA_STAT_INC(host_registrations);
	} else {
		SPDK_URMA_STAT_INC(accelerator_registrations);
	}

	if (type != SPDK_NVME_URMA_MEM_HOST) {
		pthread_mutex_lock(&g_provider_mutex);
		region->provider = g_providers[type];
		if (region->provider != NULL) {
			g_provider_refs[type]++;
		}
		pthread_mutex_unlock(&g_provider_mutex);
		if (region->provider == NULL) {
			SPDK_URMA_STAT_INC(registration_failures);
			free(region);
			return -ENOTSUP;
		}
		rc = region->provider->pin(region->provider->provider_ctx, addr, length,
					   &region->pin_handle);
		if (rc != 0) {
			SPDK_URMA_STAT_INC(registration_failures);
			goto fail_provider;
		}
	}

	cfg.va = (uint64_t)addr;
	/* Modified by Yin: grant 长度向上取整到 4K，UMMU Table mode 页粒度 */
	cfg.len = SPDK_ALIGN_CEIL(length, (size_t)4096);
	cfg.token_value.token = SPDK_URMA_DEFAULT_TOKEN;
	cfg.flag.bs.token_policy = URMA_TOKEN_NONE;
	cfg.flag.bs.cacheable = URMA_NON_CACHEABLE;
	cfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
	cfg.is_gpu_seg = type == SPDK_NVME_URMA_MEM_HOST ? 0 : 1;

	if (region->provider != NULL && region->provider->export_dmabuf != NULL) {
		uint64_t offset = 0;
		rc = region->provider->export_dmabuf(region->provider->provider_ctx,
					     region->pin_handle, &region->dmabuf_fd, &offset);
		if (rc == 0) {
			region->target_seg = urma_register_seg_dmabuf(urma_context, &cfg,
								       region->dmabuf_fd, offset);
			if (region->target_seg != NULL) {
				SPDK_URMA_STAT_INC(dmabuf_registrations);
			}
		}
	}
	/* The current UMDK gds branch uses is_gpu_seg for peer-memory pinning.
	 * dma-buf registration may return ENOTSUP until the kernel provider lands. */
	if (region->target_seg == NULL) {
		/* Modified By Yida (v3): UMMU Table mode 要求注册区间从 4K 对齐的
		 * base 起始并覆盖完整页。应用 buffer 可能位于子页偏移（Mooncake
		 * ClientBufferAllocator 仅保证 64B 对齐，torch tensor 是 caching
		 * allocator 大块显存的子区间），因此对常规/peer-memory 注册路径把
		 * base 向下对齐到 4K、grant 长度向上扩展覆盖 (offset + length) 的
		 * 完整页区间。wire capsule 仍携带原始 I/O 地址（nvme_urma.c 的
		 * capsule.data.address），远端访问的 [addr, addr+length) 完整落在
		 * 注册区间内即可。
		 * 注意：dma-buf 路径不走此对齐——cfg.va 与 export_dmabuf 返回的
		 * offset 一一对应，移位 base 会错位映射，故保持 v2 行为（精确 va）。 */
		uintptr_t base = SPDK_ALIGN_FLOOR((uintptr_t)addr, (uintptr_t)4096);
		cfg.va = (uint64_t)base;
		cfg.len = SPDK_ALIGN_CEIL(((uintptr_t)addr - base) + length,
					  (size_t)4096);
		region->target_seg = urma_register_seg(urma_context, &cfg);
		if (region->target_seg != NULL && type != SPDK_NVME_URMA_MEM_HOST) {
			SPDK_URMA_STAT_INC(peer_memory_registrations);
		}
	}
	if (region->target_seg == NULL) {
		SPDK_URMA_STAT_INC(registration_failures);
		rc = -errno;
		if (rc == 0) {
			rc = -EIO;
		}
		goto fail_pin;
	}
	*region_out = region;
	return 0;

fail_pin:
	if (region->provider != NULL && region->pin_handle != NULL) {
		region->provider->unpin(region->provider->provider_ctx, region->pin_handle);
	}
	/* Modified by Yin: 失败路径补 close 释放 dma-buf fd，防泄漏 */
	if (region->dmabuf_fd >= 0) {
		close(region->dmabuf_fd);
		region->dmabuf_fd = -1;
	}
fail_provider:
	if (region->provider != NULL) {
		pthread_mutex_lock(&g_provider_mutex);
		g_provider_refs[type]--;
		pthread_mutex_unlock(&g_provider_mutex);
	}
	free(region);
	return rc;
}

void
spdk_nvme_urma_unregister_memory(struct spdk_nvme_urma_memory_region *region)
{
	if (region == NULL) {
		return;
	}
	/* Modified By Yida(v7): 整池 region 注销时同步移出注册表，防悬垂采纳 */
	nvme_urma_region_registry_remove(region);
	if (region->target_seg != NULL) {
		urma_unregister_seg(region->target_seg);
	}
	/* Modified by Yin: 注销路径补 close 释放 dma-buf fd，防每 I/O 泄漏 */
	if (region->dmabuf_fd >= 0) {
		close(region->dmabuf_fd);
		region->dmabuf_fd = -1;
	}
	if (region->provider != NULL) {
		region->provider->unpin(region->provider->provider_ctx, region->pin_handle);
		pthread_mutex_lock(&g_provider_mutex);
		g_provider_refs[region->type]--;
		pthread_mutex_unlock(&g_provider_mutex);
	}
	free(region);
}

void *
spdk_nvme_urma_memory_region_get_addr(const struct spdk_nvme_urma_memory_region *region)
{
	return region == NULL ? NULL : region->addr;
}

size_t
spdk_nvme_urma_memory_region_get_length(const struct spdk_nvme_urma_memory_region *region)
{
	return region == NULL ? 0 : region->length;
}

urma_target_seg_t *
spdk_urma_memory_region_get_tseg(struct spdk_nvme_urma_memory_region *region)
{
	return region == NULL ? NULL : region->target_seg;
}

int
spdk_nvme_urma_memory_region_export(const struct spdk_nvme_urma_memory_region *region,
				    void *buf, size_t *length)
{
	if (region == NULL || length == NULL) {
		return -EINVAL;
	}
	if (buf == NULL || *length < sizeof(region->target_seg->seg)) {
		*length = sizeof(region->target_seg->seg);
		return buf == NULL ? 0 : -ENOSPC;
	}
	memcpy(buf, &region->target_seg->seg, sizeof(region->target_seg->seg));
	*length = sizeof(region->target_seg->seg);
	return 0;
}

void
spdk_nvme_urma_get_memory_stats(struct spdk_nvme_urma_memory_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	stats->host_registrations = __atomic_load_n(&g_memory_stats.host_registrations,
						     __ATOMIC_RELAXED);
	stats->accelerator_registrations = __atomic_load_n(&g_memory_stats.accelerator_registrations,
							    __ATOMIC_RELAXED);
	stats->dmabuf_registrations = __atomic_load_n(&g_memory_stats.dmabuf_registrations,
						       __ATOMIC_RELAXED);
	stats->peer_memory_registrations = __atomic_load_n(&g_memory_stats.peer_memory_registrations,
							     __ATOMIC_RELAXED);
	stats->registration_failures = __atomic_load_n(&g_memory_stats.registration_failures,
							__ATOMIC_RELAXED);
}

void
spdk_nvme_urma_reset_memory_stats(void)
{
	__atomic_store_n(&g_memory_stats.host_registrations, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_memory_stats.accelerator_registrations, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_memory_stats.dmabuf_registrations, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_memory_stats.peer_memory_registrations, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_memory_stats.registration_failures, 0, __ATOMIC_RELAXED);
}

static bool
spdk_urma_device_opts_compatible(const struct spdk_urma_device *device,
				 const struct spdk_urma_transport_opts *opts)
{
	return (opts->dev_name[0] == '\0' || strcmp(device->dev_name, opts->dev_name) == 0) &&
	       device->opts.eid_index == opts->eid_index &&
	       device->opts.active_port == opts->active_port &&
	       device->opts.transport_mode == opts->transport_mode &&
	       device->opts.send_jfc_count == opts->send_jfc_count &&
	       device->opts.recv_jfc_count == opts->recv_jfc_count &&
	       device->opts.jfc_depth == opts->jfc_depth &&
	       device->opts.num_jetty_per_ep == opts->num_jetty_per_ep &&
	       device->opts.jetty_depth == opts->jetty_depth &&
	       device->opts.capsule_transport == opts->capsule_transport &&
	       device->opts.bonding_balance == opts->bonding_balance &&
	       device->opts.bonding_multipath == opts->bonding_multipath &&
	       device->opts.numa_affinity == opts->numa_affinity &&
	       device->opts.priority == opts->priority &&
	       device->opts.tp_type == opts->tp_type;
}

static int
spdk_urma_select_priority(const urma_device_attr_t *attr, urma_tp_type_t tp_type,
			  int32_t configured)
{
	union urma_tp_type_en wanted = {};

	if (tp_type == URMA_CTP) {
		wanted.bs.ctp = 1;
	} else if (tp_type == URMA_RTP) {
		wanted.bs.rtp = 1;
	} else {
		return -EINVAL;
	}
	if (configured >= 0) {
		if (configured > URMA_MAX_PRIORITY ||
		    attr->dev_cap.priority_info[configured].tp_type.value != wanted.value) {
			SPDK_ERRLOG("URMA priority %d does not provide requested %s path\n",
				    configured, tp_type == URMA_CTP ? "CTP" : "RTP");
			return -EINVAL;
		}
		return configured;
	}
	for (int i = 0; i <= URMA_MAX_PRIORITY; i++) {
		if (attr->dev_cap.priority_info[i].tp_type.value == wanted.value) {
			return i;
		}
	}
	SPDK_ERRLOG("No URMA priority provides requested %s path\n",
		    tp_type == URMA_CTP ? "CTP" : "RTP");
	return -ENOTSUP;
}

uint32_t
spdk_urma_device_next_send_jfc(struct spdk_urma_device *device)
{
	return __atomic_fetch_add(&device->next_send_jfc, 1, __ATOMIC_RELAXED) %
	       device->send_jfc_count;
}

uint32_t
spdk_urma_device_next_jfr(struct spdk_urma_device *device)
{
	return __atomic_fetch_add(&device->next_jfr, 1, __ATOMIC_RELAXED) % device->jfr_count;
}

int
spdk_urma_device_poll_send_jfc(struct spdk_urma_device *device, uint32_t index,
			       int max_cr, urma_cr_t *cr)
{
	int rc;

	if (index >= device->send_jfc_count ||
	    pthread_mutex_trylock(&device->send_jfc_poll_locks[index]) != 0) {
		return 0;
	}
	rc = urma_poll_jfc(device->send_jfcs[index], max_cr, cr);
	pthread_mutex_unlock(&device->send_jfc_poll_locks[index]);
	return rc;
}

int
spdk_urma_device_poll_recv_jfc(struct spdk_urma_device *device, uint32_t index,
			       int max_cr, urma_cr_t *cr)
{
	int rc;

	if (index >= device->recv_jfc_count ||
	    pthread_mutex_trylock(&device->recv_jfc_poll_locks[index]) != 0) {
		return 0;
	}
	rc = urma_poll_jfc(device->recv_jfcs[index], max_cr, cr);
	pthread_mutex_unlock(&device->recv_jfc_poll_locks[index]);
	return rc;
}

static int
spdk_urma_compare_int32(const void *lhs, const void *rhs)
{
	const int32_t a = *(const int32_t *)lhs;
	const int32_t b = *(const int32_t *)rhs;

	return (a > b) - (a < b);
}

static int32_t
spdk_urma_read_numa_package_id(int32_t numa_id)
{
	char path[PATH_MAX];
	FILE *file;
	int32_t cpu_id, package_id;

	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", numa_id);
	file = fopen(path, "r");
	if (file == NULL) {
		return -1;
	}
	if (fscanf(file, "%" SCNd32, &cpu_id) != 1 || cpu_id < 0) {
		fclose(file);
		return -1;
	}
	fclose(file);

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu_id);
	file = fopen(path, "r");
	if (file == NULL) {
		return -1;
	}
	if (fscanf(file, "%" SCNd32, &package_id) != 1 || package_id < 0) {
		fclose(file);
		return -1;
	}
	fclose(file);
	return package_id;
}

static void
spdk_urma_numa_chip_map_init(void)
{
	int32_t *package_ids = NULL, *packages = NULL;
	int32_t first = spdk_env_get_first_numa_id();
	int32_t last = spdk_env_get_last_numa_id();
	int32_t numa_id;
	size_t node_count = 0, package_count = 0;
	bool have_sysfs_mapping = false;

	if (first < 0 || last < first || last == INT32_MAX) {
		return;
	}
	g_numa_chip_id_count = (size_t)last + 1;
	g_numa_chip_ids = malloc(g_numa_chip_id_count * sizeof(*g_numa_chip_ids));
	package_ids = malloc(g_numa_chip_id_count * sizeof(*package_ids));
	packages = malloc(g_numa_chip_id_count * sizeof(*packages));
	if (g_numa_chip_ids == NULL || package_ids == NULL || packages == NULL) {
		free(g_numa_chip_ids);
		g_numa_chip_ids = NULL;
		g_numa_chip_id_count = 0;
		goto out;
	}
	memset(g_numa_chip_ids, SPDK_URMA_INVALID_CHIP_ID,
	       g_numa_chip_id_count * sizeof(*g_numa_chip_ids));
	for (size_t i = 0; i < g_numa_chip_id_count; i++) {
		package_ids[i] = -1;
	}
	SPDK_ENV_FOREACH_NUMA_ID(numa_id) {
		int32_t package_id;
		bool seen = false;

		if (numa_id < 0 || (size_t)numa_id >= g_numa_chip_id_count) {
			continue;
		}
		node_count++;
		package_id = spdk_urma_read_numa_package_id(numa_id);
		if (package_id < 0) {
			continue;
		}
		package_ids[numa_id] = package_id;
		have_sysfs_mapping = true;
		for (size_t i = 0; i < package_count; i++) {
			if (packages[i] == package_id) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			packages[package_count++] = package_id;
		}
	}
	if (have_sysfs_mapping) {
		qsort(packages, package_count, sizeof(*packages), spdk_urma_compare_int32);
		SPDK_ENV_FOREACH_NUMA_ID(numa_id) {
			if (numa_id < 0 || (size_t)numa_id >= g_numa_chip_id_count ||
			    package_ids[numa_id] < 0) {
				continue;
			}
			for (size_t i = 0; i < package_count && i < UINT8_MAX - 1; i++) {
				if (packages[i] == package_ids[numa_id]) {
					g_numa_chip_ids[numa_id] = (uint8_t)i + 1;
					SPDK_NOTICELOG("URMA NUMA affinity: node %d package %d -> chip %u\n",
						       numa_id, package_ids[numa_id],
						       (unsigned)g_numa_chip_ids[numa_id]);
					break;
				}
			}
		}
	} else if (node_count != 0) {
		/* Restricted containers may hide CPU topology. Preserve Mooncake's
		 * fallback by assigning the lower half of NUMA nodes to chip 1. */
		size_t ordinal = 0;
		size_t first_half = (node_count + 1) / 2;

		SPDK_WARNLOG("URMA NUMA affinity: physical package topology unavailable; "
			     "using node-order fallback\n");
		SPDK_ENV_FOREACH_NUMA_ID(numa_id) {
			if (numa_id >= 0 && (size_t)numa_id < g_numa_chip_id_count) {
				g_numa_chip_ids[numa_id] = ordinal++ < first_half ? 1 : 2;
			}
		}
	}
out:
	free(packages);
	free(package_ids);
}

uint8_t
spdk_urma_memory_chip_id(const void *addr)
{
	int32_t numa_id = spdk_mem_get_numa_id(addr, NULL);

	if (numa_id < 0) {
		return SPDK_URMA_INVALID_CHIP_ID;
	}
	pthread_once(&g_numa_chip_once, spdk_urma_numa_chip_map_init);
	if (g_numa_chip_ids == NULL || (size_t)numa_id >= g_numa_chip_id_count) {
		return SPDK_URMA_INVALID_CHIP_ID;
	}
	return g_numa_chip_ids[numa_id];
}

int
spdk_urma_device_open(const struct spdk_urma_transport_opts *opts,
			  struct spdk_urma_device **device_out)
{
	struct spdk_urma_device *device, *existing;
	urma_device_t **devices = NULL;
	urma_device_t *selected = NULL;
	urma_eid_info_t *eids = NULL;
	uint32_t eid_count = 0;
	uint32_t max_jfc;
	bool eid_found = false;
	int count = 0, rc = -ENODEV;

	if (opts == NULL || device_out == NULL) {
		return -EINVAL;
	}
	if (opts->send_jfc_count == 0 || opts->recv_jfc_count == 0 ||
	    opts->jfc_depth == 0 || opts->jetty_depth == 0 || opts->max_io_size == 0 ||
	    opts->num_jetty_per_ep == 0 ||
	    opts->num_jetty_per_ep > SPDK_URMA_MAX_JETTY_PER_EP ||
	    opts->priority < -1 || opts->priority > URMA_MAX_PRIORITY ||
	    (opts->tp_type != URMA_CTP && opts->tp_type != URMA_RTP) ||
	    opts->capsule_transport > SPDK_URMA_CAPSULE_TRANSPORT_SEND_RECV) {
		SPDK_ERRLOG("Invalid URMA transport options\n");
		return -EINVAL;
	}
	pthread_mutex_lock(&g_device_mutex);
	TAILQ_FOREACH(device, &g_devices, link) {
		if (spdk_urma_device_opts_compatible(device, opts)) {
			device->refs++;
			*device_out = device;
			pthread_mutex_unlock(&g_device_mutex);
			return 0;
		}
		if (opts->dev_name[0] == '\0' || strcmp(device->dev_name, opts->dev_name) == 0) {
			SPDK_ERRLOG("URMA NIC '%s' already has a context with incompatible options\n",
				    device->dev_name);
			pthread_mutex_unlock(&g_device_mutex);
			return -EINVAL;
		}
	}
	pthread_mutex_unlock(&g_device_mutex);
	if (spdk_urma_runtime_get() != 0) {
		SPDK_ERRLOG("urma device open '%s': runtime init failed\n", opts->dev_name);
		return -EIO;
	}
	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		rc = -ENOMEM;
		goto fail_runtime;
	}
	device->opts = *opts;
	device->refs = 1;
	devices = urma_get_device_list(&count);
	for (int i = 0; devices != NULL && i < count; i++) {
		if (opts->dev_name[0] == '\0' || strcmp(opts->dev_name, devices[i]->name) == 0) {
			selected = devices[i];
			break;
		}
	}
	if (selected == NULL) {
		SPDK_ERRLOG("urma device open: device '%s' not found in device list\n",
			    opts->dev_name);
		goto fail;
	}
	snprintf(device->dev_name, sizeof(device->dev_name), "%s", selected->name);
	eids = urma_get_eid_list(selected, &eid_count);
	if (eids == NULL || eid_count == 0) {
		SPDK_ERRLOG("urma device open '%s': no eid list\n", selected->name);
		goto fail;
	}
	for (uint32_t i = 0; i < eid_count; i++) {
		if (eids[i].eid_index == opts->eid_index) {
			device->eid = eids[i].eid;
			device->eid_index = eids[i].eid_index;
			eid_found = true;
			break;
		}
	}
	if (!eid_found) {
		device->eid = eids[0].eid;
		device->eid_index = eids[0].eid_index;
	}
	device->context = urma_create_context(selected, device->eid_index);
	if (device->context == NULL || urma_query_device(selected, &device->attr) != URMA_SUCCESS) {
		/* Modified By Yida(v6): 原本静默返回——失败后只剩 transport.c 一行兜底，
		 * 卡 ~5s 才报 = 驱动层超时，这里标出设备名与 eid 便于定位 */
		SPDK_ERRLOG("urma device open '%s' eid%u: create_context/query failed (~5s delay means driver timeout)\n",
			    selected->name, (unsigned)device->eid_index);
		rc = -EIO;
		goto fail;
	}
	rc = spdk_urma_select_priority(&device->attr, opts->tp_type, opts->priority);
	if (rc < 0) {
		goto fail;
	}
	device->priority = (uint32_t)rc;
	SPDK_NOTICELOG("URMA device %s selected priority %u for %s\n", selected->name,
		       device->priority, opts->tp_type == URMA_CTP ? "CTP" : "RTP");
	/* Modified by Yin: 仅 BALANCE/MULTIPATH 才调 SET_BONDING_MODE，STANDALONE 不调（防 jetty 野指针崩溃） */
	if (opts->bonding_balance || opts->bonding_multipath) {
		bondp_set_bonding_mode_in_t mode = {
			/* Modified by Yin: bonding_mode 直接固定为 BALANCE（与上面守卫一致） */
			.bonding_mode = BONDP_BONDING_MODE_BALANCE,
			.bonding_level = opts->bonding_multipath ?
					BONDP_BONDING_LEVEL_IODIE : BONDP_BONDING_LEVEL_PORT,
		};
		urma_user_ctl_in_t in = {
			.addr = (uint64_t)&mode,
			.len = sizeof(mode),
			.opcode = BONDP_USER_CTL_SET_BONDING_MODE,
		};
		urma_user_ctl_out_t out = {};
		if (urma_user_ctl(device->context, &in, &out) != URMA_SUCCESS) {
			SPDK_ERRLOG("urma device open '%s': set bonding mode failed\n",
				    selected->name);
			rc = -EIO;
			goto fail;
		}
	}
	if (opts->active_port >= 0 && opts->active_port < MAX_PORT_CNT) {
		device->active_port = opts->active_port;
	} else {
		bool found = false;
		for (uint32_t i = 0; i < MAX_PORT_CNT; i++) {
			if (device->attr.port_attr[i].state == URMA_PORT_ACTIVE ||
			    device->attr.port_attr[i].state == URMA_PORT_ACTIVE_DEFER) {
				device->active_port = i;
				found = true;
				break;
			}
		}
		if (!found && device->attr.port_cnt != 0) {
			SPDK_ERRLOG("urma device open '%s': no active port\n", selected->name);
			rc = -ENETDOWN;
			goto fail;
		}
	}
	max_jfc = device->attr.dev_cap.max_jfc;
	if (max_jfc < 2) {
		SPDK_ERRLOG("urma device open '%s': at least two JFCs are required\n",
			    selected->name);
		rc = -ENOTSUP;
		goto fail;
	}
	device->send_jfc_count = spdk_min(opts->send_jfc_count, max_jfc - 1);
	if (device->send_jfc_count == 0) {
		device->send_jfc_count = 1;
	}
	device->recv_jfc_count = spdk_min(opts->recv_jfc_count,
					  max_jfc - device->send_jfc_count);
	if (device->recv_jfc_count == 0) {
		device->recv_jfc_count = 1;
	}
	device->send_jfcs = calloc(device->send_jfc_count, sizeof(*device->send_jfcs));
	device->recv_jfcs = calloc(device->recv_jfc_count, sizeof(*device->recv_jfcs));
	device->jfrs = calloc(device->recv_jfc_count, sizeof(*device->jfrs));
	device->send_jfc_poll_locks = calloc(device->send_jfc_count,
						 sizeof(*device->send_jfc_poll_locks));
	device->recv_jfc_poll_locks = calloc(device->recv_jfc_count,
						 sizeof(*device->recv_jfc_poll_locks));
	if (device->send_jfcs == NULL || device->recv_jfcs == NULL || device->jfrs == NULL ||
	    device->send_jfc_poll_locks == NULL || device->recv_jfc_poll_locks == NULL) {
		SPDK_ERRLOG("urma device open '%s': allocate shared JFC/JFR tables failed\n",
			    selected->name);
		rc = -ENOMEM;
		goto fail;
	}
	for (uint32_t i = 0; i < device->send_jfc_count; i++) {
		urma_jfc_cfg_t cfg = {};

		cfg.depth = spdk_min(opts->jfc_depth,
				     (uint32_t)device->attr.dev_cap.max_jfc_depth);
		if (pthread_mutex_init(&device->send_jfc_poll_locks[i], NULL) != 0) {
			SPDK_ERRLOG("urma device open '%s': initialize send JFC lock %u failed\n",
				    selected->name, i);
			rc = -EIO;
			goto fail;
		}
		device->send_jfc_poll_lock_count++;
		device->send_jfcs[i] = urma_create_jfc(device->context, &cfg);
		if (device->send_jfcs[i] == NULL) {
			SPDK_ERRLOG("urma device open '%s': create send jfc %u (depth %u) failed\n",
				    selected->name, i, cfg.depth);
			rc = -EIO;
			goto fail;
		}
	}
	device->jfr_count = device->recv_jfc_count;
	for (uint32_t i = 0; i < device->recv_jfc_count; i++) {
		urma_jfc_cfg_t jfc_cfg = {};
		urma_jfr_cfg_t jfr_cfg = {};
		uint32_t max_jfr_depth = device->attr.dev_cap.max_jfr_depth;
		uint8_t max_jfr_sge = device->attr.dev_cap.max_jfr_sge;

		jfc_cfg.depth = spdk_min(opts->jfc_depth,
					 (uint32_t)device->attr.dev_cap.max_jfc_depth);
		if (pthread_mutex_init(&device->recv_jfc_poll_locks[i], NULL) != 0) {
			SPDK_ERRLOG("urma device open '%s': initialize recv JFC lock %u failed\n",
				    selected->name, i);
			rc = -EIO;
			goto fail;
		}
		device->recv_jfc_poll_lock_count++;
		device->recv_jfcs[i] = urma_create_jfc(device->context, &jfc_cfg);
		if (device->recv_jfcs[i] == NULL) {
			SPDK_ERRLOG("urma device open '%s': create recv jfc %u failed\n",
				    selected->name, i);
			rc = -EIO;
			goto fail;
		}
		if (max_jfr_depth == 0) {
			max_jfr_depth = device->attr.dev_cap.max_jfs_depth ?
					 device->attr.dev_cap.max_jfs_depth : opts->jetty_depth;
		}
		if (max_jfr_sge == 0) {
			max_jfr_sge = SPDK_URMA_DEFAULT_MAX_SGE;
		}
		jfr_cfg.depth = spdk_min(opts->jetty_depth, max_jfr_depth);
		if (jfr_cfg.depth == 0) {
			jfr_cfg.depth = opts->jetty_depth;
		}
		jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
		jfr_cfg.trans_mode = opts->transport_mode;
		jfr_cfg.max_sge = spdk_min(SPDK_URMA_DEFAULT_MAX_SGE, max_jfr_sge);
		jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
		jfr_cfg.token_value.token = SPDK_URMA_DEFAULT_TOKEN;
		jfr_cfg.jfc = device->recv_jfcs[i];
		device->jfrs[i] = urma_create_jfr(device->context, &jfr_cfg);
		if (device->jfrs[i] == NULL) {
			SPDK_ERRLOG("urma device open '%s': create shared jfr %u (depth %u) failed\n",
				    selected->name, i, jfr_cfg.depth);
			rc = -EIO;
			goto fail;
		}
	}
	{
		struct spdk_memory_domain_ctx domain_ctx = {
			.size = sizeof(domain_ctx),
			/* Modified by Yin: user_ctx 改指向指针本身，user_ctx_size 才能正确复制 */
			.user_ctx = &device,
			.user_ctx_size = sizeof(device),
		};
		char id[URMA_MAX_NAME + 32];
		snprintf(id, sizeof(id), "SPDK_URMA_DMA_DEVICE:%s", selected->name);
		if (spdk_memory_domain_create(&device->memory_domain,
				SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START, &domain_ctx, id) != 0) {
			SPDK_ERRLOG("urma device open '%s': memory domain create failed\n",
				    selected->name);
			rc = -ENOMEM;
			goto fail;
		}
	}
	if (eids != NULL) {
		urma_free_eid_list(eids);
	}
	urma_free_device_list(devices);
	pthread_mutex_lock(&g_device_mutex);
	TAILQ_FOREACH(existing, &g_devices, link) {
		if (spdk_urma_device_opts_compatible(existing, opts)) {
			existing->refs++;
			pthread_mutex_unlock(&g_device_mutex);
			spdk_urma_device_close(device);
			*device_out = existing;
			return 0;
		}
		if (strcmp(existing->dev_name, device->dev_name) == 0) {
			pthread_mutex_unlock(&g_device_mutex);
			SPDK_ERRLOG("URMA NIC '%s' raced with incompatible context creation\n",
				    device->dev_name);
			spdk_urma_device_close(device);
			return -EINVAL;
		}
	}
	TAILQ_INSERT_TAIL(&g_devices, device, link);
	pthread_mutex_unlock(&g_device_mutex);
	*device_out = device;
	return 0;

fail:
	if (eids != NULL) {
		urma_free_eid_list(eids);
	}
	if (devices != NULL) {
		urma_free_device_list(devices);
	}
	spdk_urma_device_close(device);
	return rc;
fail_runtime:
	spdk_urma_runtime_put();
	return rc;
}

void
spdk_urma_device_get(struct spdk_urma_device *device)
{
	if (device == NULL) {
		return;
	}
	pthread_mutex_lock(&g_device_mutex);
	device->refs++;
	pthread_mutex_unlock(&g_device_mutex);
}

void
spdk_urma_device_close(struct spdk_urma_device *device)
{
	struct spdk_urma_device *it;
	bool registered = false;

	if (device == NULL) {
		return;
	}
	pthread_mutex_lock(&g_device_mutex);
	TAILQ_FOREACH(it, &g_devices, link) {
		if (it == device) {
			registered = true;
			break;
		}
	}
	if (registered) {
		assert(device->refs > 0);
		if (--device->refs != 0) {
			pthread_mutex_unlock(&g_device_mutex);
			return;
		}
		TAILQ_REMOVE(&g_devices, device, link);
	}
	pthread_mutex_unlock(&g_device_mutex);

	for (uint32_t i = 0; i < device->jfr_count; i++) {
		if (device->jfrs != NULL && device->jfrs[i] != NULL) {
			urma_delete_jfr(device->jfrs[i]);
		}
	}
	for (uint32_t i = 0; i < device->recv_jfc_count; i++) {
		if (device->recv_jfcs != NULL && device->recv_jfcs[i] != NULL) {
			urma_delete_jfc(device->recv_jfcs[i]);
		}
	}
	for (uint32_t i = 0; i < device->send_jfc_count; i++) {
		if (device->send_jfcs != NULL && device->send_jfcs[i] != NULL) {
			urma_delete_jfc(device->send_jfcs[i]);
		}
	}
	for (uint32_t i = 0; i < device->recv_jfc_poll_lock_count; i++) {
		pthread_mutex_destroy(&device->recv_jfc_poll_locks[i]);
	}
	for (uint32_t i = 0; i < device->send_jfc_poll_lock_count; i++) {
		pthread_mutex_destroy(&device->send_jfc_poll_locks[i]);
	}
	free(device->jfrs);
	free(device->recv_jfcs);
	free(device->send_jfcs);
	free(device->send_jfc_poll_locks);
	free(device->recv_jfc_poll_locks);
	if (device->memory_domain != NULL) {
		spdk_memory_domain_destroy(device->memory_domain);
	}
	if (device->context != NULL) {
		urma_delete_context(device->context);
	}
	free(device);
	spdk_urma_runtime_put();
}
