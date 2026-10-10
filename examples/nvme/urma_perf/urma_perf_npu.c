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
 * AscendCL exposes no public dmabuf export API, so direct registration goes
 * through the RA/HCCP path (libra.so RaCtxLmemRegister with nonPin=ENABLE):
 * the pinned HBM segment is remotely DMA-able via URMA jetty READ/WRITE with
 * no kernel gpu_p2p framework (P2-19 verdict, P2-21 init sequence). */

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
typedef acl_error_t (*aclrt_create_context_fn)(aclrt_context_t *context, int32_t device_id);
typedef acl_error_t (*aclrt_destroy_context_fn)(aclrt_context_t context);
typedef acl_error_t (*aclrt_set_current_context_fn)(aclrt_context_t context);
typedef acl_error_t (*aclrt_malloc_fn)(void **dev_ptr, size_t size, int policy);
typedef acl_error_t (*aclrt_free_fn)(void *dev_ptr);
typedef acl_error_t (*aclrt_memcpy_fn)(void *dst, size_t dest_max, const void *src,
					size_t count, int kind);

/* ===== RA/HCCP (libra.so) — third official HBM registration path =====
 *
 * Types mirror rdma_agent_plugin.h from the CCDK repo (colleague_c).
 * Registering HBM through the RA layer (instead of urma_register_seg with
 * is_gpu_seg) makes the segment remotely DMA-able via URMA jetty ops
 * without any kernel gpu_p2p framework (P2-19 verdict: bidirectional
 * byte-exact on 133). All libra symbols are dlopen/dlsym-bound; libra.so
 * ships inside CANN (${CANN}/aarch64-linux/lib64/libra.so). */
struct ra_init_config {
	uint32_t phy_id;
	uint32_t nic_position;
	int32_t hdc_type;
	bool enable_hdc_async;
};

struct ra_ctx_init_cfg {
	int32_t mode;
};

union ra_hccp_eid_u {
	uint8_t raw[16];
	struct {
		uint64_t reserved;
		uint32_t prefix;
		uint32_t addr;
	} in4;
	struct {
		uint64_t subnet_prefix;
		uint64_t interface_id;
	} in6;
};

/* Mirrors CtxInitAttr from rdma_agent_plugin.h: union HccpEid contains a
 * uint64_t so the struct is 8-byte aligned (eidIndex@8, eid@16, resv@32,
 * sizeof 96). A plain uint8_t eid[16] layout is 4-byte aligned (sizeof 88)
 * and RaCtxInit fails with 128003 "ra_get_init_ctx_handle failed" — the
 * P2-21 root cause. */
struct ra_ctx_init_attr {
	uint32_t phy_id;
	union {
		struct {
			uint32_t notify_type;
			int32_t family;
		} rdma;
		struct {
			uint32_t eid_index;
			union ra_hccp_eid_u eid;
		} ub;
	};
	uint32_t resv[16];
};

/* nds.c constants (P2-21 verified on 133). */
#define RA_HDC_TYPE 18
#define RA_NETWORK_OFFLINE 1
#define RA_INIT_REPEAT_RET_CODE 328002

struct ra_info {
	int32_t mode;
	uint32_t phy_id;
};

struct ra_dev_eid_info {
	char name[64];
	uint32_t type;
	uint32_t eid_index;
	union ra_hccp_eid_u eid;
	uint32_t die_id;
	uint32_t chip_id;
	uint32_t func_id;
	uint32_t resv;
};

struct rt_proc_ext_param {
	const char *param_info;
	uint64_t param_len;
};

struct rt_net_service_open_args {
	struct rt_proc_ext_param *ext_param_list;
	uint64_t ext_param_cnt;
};

struct ra_hccp_mem_info {
	uint64_t addr;
	uint64_t size;
};

union ra_reg_seg_flag {
	struct {
		uint32_t token_policy	: 3;
		uint32_t cacheable	: 1;
		uint32_t dsva		: 1;
		uint32_t access		: 6;
		uint32_t non_pin	: 1;
		uint32_t user_iova	: 1;
		uint32_t token_id_valid	: 1;
		uint32_t reserved	: 18;
	} bs;
	uint32_t value;
};

