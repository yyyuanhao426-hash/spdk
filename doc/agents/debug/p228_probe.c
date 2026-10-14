/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * p228_probe (rev2) — P2-28/P2-30 discriminating probe (NDS debugging).
 *
 * Replicates the SPDK URMA transport resource-creation sequence
 * (JFC -> shared JFR -> jetty with share_jfr=1) with every parameter
 * under an env knob, plus P2-side resource pre-creation knobs for the
 * importer-state experiment.
 *
 * rev2 fixes (P2-28 findings):
 *   - setvbuf line buffering (p1 output was lost when killed)
 *   - clamp depths/sge to device cap (default MAX_SGE=5 > max_jfr_sge=4
 *     made p1 create_jfr fail spuriously)
 *   - p2 mode honors P2_CREATE_JFC / P2_CREATE_JFR (importer-state test)
 *
 * Usage:
 *   p228 p1                                  # creator (blocks on stdin)
 *   p228 p2 <eid_hex32> <jetty_id> <import_token_hex>
 *
 * Env knobs:
 *   DEV_NAME  EID_INDEX  MODE(RM|RC)
 *   CREATE_JFC (1)  CREATE_JFR (1)            # p1 creator switches
 *   P2_CREATE_JFC (0)  P2_CREATE_JFR (0)      # p2 importer-side switches
 *   JFC_DEPTH (4096)  JFR_DEPTH (2048)  JFR_TOKEN (0xACFE)
 *   JETTY_PRIORITY (15)  MAX_SGE (5)  RNR_RETRY (7)  ERR_TIMEOUT (17)
 *   JETTY_DEPTH (2048)  MIN_RNR (12)
 *   IMPORT_TOKEN (0xACFE)                     # p2 only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <urma_api.h>
#include <urma_types.h>

static uint32_t env_u32(const char *name, uint32_t def)
{
	const char *v = getenv(name);

	return (v == NULL || v[0] == '\0') ? def : (uint32_t)strtoul(v, NULL, 0);
}

static urma_transport_mode_t env_mode(void)
{
	const char *v = getenv("MODE");

	if (v != NULL && strcasecmp(v, "RC") == 0) {
		return URMA_TM_RC;
	}
	if (v != NULL && strcasecmp(v, "UM") == 0) {
		return URMA_TM_UM;
	}
	return URMA_TM_RM;
}

struct probe_env {
	urma_context_t *ctx;
	urma_device_t *dev;
	urma_eid_t eid;
	uint32_t eid_index;
	urma_device_attr_t attr;
};

static urma_device_t *sel_device(const char *tag)
{
	urma_device_t **devices = NULL;
	urma_device_t *selected = NULL;
	const char *dev_name = getenv("DEV_NAME");
	int count = 0, i;

	devices = urma_get_device_list(&count);
	for (i = 0; devices != NULL && i < count; i++) {
		if (dev_name == NULL || dev_name[0] == '\0' ||
		    strcmp(dev_name, devices[i]->name) == 0) {
			selected = devices[i];
			break;
		}
	}
	if (selected == NULL) {
		printf("[%s] device %s not found (n=%d)\n", tag,
		       dev_name ? dev_name : "<first>", count);
	}
	return selected;
}

static int probe_open(struct probe_env *e, const char *tag)
{
	urma_eid_info_t *eids = NULL;
	uint32_t eid_count = 0, i;

	memset(e, 0, sizeof(*e));
	if (urma_init(NULL) != URMA_SUCCESS) {
		printf("[%s] urma_init failed\n", tag);
		return -1;
	}
	e->dev = sel_device(tag);
	if (e->dev == NULL) {
		return -1;
	}
	eids = urma_get_eid_list(e->dev, &eid_count);
	if (eids == NULL || eid_count == 0) {
		printf("[%s] no eid list\n", tag);
		return -1;
	}
	e->eid_index = env_u32("EID_INDEX", 0);
	if (e->eid_index >= eid_count) {
		e->eid_index = eids[0].eid_index;
	}
	for (i = 0; i < (int)eid_count; i++) {
		if (eids[i].eid_index == e->eid_index) {
			e->eid = eids[i].eid;
			break;
		}
	}
	if (i == (int)eid_count) {
		e->eid = eids[0].eid;
	}
	e->ctx = urma_create_context(e->dev, e->eid_index);
	if (e->ctx == NULL) {
		printf("[%s] create_context failed\n", tag);
		return -1;
	}
	if (urma_query_device(e->dev, &e->attr) != URMA_SUCCESS) {
		printf("[%s] query_device failed (cap clamp disabled)\n", tag);
		memset(&e->attr, 0, sizeof(e->attr));
	}
	printf("[%s] device=%s eid_index=%u\n", tag, e->dev->name, e->eid_index);
	return 0;
}

