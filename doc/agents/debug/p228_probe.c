/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * p228_probe — P2-28 discriminating probe (NDS debugging).
 *
 * Replicates the SPDK URMA transport resource-creation sequence
 * (JFC -> shared JFR -> jetty with share_jfr=1) with every parameter
 * under an env knob, so the failing -M cpu loopback import
 * (ubcore create_tpid -28 / ctrl queue send -2) can be bisected
 * against the known-good bare-jetty sequence (p227 probe).
 *
 * Usage:
 *   p228 p1                       # creator: build resources, print EID +
 *                                 # JETTY id, block on stdin until Enter
 *   p228 p2 <eid_hex32> <jetty_id> <import_token_hex>
 *
 * Env knobs (both sides):
 *   DEV_NAME       device name (default: first in list)
 *   EID_INDEX      (default 0)
 *   MODE           RM | RC           (default RM)
 *   CREATE_JFC     1 = create JFC (SPDK does), 0 = skip  (default 1)
 *   CREATE_JFR     1 = create shared JFR,       0 = skip  (default 1)
 *   JFC_DEPTH      (default 4096, SPDK default)
 *   JFR_DEPTH      (default 2048, SPDK default)
 *   JFR_TOKEN      hex, default 0xACFE (SPDK default)
 *   JETTY_PRIORITY default 15 (SPDK default)
 *   MAX_SGE        default 5 (SPDK default)
 *   RNR_RETRY      default 7 (SPDK default)
 *   ERR_TIMEOUT    default 17 (SPDK default)
 *   JETTY_DEPTH    default 2048 (SPDK default)
 *   MIN_RNR        default 12 (URMA_TYPICAL_MIN_RNR_TIMER)
 *   IMPORT_TOKEN   hex, default 0xACFE (SPDK target default; p219 used 0)
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