#define RA_MEM_KEY_SIZE 128

struct ra_mem_key {
	uint8_t value[RA_MEM_KEY_SIZE];
	uint8_t size;
};

struct ra_mem_reg_attr {
	struct ra_hccp_mem_info mem;
	struct {
		union ra_reg_seg_flag flags;
		uint32_t token_value;
		void *token_id_handle;
	} ub;
	uint32_t resv[8];
};

struct ra_mem_reg_info {
	struct ra_mem_key key;
	struct {
		uint32_t token_id;
		uint64_t target_seg_handle;
	} ub;
	uint32_t resv[8];
};

struct ra_lmem_reg_info {
	struct ra_mem_reg_attr in;
	struct ra_mem_reg_info out;
};

typedef int (*ra_init_fn)(struct ra_init_config *config);
typedef int (*ra_deinit_fn)(struct ra_init_config *config);
typedef int (*ra_ctx_init_fn)(struct ra_ctx_init_cfg *cfg,
			      struct ra_ctx_init_attr *attr, void **ctx_handle);
typedef int (*ra_ctx_lmem_register_fn)(void *ctx_handle,
		struct ra_lmem_reg_info *lmem_info, void **lmem_handle);
typedef int (*ra_ctx_lmem_unregister_fn)(void *ctx_handle, void *lmem_handle);
typedef int (*ra_ctx_deinit_fn)(void *ctx_handle);
typedef int (*ra_get_eid_num_fn)(struct ra_info info, unsigned int *num);
typedef int (*ra_get_eid_list_fn)(struct ra_info info,
				  struct ra_dev_eid_info *info_list,
				  unsigned int *num);
typedef int (*rt_open_net_service_fn)(struct rt_net_service_open_args *args);
typedef int (*aclrt_get_phy_dev_id_fn)(int32_t device_id, uint32_t *phy_id);

/* Segment descriptor exposed to the remote endpoint: the remote does
 * urma_import_seg(ubva{eid, uasid, va}) + urma_import_jetty(CTP) and its
 * jetty READ/WRITE ops reach this HBM segment directly (P2-19 flow). */
struct npu_segment_info {
	uint8_t eid[16];
	uint32_t uasid;
	uint64_t va;
	uint32_t token;
	uint32_t len;
};