static int create_resources(struct probe_env *e, const char *tag,
			    int create_jfc, int create_jfr,
			    urma_jfc_t **jfc_out, urma_jfr_t **jfr_out)
{
	uint32_t jfc_depth = env_u32("JFC_DEPTH", 4096);
	uint32_t jfr_depth = env_u32("JFR_DEPTH", 2048);
	uint32_t jfr_token = env_u32("JFR_TOKEN", 0xACFE);
	uint32_t max_sge = env_u32("MAX_SGE", 5);
	urma_jfc_t *jfc = NULL;
	urma_jfr_t *jfr = NULL;

	*jfc_out = NULL;
	*jfr_out = NULL;
	/* clamp to device cap (P2-28: uncapped defaults exceed max_jfr_sge) */
	if (e->attr.dev_cap.max_jfc_depth != 0 && jfc_depth > e->attr.dev_cap.max_jfc_depth) {
		jfc_depth = e->attr.dev_cap.max_jfc_depth;
	}
	if (e->attr.dev_cap.max_jfr_depth != 0 && jfr_depth > e->attr.dev_cap.max_jfr_depth) {
		jfr_depth = e->attr.dev_cap.max_jfr_depth;
	}
	if (e->attr.dev_cap.max_jfr_sge != 0 && max_sge > e->attr.dev_cap.max_jfr_sge) {
		max_sge = e->attr.dev_cap.max_jfr_sge;
	}
	if (create_jfc) {
		urma_jfc_cfg_t fc = {};

		fc.depth = jfc_depth;
		jfc = urma_create_jfc(e->ctx, &fc);
		if (jfc == NULL) {
			printf("[%s] create_jfc(depth %u) failed\n", tag, jfc_depth);
			return -1;
		}
		printf("[%s] jfc depth=%u ok\n", tag, jfc_depth);
	}
	if (create_jfr) {
		urma_jfr_cfg_t fr = {};

		fr.depth = jfr_depth;
		fr.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
		fr.trans_mode = env_mode();
		fr.max_sge = (uint8_t)max_sge;
		fr.min_rnr_timer = (uint8_t)env_u32("MIN_RNR", 12);
		fr.token_value.token = jfr_token;
		fr.jfc = jfc;
		jfr = urma_create_jfr(e->ctx, &fr);
		if (jfr == NULL) {
			printf("[%s] create_jfr(depth %u token %#x sge %u) failed\n",
			       tag, jfr_depth, jfr_token, max_sge);
			return -1;
		}
		printf("[%s] jfr depth=%u token=%#x sge=%u ok\n", tag,
		       jfr_depth, jfr_token, max_sge);
	}
	*jfc_out = jfc;
	*jfr_out = jfr;
	return 0;
}