static urma_context_t *probe_open_context(const char *tag)
{
	urma_device_t **devices = NULL;
	urma_device_t *selected = NULL;
	urma_eid_info_t *eids = NULL;
	urma_context_t *ctx = NULL;
	uint32_t eid_count = 0, eid_index = env_u32("EID_INDEX", 0);
	const char *dev_name = getenv("DEV_NAME");
	int count = 0, i;

	if (urma_init(NULL) != URMA_SUCCESS) {
		printf("[%s] urma_init failed\n", tag);
		return NULL;
	}
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
		return NULL;
	}
	eids = urma_get_eid_list(selected, &eid_count);
	if (eids == NULL || eid_count == 0) {
		printf("[%s] no eid list\n", tag);
		return NULL;
	}
	if (eid_index >= eid_count) {
		eid_index = eids[0].eid_index;
	}
	ctx = urma_create_context(selected, eid_index);
	if (ctx == NULL) {
		printf("[%s] create_context(%s eid%u) failed\n", tag,
		       selected->name, eid_index);
		return NULL;
	}
	printf("[%s] device=%s eid_index=%u\n", tag, selected->name, eid_index);
	return ctx;
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
	urma_transport_mode_t mode = env_mode();

	if (argc < 2) {
		printf("usage: %s p1 | p2 <eid_hex32> <jetty_id> <import_token_hex>\n",
		       argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "p1") == 0) {
		urma_context_t *ctx = probe_open_context("p1");
		urma_jfc_t *jfc = NULL;
		urma_jfr_t *jfr = NULL;
		urma_jetty_t *jetty = NULL;
		uint32_t jfc_depth = env_u32("JFC_DEPTH", 4096);
		uint32_t jfr_depth = env_u32("JFR_DEPTH", 2048);
		uint32_t jfr_token = env_u32("JFR_TOKEN", 0xACFE);
		uint32_t jetty_depth = env_u32("JETTY_DEPTH", 2048);
		int create_jfc = (int)env_u32("CREATE_JFC", 1);
		int create_jfr = (int)env_u32("CREATE_JFR", 1);
		urma_jetty_cfg_t jcfg = {};
		urma_eid_t self_eid = {};
		urma_eid_info_t *eids = NULL;
		urma_device_t **devices = NULL;
		uint32_t eid_count = 0, eid_index = env_u32("EID_INDEX", 0);
		int count = 0, i;
		const char *dev_name = getenv("DEV_NAME");

		if (ctx == NULL) {
			return 1;
		}
		if (create_jfc) {
			urma_jfc_cfg_t fc = {};

			fc.depth = jfc_depth;
			jfc = urma_create_jfc(ctx, &fc);
			if (jfc == NULL) {
				printf("[p1] create_jfc(depth %u) failed\n", jfc_depth);
				return 1;
			}
			printf("[p1] jfc depth=%u ok\n", jfc_depth);
		}
		if (create_jfr) {
			urma_jfr_cfg_t fr = {};

			fr.depth = jfr_depth;
			fr.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
			fr.trans_mode = mode;
			fr.max_sge = (uint8_t)env_u32("MAX_SGE", 5);
			fr.min_rnr_timer = (uint8_t)env_u32("MIN_RNR", 12);
			fr.token_value.token = jfr_token;
			fr.jfc = jfc;
			jfr = urma_create_jfr(ctx, &fr);
			if (jfr == NULL) {
				printf("[p1] create_jfr(depth %u token %#x) failed\n",
				       jfr_depth, jfr_token);
				return 1;
			}
			printf("[p1] jfr depth=%u token=%#x ok\n", jfr_depth, jfr_token);
		}
		jcfg.jfs_cfg.depth = jetty_depth;
		jcfg.jfs_cfg.trans_mode = mode;
		jcfg.jfs_cfg.priority = (uint8_t)env_u32("JETTY_PRIORITY", 15);
		jcfg.jfs_cfg.max_sge = (uint8_t)env_u32("MAX_SGE", 5);
		jcfg.jfs_cfg.rnr_retry = (uint8_t)env_u32("RNR_RETRY", 7);
		jcfg.jfs_cfg.err_timeout = (uint8_t)env_u32("ERR_TIMEOUT", 17);
		jcfg.jfs_cfg.jfc = jfc;
		jcfg.flag.bs.share_jfr = 1;
		jcfg.shared.jfr = jfr;
		jcfg.shared.jfc = jfc;
		jetty = urma_create_jetty(ctx, &jcfg);
		if (jetty == NULL) {
			printf("[p1] create_jetty failed (depth %u prio %u share_jfr=1)\n",
			       jetty_depth, jcfg.jfs_cfg.priority);
			return 1;
		}
		/* fetch the context eid for printing (same rule as SPDK) */
		devices = urma_get_device_list(&count);
		for (i = 0; devices != NULL && i < count; i++) {
			if (dev_name == NULL || dev_name[0] == '\0' ||
			    strcmp(dev_name, devices[i]->name) == 0) {
				eids = urma_get_eid_list(devices[i], &eid_count);
				if (eids != NULL && eid_count > 0) {
					uint32_t k;

					for (k = 0; k < eid_count; k++) {
						if (eids[k].eid_index == eid_index) {
							self_eid = eids[k].eid;
							break;
						}
					}
					if (k == eid_count) {
						self_eid = eids[0].eid;
					}
				}
				break;
			}
		}
		printf("EID ");
		print_eid(&self_eid);
		printf(" JETTY %u (mode=%s jfc=%d jfr=%d prio=%u depth=%u)\n",
		       jetty->jetty_id.id,
		       mode == URMA_TM_RM ? "RM" : (mode == URMA_TM_RC ? "RC" : "UM"),
		       create_jfc, create_jfr, jcfg.jfs_cfg.priority, jetty_depth);
		printf("press Enter to exit (keep resources alive for p2)...\n");
		(void)getchar();
		return 0;
	}

	if (strcmp(argv[1], "p2") == 0) {
		urma_context_t *ctx;
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
		ctx = probe_open_context("p2");
		if (ctx == NULL) {
			return 1;
		}
		memcpy(rjetty.jetty_id.eid.raw, eid, 16);
		rjetty.jetty_id.uasid = 0;
		rjetty.jetty_id.id = (uint32_t)strtoul(argv[3], NULL, 0);
		rjetty.trans_mode = mode;
		rjetty.type = URMA_JETTY;
		rjetty.tp_type = URMA_CTP;
		token.token = (uint32_t)strtoul(argv[4], NULL, 0);
		tj = urma_import_jetty(ctx, &rjetty, &token);
		if (tj == NULL) {
			printf("[p2] import_jetty FAILED (eid=");
			print_eid(&rjetty.jetty_id.eid);
			printf(" id=%u token=%#x mode=%s)\n", rjetty.jetty_id.id,
			       token.token,
			       mode == URMA_TM_RM ? "RM" : (mode == URMA_TM_RC ? "RC" : "UM"));
			return 1;
		}
		printf("[p2] import_jetty OK (id=%u token=%#x)\n",
		       rjetty.jetty_id.id, token.token);
		return 0;
	}

	printf("unknown mode %s\n", argv[1]);
	return 2;
}