struct npu_driver {
	void *library;
	void *ra_library;
	void *rt_library;
	void *rt_acl_library;
	void *ra_ctx_handle;
	struct ra_init_config ra_cfg;
	uint32_t ra_phy_id;
	ra_init_fn ra_init;
	ra_deinit_fn ra_deinit;
	ra_ctx_init_fn ra_ctx_init;
	ra_ctx_lmem_register_fn ra_lmem_register;
	ra_ctx_lmem_unregister_fn ra_lmem_unregister;
	ra_ctx_deinit_fn ra_ctx_deinit;
	ra_get_eid_num_fn ra_get_eid_num;
	ra_get_eid_list_fn ra_get_eid_list;
	rt_open_net_service_fn rt_open_net_service;
	aclrt_get_phy_dev_id_fn get_phy_dev_id;
	uint32_t ra_uasid;
	bool ra_uasid_valid;
	bool ra_ready;
	uint8_t ra_eid[16];
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
	/* RA/HCCP registration output (libra path); non-NULL when the RA
	 * context was ready at pin time. */
	void *lmem_handle;
	/* RaCtxLmemRegister output: token_id from lmem.out.ub — the peer
	 * needs it in the imported urma_seg_t (P2-23 B). */
	uint32_t token_id;
	bool token_id_valid;
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

/* RA/HCCP init sequence, mirrors colleague_c nds/src/nds.c nds_init
 * (P2-21 verified on 133): rtOpenNetService("--hdcType=18") →
 * aclrtGetPhyDevIdByLogicDevId → RaInit → RaGetDevEidInfoNum/List →
 * RaCtxInit. Returns 0 on success; on any failure the RA path is disabled
 * with a message and the provider keeps working for non-direct routes. */
static int
npu_ra_init(int32_t device_id)
{
	struct ra_ctx_init_cfg ra_ctx_cfg = {};
	struct ra_ctx_init_attr ra_attr = {};
	struct ra_info ra_info = {};
	struct ra_dev_eid_info *eid_list = NULL;
	unsigned int eid_num = 0;
	struct rt_net_service_open_args rt_args = {};
	struct rt_proc_ext_param rt_ext = {};
	int ret;

	ra_info.mode = RA_NETWORK_OFFLINE;

	g_npu.ra_library = dlopen("libra.so", RTLD_NOW | RTLD_GLOBAL);
	if (g_npu.ra_library == NULL) {
		fprintf(stderr, "RA libra.so unavailable (%s)\n", dlerror());
		return -1;
	}
	g_npu.ra_init = (ra_init_fn)dlsym(g_npu.ra_library, "RaInit");
	g_npu.ra_ctx_init = (ra_ctx_init_fn)dlsym(g_npu.ra_library, "RaCtxInit");
	g_npu.ra_lmem_register = (ra_ctx_lmem_register_fn)
				 dlsym(g_npu.ra_library, "RaCtxLmemRegister");
	g_npu.ra_lmem_unregister = (ra_ctx_lmem_unregister_fn)
				   dlsym(g_npu.ra_library, "RaCtxLmemUnregister");
	/* libra.so exports RaCtxDeinit (lowercase i, nm -D) — "RaCtxDeInit"
	 * never resolves. */
	g_npu.ra_ctx_deinit = (ra_ctx_deinit_fn)dlsym(g_npu.ra_library, "RaCtxDeinit");
	g_npu.ra_deinit = (ra_deinit_fn)dlsym(g_npu.ra_library, "RaDeinit");
	g_npu.ra_get_eid_num = (ra_get_eid_num_fn)
			       dlsym(g_npu.ra_library, "RaGetDevEidInfoNum");
	g_npu.ra_get_eid_list = (ra_get_eid_list_fn)
				dlsym(g_npu.ra_library, "RaGetDevEidInfoList");
	if (g_npu.ra_init == NULL || g_npu.ra_ctx_init == NULL ||
	    g_npu.ra_lmem_register == NULL || g_npu.ra_lmem_unregister == NULL ||
	    g_npu.ra_get_eid_num == NULL || g_npu.ra_get_eid_list == NULL) {
		fprintf(stderr, "RA libra symbols missing\n");
		return -1;
	}

	/* Step 1: rtOpenNetService (nds.c tsd_process_open, libruntime.so). */
	g_npu.rt_library = dlopen("libruntime.so", RTLD_NOW | RTLD_GLOBAL);
	if (g_npu.rt_library == NULL) {
		fprintf(stderr, "RA libruntime.so unavailable (%s)\n", dlerror());
		return -1;
	}
	g_npu.rt_open_net_service = (rt_open_net_service_fn)
				    dlsym(g_npu.rt_library, "rtOpenNetService");
	if (g_npu.rt_open_net_service == NULL) {
		fprintf(stderr, "RA rtOpenNetService missing\n");
		return -1;
	}
	rt_ext.param_info = "--hdcType=18";
	rt_ext.param_len = 12;
	rt_args.ext_param_list = &rt_ext;
	rt_args.ext_param_cnt = 1;
	if (g_npu.rt_open_net_service(&rt_args) != 0) {
		fprintf(stderr, "rtOpenNetService failed\n");
		return -1;
	}

	/* Step 2: physical device id (libacl_rt.so). */
	g_npu.rt_acl_library = dlopen("libacl_rt.so", RTLD_NOW | RTLD_GLOBAL);
	if (g_npu.rt_acl_library != NULL) {
		g_npu.get_phy_dev_id = (aclrt_get_phy_dev_id_fn)
				       dlsym(g_npu.rt_acl_library,
					     "aclrtGetPhyDevIdByLogicDevId");
	}
	if (g_npu.get_phy_dev_id == NULL) {
		fprintf(stderr, "RA aclrtGetPhyDevIdByLogicDevId missing\n");
		return -1;
	}
	if (g_npu.get_phy_dev_id(device_id, &g_npu.ra_phy_id) != 0) {
		fprintf(stderr, "aclrtGetPhyDevIdByLogicDevId(%d) failed\n", device_id);
		return -1;
	}

	/* Step 3: RaInit; 328002 = repeat init, treated as success. */
	memset(&g_npu.ra_cfg, 0, sizeof(g_npu.ra_cfg));
	g_npu.ra_cfg.phy_id = g_npu.ra_phy_id;
	g_npu.ra_cfg.nic_position = RA_NETWORK_OFFLINE;
	g_npu.ra_cfg.hdc_type = RA_HDC_TYPE;
	g_npu.ra_cfg.enable_hdc_async = true;
	ret = g_npu.ra_init(&g_npu.ra_cfg);
	if (ret != 0 && ret != RA_INIT_REPEAT_RET_CODE) {
		fprintf(stderr, "RaInit failed: ret %d\n", ret);
		return -1;
	}

	/* Step 4: query real eid/eidIndex (nds.c nds_init_get_eid_info). */
	ra_info.phy_id = g_npu.ra_phy_id;
	ret = g_npu.ra_get_eid_num(ra_info, &eid_num);
	if (ret != 0 || eid_num == 0) {
		fprintf(stderr, "RaGetDevEidInfoNum failed: ret %d num %u\n",
			ret, eid_num);
		return -1;
	}
	eid_list = calloc(eid_num, sizeof(*eid_list));
	if (eid_list == NULL) {
		return -1;
	}
	ret = g_npu.ra_get_eid_list(ra_info, eid_list, &eid_num);
	if (ret != 0) {
		fprintf(stderr, "RaGetDevEidInfoList failed: ret %d\n", ret);
		free(eid_list);
		return -1;
	}

	/* Step 5: RaCtxInit with the queried eid (nds.c nds_init_ub_resources). */
	ra_ctx_cfg.mode = RA_NETWORK_OFFLINE;
	ra_attr.phy_id = g_npu.ra_phy_id;
	ra_attr.ub.eid_index = eid_list[0].eid_index;
	memcpy(ra_attr.ub.eid.raw, eid_list[0].eid.raw, sizeof(ra_attr.ub.eid.raw));
	ret = g_npu.ra_ctx_init(&ra_ctx_cfg, &ra_attr, &g_npu.ra_ctx_handle);
	if (ret != 0) {
		fprintf(stderr, "RaCtxInit failed: ret %d\n", ret);
		free(eid_list);
		return -1;
	}
	memcpy(g_npu.ra_eid, eid_list[0].eid.raw, sizeof(g_npu.ra_eid));
	free(eid_list);
	g_npu.ra_ready = true;
	printf("RA/HCCP context ready (phy_id=%u eid_index=%u)\n",
	       g_npu.ra_phy_id, ra_attr.ub.eid_index);
	return 0;
}

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
	/* RTLD_GLOBAL per P2-20/P2-21: RA and CANN symbols must stay mutually
	 * visible (CCDK nds.c uses RTLD_GLOBAL for libascendcl too). */
	g_npu.library = dlopen(lib_name, RTLD_NOW | RTLD_GLOBAL);
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
	err = g_npu.create_context(&g_npu.context, device_id);
	if (err != NPU_ACL_SUCCESS) {
		fprintf(stderr, "aclrtCreateContext failed: aclError %d\n", err);
		goto fail;
	}