static void print_eid(const urma_eid_t *eid)
{
	int i;

	for (i = 0; i < 16; i++) {
		printf("%02x", eid->raw[i]);
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc < 2) {
		printf("usage: %s p1 | p2 <eid_hex32> <jetty_id> <import_token_hex>\n",
		       argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "p1") == 0) {
		struct probe_env e;
		urma_jfc_t *jfc = NULL;
		urma_jfr_t *jfr = NULL;
		urma_jetty_t *jetty = NULL;
		urma_jetty_cfg_t jcfg = {};

		if (probe_open(&e, "p1") != 0) {
			return 1;
		}
		if (create_resources(&e, "p1", (int)env_u32("CREATE_JFC", 1),
				     (int)env_u32("CREATE_JFR", 1), &jfc, &jfr) != 0) {
			return 1;
		}
		jcfg.jfs_cfg.depth = env_u32("JETTY_DEPTH", 2048);
		jcfg.jfs_cfg.trans_mode = env_mode();
		jcfg.jfs_cfg.priority = (uint8_t)env_u32("JETTY_PRIORITY", 15);
		jcfg.jfs_cfg.max_sge = (uint8_t)env_u32("MAX_SGE", 5);
		jcfg.jfs_cfg.rnr_retry = (uint8_t)env_u32("RNR_RETRY", 7);
		jcfg.jfs_cfg.err_timeout = (uint8_t)env_u32("ERR_TIMEOUT", 17);
		jcfg.jfs_cfg.jfc = jfc;
		jcfg.flag.bs.share_jfr = 1;
		jcfg.shared.jfr = jfr;
		jcfg.shared.jfc = jfc;
		jetty = urma_create_jetty(e.ctx, &jcfg);
		if (jetty == NULL) {
			printf("[p1] create_jetty failed (depth %u prio %u)\n",
			       jcfg.jfs_cfg.depth, jcfg.jfs_cfg.priority);
			return 1;
		}
		printf("EID ");
		print_eid(&e.eid);
		printf(" JETTY %u\n", jetty->jetty_id.id);
		printf("press Enter to exit (keep resources alive for p2)...\n");
		(void)getchar();
		return 0;
	}

	if (strcmp(argv[1], "p2") == 0) {
		struct probe_env e;
		urma_jfc_t *jfc = NULL;
		urma_jfr_t *jfr = NULL;
		urma_rjetty_t rjetty = {};
		urma_token_t token = {};
		urma_target_jetty_t *tj;
		uint8_t eid[16];
		int i;

		if (argc < 5) {
			printf("p2 needs <eid_hex32> <jetty_id> <import_token_hex>\n");
			return 2;
		}
		if (strlen(argv[2]) != 32) {
			printf("eid must be 32 hex chars\n");
			return 2;
		}
		for (i = 0; i < 16; i++) {
			unsigned byte;

			if (sscanf(argv[2] + 2 * i, "%2x", &byte) != 1) {
				printf("bad eid hex\n");
				return 2;
			}
			eid[i] = (uint8_t)byte;
		}
		if (probe_open(&e, "p2") != 0) {
			return 1;
		}
		/* importer-side state experiment: optionally pre-create the
		 * same JFC/JFR resources SPDK's target has before importing */
		if (env_u32("P2_CREATE_JFC", 0) || env_u32("P2_CREATE_JFR", 0)) {
			if (create_resources(&e, "p2", (int)env_u32("P2_CREATE_JFC", 0),
					     (int)env_u32("P2_CREATE_JFR", 0),
					     &jfc, &jfr) != 0) {
				return 1;
			}
		}
		memcpy(rjetty.jetty_id.eid.raw, eid, 16);
		rjetty.jetty_id.uasid = 0;
		rjetty.jetty_id.id = (uint32_t)strtoul(argv[3], NULL, 0);
		rjetty.trans_mode = env_mode();
		rjetty.type = URMA_JETTY;
		rjetty.tp_type = URMA_CTP;
		token.token = (uint32_t)strtoul(argv[4], NULL, 0);
		tj = urma_import_jetty(e.ctx, &rjetty, &token);
		if (tj == NULL) {
			printf("[p2] import_jetty FAILED (id=%u token=%#x)\n",
			       rjetty.jetty_id.id, token.token);
			return 1;
		}
		printf("[p2] import_jetty OK (id=%u token=%#x)\n",
		       rjetty.jetty_id.id, token.token);
		return 0;
	}

	printf("unknown mode %s\n", argv[1]);
	return 2;
}
