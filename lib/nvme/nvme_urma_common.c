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

uint32_t
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
	opts->jfc_count = spdk_urma_env_u32("SPDK_URMA_JFC_COUNT", SPDK_URMA_DEFAULT_JFC_COUNT);
	opts->jfc_depth = spdk_urma_env_u32("SPDK_URMA_JFC_DEPTH", SPDK_URMA_DEFAULT_JFC_DEPTH);
	opts->jetty_count = spdk_urma_env_u32("SPDK_URMA_JETTY_COUNT",
			    SPDK_URMA_DEFAULT_JETTY_COUNT);
	opts->jetty_depth = spdk_urma_env_u32("SPDK_URMA_JETTY_DEPTH",
			    SPDK_URMA_DEFAULT_JETTY_DEPTH);
	opts->max_io_size = spdk_urma_env_u32("SPDK_URMA_MAX_IO_SIZE", 131072);
	opts->bonding_balance = spdk_urma_env_bool("SPDK_URMA_BONDING_BALANCE",
				"MC_URMA_BONDING_BALANCE", false);
	opts->bonding_multipath = spdk_urma_env_bool("SPDK_URMA_BONDING_MULTIPATH_ENABLE",
				  "MC_URMA_BONDING_MULTIPATH_ENABLE", false);

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

/* Modified By Yida(v7): 整池注册表 —— 仅由应用侧 spdk_nvme_urma_register_memory_for_qpair
 * （预注册整块连续缓冲）填充；I/O 提交路径用 find() 采纳覆盖本 I/O 缓冲的 region，
 * 跳过 per-I/O register，capsule 携带全区 seg，对端可整池 import 一次。
 * 按 urma_context 键控：共享同一 context 的 qpair 可以复用该 context 已注册的 region。 */
#define NVME_URMA_REGION_REGISTRY_SIZE 64

struct nvme_urma_region_entry {
	void *context;
	uintptr_t start;
	uintptr_t end;   /* start + length */
	struct spdk_nvme_urma_memory_region *region;
	bool used;
};

static struct nvme_urma_region_entry g_region_registry[NVME_URMA_REGION_REGISTRY_SIZE];
static pthread_mutex_t g_region_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