	/* RA/HCCP init (optional): libra.so enables the third official HBM
	 * registration path (RaCtxLmemRegister nonPin). Absent libra -> RA
	 * path disabled with a warning; the provider keeps working for the
	 * non-direct routes. */
	if (npu_ra_init(device_id) != 0) {
		fprintf(stderr, "RA/HCCP HBM registration disabled\n");
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
	if (g_npu.ra_ctx_handle != NULL && g_npu.ra_ctx_deinit != NULL) {
		g_npu.ra_ctx_deinit(g_npu.ra_ctx_handle);
		g_npu.ra_ctx_handle = NULL;
	}
	if (g_npu.ra_ready && g_npu.ra_deinit != NULL) {
		g_npu.ra_deinit(&g_npu.ra_cfg);
	}
	g_npu.ra_ready = false;
	if (g_npu.ra_library != NULL) {
		dlclose(g_npu.ra_library);
		g_npu.ra_library = NULL;
	}
	if (g_npu.rt_library != NULL) {
		dlclose(g_npu.rt_library);
		g_npu.rt_library = NULL;
	}
	if (g_npu.rt_acl_library != NULL) {
		dlclose(g_npu.rt_acl_library);
		g_npu.rt_acl_library = NULL;
	}
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
	struct ra_lmem_reg_info lmem = {};
	int ret;

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

	/* RA/HCCP registration (third official path): RaCtxLmemRegister with
	 * nonPin=ENABLE makes the HBM segment remotely DMA-able via URMA
	 * jetty ops — bypasses the kernel gpu_p2p framework that 133's
	 * udma.ko lacks (P2-8/P2-19). Only possible when the RA context was
	 * initialized in npu_driver_init. */
	if (g_npu.ra_ready) {
		memset(&lmem, 0, sizeof(lmem));
		lmem.in.mem.addr = (uint64_t)(uintptr_t)addr;
		lmem.in.mem.size = length;
		lmem.in.ub.flags.bs.token_policy = 0;		/* TOKEN_POLICY_NONE */
		lmem.in.ub.flags.bs.cacheable = 0;		/* NON_CACHEABLE */
		lmem.in.ub.flags.bs.access = 0xE;		/* READ|WRITE|ATOMIC */
		lmem.in.ub.flags.bs.non_pin = 1;
		lmem.in.ub.flags.bs.token_id_valid = 0;
		ret = g_npu.ra_lmem_register(g_npu.ra_ctx_handle, &lmem,
					     &handle->lmem_handle);
		if (ret != 0) {
			fprintf(stderr, "RaCtxLmemRegister failed: ret %d\n", ret);
			free(handle);
			return -EIO;
		}
		handle->token_id = lmem.out.ub.token_id;
		handle->token_id_valid = true;
	}
	*pin_handle = handle;
	return 0;
}

/* NDS direct route: synthesize the peer-importable segment descriptor.
 * eid/va/len/token_id are all available from the RA path (P2-23 B); uasid
 * comes from the RA jetty key (RaCtxQpCreate output, nds_parse_jetty_info)
 * which is not sourced yet — gated until that format is extracted from the
 * CCDK. Returning nonzero here makes registration fall back to the legacy
 * urma_register_seg path (harmless: it fails visibly on 133). */
static int
npu_provider_get_segment_info(void *provider_ctx, void *pin_handle,
			      struct spdk_nvme_urma_seg_info *info)
{
	struct npu_pin_handle *handle = pin_handle;

