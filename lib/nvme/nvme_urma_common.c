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

int
spdk_urma_device_open(const struct spdk_urma_transport_opts *opts,
			  struct spdk_urma_device **device_out)
{
	struct spdk_urma_device *device;
	urma_device_t **devices = NULL;
	urma_device_t *selected = NULL;
	urma_eid_info_t *eids = NULL;
	uint32_t eid_count = 0;
	bool eid_found = false;
	int count = 0, rc = -ENODEV;

	if (opts == NULL || device_out == NULL) {
		return -EINVAL;
	}
	if (spdk_urma_runtime_get() != 0) {
		return -EIO;
	}
	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		rc = -ENOMEM;
		goto fail_runtime;
	}
	device->opts = *opts;
	devices = urma_get_device_list(&count);
	for (int i = 0; devices != NULL && i < count; i++) {
		if (opts->dev_name[0] == '\0' || strcmp(opts->dev_name, devices[i]->name) == 0) {
			selected = devices[i];
			break;
		}
	}
	if (selected == NULL) {
		goto fail;
	}
	eids = urma_get_eid_list(selected, &eid_count);
	if (eids == NULL || eid_count == 0) {
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
		rc = -EIO;
		goto fail;
	}
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
			rc = -ENETDOWN;
			goto fail;
		}
	}
	device->jfc_count = spdk_min(opts->jfc_count,
				     (uint32_t)device->attr.dev_cap.max_jfc);
	if (device->jfc_count == 0) {
		device->jfc_count = 1;
	}
	device->jfcs = calloc(device->jfc_count, sizeof(*device->jfcs));
	if (device->jfcs == NULL) {
		rc = -ENOMEM;
		goto fail;
	}
	for (uint32_t i = 0; i < device->jfc_count; i++) {
		urma_jfc_cfg_t cfg = {};
		cfg.depth = spdk_min(opts->jfc_depth,
				     (uint32_t)device->attr.dev_cap.max_jfc_depth);
		device->jfcs[i] = urma_create_jfc(device->context, &cfg);
		if (device->jfcs[i] == NULL) {
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
			rc = -ENOMEM;
			goto fail;
		}
	}
	if (eids != NULL) {
		urma_free_eid_list(eids);
	}
	urma_free_device_list(devices);
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
	if (device->context != NULL) {
		urma_delete_context(device->context);
	}
	free(device);
	spdk_urma_runtime_put();
}