void
nvme_urma_region_registry_add(void *urma_context, void *addr, size_t length,
			      struct spdk_nvme_urma_memory_region *region)
{
	pthread_mutex_lock(&g_region_registry_mutex);
	for (int i = 0; i < NVME_URMA_REGION_REGISTRY_SIZE; i++) {
		struct nvme_urma_region_entry *e = &g_region_registry[i];

		if (!e->used) {
			e->context = urma_context;
			e->start = (uintptr_t)addr;
			e->end = (uintptr_t)addr + length;
			e->region = region;
			e->used = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_region_registry_mutex);
}

void
nvme_urma_region_registry_remove(struct spdk_nvme_urma_memory_region *region)
{
	if (region == NULL) {
		return;
	}
	pthread_mutex_lock(&g_region_registry_mutex);
	for (int i = 0; i < NVME_URMA_REGION_REGISTRY_SIZE; i++) {
		struct nvme_urma_region_entry *e = &g_region_registry[i];

		if (e->used && e->region == region) {
			e->used = false;
			e->region = NULL;
		}
	}
	pthread_mutex_unlock(&g_region_registry_mutex);
}

struct spdk_nvme_urma_memory_region *
nvme_urma_region_registry_find(void *urma_context, uint64_t addr, size_t length)
{
	struct spdk_nvme_urma_memory_region *found = NULL;

	pthread_mutex_lock(&g_region_registry_mutex);
	for (int i = 0; i < NVME_URMA_REGION_REGISTRY_SIZE; i++) {
		struct nvme_urma_region_entry *e = &g_region_registry[i];

		if (e->used && e->context == urma_context &&
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

struct spdk_urma_shared_context {
	urma_context_t *context;
	urma_device_attr_t attr;
	urma_eid_t eid;
	uint32_t eid_index;
	uint8_t active_port;
	int32_t requested_active_port;
	bool bonding_balance;
	bool bonding_multipath;
	char dev_name[URMA_MAX_NAME];
	uint32_t refcnt;
	TAILQ_ENTRY(spdk_urma_shared_context) link;
};

static TAILQ_HEAD(, spdk_urma_shared_context) g_shared_contexts =
	TAILQ_HEAD_INITIALIZER(g_shared_contexts);
static pthread_mutex_t g_shared_context_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool
spdk_urma_shared_context_matches(const struct spdk_urma_shared_context *shared,
				 const struct spdk_urma_transport_opts *opts,
				 const char *dev_name, uint32_t eid_index)
{
	return strcmp(shared->dev_name, dev_name) == 0 && shared->eid_index == eid_index &&
	       shared->requested_active_port == opts->active_port &&
	       shared->bonding_balance == opts->bonding_balance &&
	       shared->bonding_multipath == opts->bonding_multipath;
}

static int
spdk_urma_shared_context_get(const struct spdk_urma_transport_opts *opts,
			     struct spdk_urma_shared_context **shared_out)
{
	struct spdk_urma_shared_context *shared;
	urma_device_t **devices = NULL;
	urma_device_t *selected = NULL;
	urma_eid_info_t *eids = NULL;
	urma_eid_t eid = {};
	uint32_t eid_index = 0;
	uint32_t eid_count = 0;
	bool eid_found = false;
	int count = 0;
	int rc = -ENODEV;

	devices = urma_get_device_list(&count);
	for (int i = 0; devices != NULL && i < count; i++) {
		if (opts->dev_name[0] == '\0' || strcmp(opts->dev_name, devices[i]->name) == 0) {
			selected = devices[i];
			break;
		}
	}
	if (selected == NULL) {
		SPDK_ERRLOG("urma context acquire: device '%s' not found in device list\n",
			    opts->dev_name);
		goto out;
	}

	eids = urma_get_eid_list(selected, &eid_count);
	if (eids == NULL || eid_count == 0) {
		SPDK_ERRLOG("urma context acquire '%s': no eid list\n", selected->name);
		goto out;
	}
	for (uint32_t i = 0; i < eid_count; i++) {
		if (eids[i].eid_index == opts->eid_index) {
			eid = eids[i].eid;
			eid_index = eids[i].eid_index;
			eid_found = true;
			break;
		}
	}
	if (!eid_found) {
		eid = eids[0].eid;
		eid_index = eids[0].eid_index;
	}

	pthread_mutex_lock(&g_shared_context_mutex);
	TAILQ_FOREACH(shared, &g_shared_contexts, link) {
		if (spdk_urma_shared_context_matches(shared, opts, selected->name, eid_index)) {
			shared->refcnt++;
			*shared_out = shared;
			rc = 0;
			goto out_unlock;
		}
	}

	shared = calloc(1, sizeof(*shared));
	if (shared == NULL) {
		rc = -ENOMEM;
		goto out_unlock;
	}
	shared->eid = eid;
	shared->eid_index = eid_index;
	shared->requested_active_port = opts->active_port;
	shared->bonding_balance = opts->bonding_balance;
	shared->bonding_multipath = opts->bonding_multipath;
	snprintf(shared->dev_name, sizeof(shared->dev_name), "%s", selected->name);

	shared->context = urma_create_context(selected, eid_index);
	if (shared->context == NULL || urma_query_device(selected, &shared->attr) != URMA_SUCCESS) {
		SPDK_ERRLOG("urma context acquire '%s' eid%u: create_context/query failed "
			    "(~5s delay means driver timeout)\n", selected->name, eid_index);
		rc = -EIO;
		goto out_free_shared;
	}

	if (opts->bonding_balance || opts->bonding_multipath) {
		bondp_set_bonding_mode_in_t mode = {
			.bonding_mode = BONDP_BONDING_MODE_BALANCE,
			.bonding_level = opts->bonding_multipath ?
					BONDP_BONDING_LEVEL_IODIE : BONDP_BONDING_LEVEL_PORT,
		};
		urma_user_ctl_in_t in = {
			.addr = (uint64_t)&mode,
			.len = sizeof(mode),
			.opcode = BONDP_USER_CTL_SET_BONDING_MODE,
		};
		urma_user_ctl_out_t out_ctl = {};

		if (urma_user_ctl(shared->context, &in, &out_ctl) != URMA_SUCCESS) {
			SPDK_ERRLOG("urma context acquire '%s': set bonding mode failed\n",
				    selected->name);
			rc = -EIO;
			goto out_free_shared;
		}
	}

	if (opts->active_port >= 0 && opts->active_port < MAX_PORT_CNT) {
		shared->active_port = opts->active_port;
	} else {
		bool found = false;

		for (uint32_t i = 0; i < MAX_PORT_CNT; i++) {
			if (shared->attr.port_attr[i].state == URMA_PORT_ACTIVE ||
			    shared->attr.port_attr[i].state == URMA_PORT_ACTIVE_DEFER) {
				shared->active_port = i;
				found = true;
				break;
			}
		}
		if (!found && shared->attr.port_cnt != 0) {
			SPDK_ERRLOG("urma context acquire '%s': no active port\n", selected->name);
			rc = -ENETDOWN;
			goto out_free_shared;
		}
	}

	shared->refcnt = 1;
	TAILQ_INSERT_TAIL(&g_shared_contexts, shared, link);
	*shared_out = shared;
	SPDK_NOTICELOG("URMA context %p created for %s eid%u\n", shared->context,
		       shared->dev_name, shared->eid_index);
	rc = 0;
	goto out_unlock;

out_free_shared:
	if (shared->context != NULL) {
		urma_delete_context(shared->context);
	}
	free(shared);
out_unlock:
	pthread_mutex_unlock(&g_shared_context_mutex);
out:
	if (eids != NULL) {
		urma_free_eid_list(eids);
	}
	if (devices != NULL) {
		urma_free_device_list(devices);
	}
	return rc;
}

static void
spdk_urma_shared_context_put(struct spdk_urma_shared_context *shared)
{
	bool destroy = false;

	if (shared == NULL) {
		return;
	}
	pthread_mutex_lock(&g_shared_context_mutex);
	assert(shared->refcnt > 0);
	if (--shared->refcnt == 0) {
		TAILQ_REMOVE(&g_shared_contexts, shared, link);
		destroy = true;
	}
	pthread_mutex_unlock(&g_shared_context_mutex);

	if (destroy) {
		SPDK_NOTICELOG("URMA context %p released for %s eid%u\n", shared->context,
			       shared->dev_name, shared->eid_index);
		urma_delete_context(shared->context);
		free(shared);
	}
}

int
spdk_urma_device_open(const struct spdk_urma_transport_opts *opts,
			  struct spdk_urma_device **device_out)
{
	struct spdk_urma_device *device;
	int rc = -ENODEV;

	if (opts == NULL || device_out == NULL) {
		return -EINVAL;
	}
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
	rc = spdk_urma_shared_context_get(opts, &device->shared_context);
	if (rc != 0) {
		goto fail;
	}
	device->context = device->shared_context->context;
	device->attr = device->shared_context->attr;
	device->eid = device->shared_context->eid;
	device->eid_index = device->shared_context->eid_index;
	device->active_port = device->shared_context->active_port;
	snprintf(device->dev_name, sizeof(device->dev_name), "%s",
		 device->shared_context->dev_name);
	device->jfc_count = spdk_min(opts->jfc_count,
				     (uint32_t)device->attr.dev_cap.max_jfc);
	if (device->jfc_count == 0) {
		device->jfc_count = 1;
	}
	device->jfcs = calloc(device->jfc_count, sizeof(*device->jfcs));
	if (device->jfcs == NULL) {
		SPDK_ERRLOG("urma device open '%s': alloc jfc table (n=%u) failed\n",
			    device->dev_name, device->jfc_count);
		rc = -ENOMEM;
		goto fail;
	}
	for (uint32_t i = 0; i < device->jfc_count; i++) {
		urma_jfc_cfg_t cfg = {};
		cfg.depth = spdk_min(opts->jfc_depth,
				     (uint32_t)device->attr.dev_cap.max_jfc_depth);
		device->jfcs[i] = urma_create_jfc(device->context, &cfg);
		if (device->jfcs[i] == NULL) {
			SPDK_ERRLOG("urma device open '%s': create jfc %u (depth %u) failed\n",
				    device->dev_name, i, cfg.depth);
			rc = -EIO;
			goto fail;
		}
	}
	/* Modified by Yin: UB transport 强制 share_jfr=1，预建共享 jfr 供所有 jetty 复用 */
	{
		urma_jfr_cfg_t jfr_cfg = {};
		uint32_t max_jfr_depth = device->attr.dev_cap.max_jfr_depth;
		uint8_t max_jfr_sge = device->attr.dev_cap.max_jfr_sge;
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
		jfr_cfg.jfc = device->jfcs[0];
		device->jfr = urma_create_jfr(device->context, &jfr_cfg);
		if (device->jfr == NULL) {
			SPDK_ERRLOG("urma device open '%s': create shared jfr (depth %u) failed\n",
				    device->dev_name, jfr_cfg.depth);
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
		snprintf(id, sizeof(id), "SPDK_URMA_DMA_DEVICE:%s", device->dev_name);
		if (spdk_memory_domain_create(&device->memory_domain,
				SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START, &domain_ctx, id) != 0) {
			SPDK_ERRLOG("urma device open '%s': memory domain create failed\n",
				    device->dev_name);
			rc = -ENOMEM;
			goto fail;
		}
	}
	*device_out = device;
	return 0;

fail:
	spdk_urma_device_close(device);
	return rc;
fail_runtime:
	spdk_urma_runtime_put();
	return rc;
}

void
spdk_urma_device_close(struct spdk_urma_device *device)
{
	if (device == NULL) {
		return;
	}
	/* Modified by Yin: 释放 1.5 预建的共享 jfr，与 open 对称 */
	if (device->jfr != NULL) {
		urma_delete_jfr(device->jfr);
		device->jfr = NULL;
	}
	for (uint32_t i = 0; i < device->jfc_count; i++) {
		if (device->jfcs != NULL && device->jfcs[i] != NULL) {
			urma_delete_jfc(device->jfcs[i]);
		}
	}
	free(device->jfcs);
	if (device->memory_domain != NULL) {
		spdk_memory_domain_destroy(device->memory_domain);
	}
	spdk_urma_shared_context_put(device->shared_context);
	device->context = NULL;
	free(device);
	spdk_urma_runtime_put();
}