	(void)provider_ctx;
	if (g_npu.library == NULL || !g_npu.ra_ready || handle == NULL) {
		return -ENODEV;
	}
	if (!handle->token_id_valid || !g_npu.ra_uasid_valid) {
		return -ENOTSUP;
	}
	memset(info, 0, sizeof(*info));
	memcpy(info->eid, g_npu.ra_eid, sizeof(info->eid));
	info->uasid = g_npu.ra_uasid;
	info->va = (uint64_t)(uintptr_t)handle->entry->addr;
	info->token_id = handle->token_id;
	info->len = handle->entry->size;
	return 0;
}

static void
npu_provider_unpin(void *provider_ctx, void *pin_handle)
{
	struct npu_pin_handle *handle = pin_handle;

	(void)provider_ctx;
	if (handle == NULL) {
		return;
	}
	if (g_npu.ra_ready && handle->lmem_handle != NULL) {
		g_npu.ra_lmem_unregister(g_npu.ra_ctx_handle, handle->lmem_handle);
	}
	free(handle);
}

/* export_dmabuf remains NULL (AscendCL has no public dmabuf export). The
 * direct route uses the RA/HCCP path instead: npu_provider_pin() calls
 * RaCtxLmemRegister(nonPin) so the HBM segment is remotely DMA-able via
 * URMA jetty ops — no kernel gpu_p2p framework needed (P2-19 verdict). */
static const struct spdk_nvme_urma_memory_provider g_npu_provider = {
	.name = "npu-urma-perf",
	.type = SPDK_NVME_URMA_MEM_NPU,
	.pin = npu_provider_pin,
	.unpin = npu_provider_unpin,
	.get_segment_info = npu_provider_get_segment_info,
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
