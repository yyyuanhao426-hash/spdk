/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise the actual initiator and target jetty constructors. */
#include "nvme_urma_internal.h"

struct nvme_urma_qpair {
	struct {
		uint32_t id;
	} qpair;
	struct spdk_urma_device *device;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	urma_jfr_t **jfrs;
	uint32_t jetty_count, send_jfc_index;
};
struct nvmf_urma_qpair {
	struct {
		uint32_t qid;
	} qpair;
	struct spdk_urma_device *device;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	urma_jfr_t **jfrs;
	uint32_t jetty_count, send_jfc_index;
};
static bool expected_multipath;
static unsigned created;

void
spdk_log(enum spdk_log_level l, const char *f, const int n, const char *fn, const char *fmt, ...)
{
}

urma_jfr_t *
urma_create_jfr(urma_context_t *context, urma_jfr_cfg_t *cfg)
{
	urma_jfr_t *jfr = calloc(1, sizeof(*jfr));
	assert(jfr != NULL);
	jfr->jfr_cfg = *cfg;
	return jfr;
}

urma_status_t
urma_delete_jfr(urma_jfr_t *jfr)
{
	free(jfr);
	return URMA_SUCCESS;
}

urma_jetty_t *
urma_create_jetty(urma_context_t *context, urma_jetty_cfg_t *cfg)
{
	if (cfg->jfs_cfg.flag.bs.multi_path != expected_multipath) {
		fprintf(stderr, "jetty policy mismatch: expected multi_path=%u, actual=%u\n",
			(unsigned)expected_multipath, cfg->jfs_cfg.flag.bs.multi_path);
		abort();
	}
	assert(cfg->jfs_cfg.trans_mode == URMA_TM_RM);
	assert(cfg->jfs_cfg.priority == 0);
	assert(cfg->jfs_cfg.depth == 128);
	created++;
	return calloc(1, sizeof(urma_jetty_t));
}

urma_status_t
urma_delete_jetty(urma_jetty_t *jetty)
{
	free(jetty);
	return URMA_SUCCESS;
}

/* REAL_JETTY_CONSTRUCTORS */

static void
check(const char *name, bool balance, bool iod_multi, bool expected)
{
	urma_jfc_t jfc = {};
	urma_jfc_t *jfcs[] = {&jfc};
	urma_jfr_t jfr = {.jfr_cfg = {.depth = 128}};
	urma_jfr_t *jfrs[] = {&jfr};
	struct spdk_urma_device d = {
		.opts = {.num_jetty_per_ep = 2, .jetty_depth = 128,
			.transport_mode = URMA_TM_RM, .bonding_balance = balance,
			.bonding_multipath = iod_multi,
			.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_URMA},
		.send_jfcs = jfcs, .recv_jfcs = jfcs, .jfrs = jfrs,
		.send_jfc_count = 1, .recv_jfc_count = 1, .jfr_count = 1,
	};
	snprintf(d.dev_name, sizeof(d.dev_name), "%s", name);
	d.attr.dev_cap.max_jfs_inline_len = 65535;
	expected_multipath = expected;
	struct nvme_urma_qpair initiator = {.device = &d};
	struct nvmf_urma_qpair target = {.device = &d};
	assert(nvme_urma_create_jetty(&initiator) == 0);
	assert(nvmf_urma_create_jetty(&target) == 0);
	for (uint32_t i = 0; i < initiator.jetty_count; i++) {
		urma_delete_jetty(initiator.jettys[i]);
		urma_delete_jfr(initiator.jfrs[i]);
		urma_delete_jetty(target.jettys[i]);
		urma_delete_jfr(target.jfrs[i]);
	}
	free(initiator.jettys);
	free(initiator.target_jettys);
	free(initiator.jfrs);
	free(target.jettys);
	free(target.target_jettys);
	free(target.jfrs);
}

int
main(void)
{
	/* BALANCE+PORT must enable each JFS, as in Mooncake TE. */
	check("bonding_dev_23", true, false, true);
	check("bonding_dev_23", false, true, true);
	check("bonding_dev_23", false, false, false);
	check("udmac0d1e3", false, false, false);
	/* A balance request on raw must not enable bonding on that provider. */
	check("udmac0d1e3", true, false, false);
	/* Preserve the pre-existing explicit multi_path override. */
	check("udmac0d1e3", false, true, true);
	assert(created == 24);
	puts("PASS real initiator/target jetty multipath policy, BALANCE+PORT, IODIE and raw");
	return 0;
}
