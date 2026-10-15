/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * p237_fake_tgt — P2-37 bare-process target probe (NDS).
 *
 * Listens on a TCP port, accepts an SPDK URMA initiator connection,
 * reads the HELLO capsule, creates its own URMA resources (SPDK-style),
 * replies HELLO_RSP carrying {own eid, own jetty_id} — so the peer
 * (urma_perf, a DPDK process) performs urma_import_jetty against a
 * known-good bare creator. This isolates the "DPDK importer" variable
 * (never yet observed: in loopback the DPDK target always died before
 * the initiator's import ran).
 *
 * Usage:  p237 <listen_port>
 * Env knobs: DEV_NAME EID_INDEX MODE JFC_DEPTH JFR_DEPTH JFR_TOKEN
 *            JETTY_PRIORITY MAX_SGE RNR_RETRY ERR_TIMEOUT JETTY_DEPTH
 *            (SPDK defaults, cap-clamped)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <urma_api.h>
#include <urma_types.h>

#define SPDK_URMA_WIRE_MAGIC   0x41524d55u
#define SPDK_URMA_WIRE_VERSION 1
#define SPDK_URMA_MSG_HELLO     1
#define SPDK_URMA_MSG_HELLO_RSP 2

struct dbg_msg_hdr {
	uint32_t magic;
	uint16_t version;
	uint16_t type;
	uint32_t length;
	uint32_t qid;
} __attribute__((packed));

struct dbg_endpoint_desc {
	uint8_t eid[16];
	uint32_t jetty_id;
	uint32_t transport_mode;
	uint32_t max_queue_depth;
	uint32_t max_io_size;
} __attribute__((packed));

static uint32_t env_u32(const char *name, uint32_t def)
{
	const char *v = getenv(name);

	return (v == NULL || v[0] == '\0') ? def : (uint32_t)strtoul(v, NULL, 0);
}

static void print_eid(const uint8_t *eid)
{
	int i;

	for (i = 0; i < 16; i++) {
		printf("%02x", eid[i]);
	}
}

int main(int argc, char **argv)
{
	int port, lfd, cfd;
	urma_device_t **devices = NULL;
	urma_device_t *dev = NULL;
	urma_eid_info_t *eids = NULL;
	urma_context_t *ctx = NULL;
	urma_jfc_t *jfc = NULL;
	urma_jfr_t *jfr = NULL;
	urma_jetty_t *jetty = NULL;
	urma_eid_t self_eid = {};
	uint32_t eid_count = 0, eid_index, max_sge;
	uint32_t jfc_depth, jfr_depth, jfr_token, jetty_depth;
	const char *dev_name = getenv("DEV_NAME");
	int count = 0, i;
	struct dbg_msg_hdr hdr = {};
	struct dbg_endpoint_desc desc = {};
	struct sockaddr_in addr = {};

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc < 2) {
		printf("usage: %s <listen_port>\n", argv[0]);
		return 2;
	}
	port = atoi(argv[1]);

	if (urma_init(NULL) != URMA_SUCCESS) {
		printf("urma_init failed\n");
		return 1;
	}
	devices = urma_get_device_list(&count);
	for (i = 0; devices != NULL && i < count; i++) {
		if (dev_name == NULL || dev_name[0] == '\0' ||
		    strcmp(dev_name, devices[i]->name) == 0) {
			dev = devices[i];
			break;
		}
	}
	if (dev == NULL) {
		printf("device not found\n");
		return 1;
	}
	eids = urma_get_eid_list(dev, &eid_count);
	if (eids == NULL || eid_count == 0) {
		printf("no eid list\n");
		return 1;
	}
	eid_index = env_u32("EID_INDEX", 0);
	if (eid_index >= eid_count) {
		eid_index = eids[0].eid_index;
	}
	for (i = 0; i < (int)eid_count; i++) {
		if (eids[i].eid_index == eid_index) {
			self_eid = eids[i].eid;
			break;
		}
	}
	if (i == (int)eid_count) {
		self_eid = eids[0].eid;
	}
	ctx = urma_create_context(dev, eid_index);
	if (ctx == NULL) {
		printf("create_context failed\n");
		return 1;
	}
	{
		urma_device_attr_t dev_attr;

		memset(&dev_attr, 0, sizeof(dev_attr));
		jfc_depth = env_u32("JFC_DEPTH", 4096);
		jfr_depth = env_u32("JFR_DEPTH", 2048);
		jfr_token = env_u32("JFR_TOKEN", 0xACFE);
		jetty_depth = env_u32("JETTY_DEPTH", 2048);
		max_sge = env_u32("MAX_SGE", 5);
		if (urma_query_device(dev, &dev_attr) == URMA_SUCCESS) {
			if (dev_attr.dev_cap.max_jfc_depth != 0 &&
			    jfc_depth > dev_attr.dev_cap.max_jfc_depth) {
				jfc_depth = dev_attr.dev_cap.max_jfc_depth;
			}
			if (dev_attr.dev_cap.max_jfr_depth != 0 &&
			    jfr_depth > dev_attr.dev_cap.max_jfr_depth) {
				jfr_depth = dev_attr.dev_cap.max_jfr_depth;
			}
			if (dev_attr.dev_cap.max_jfr_sge != 0 &&
			    max_sge > dev_attr.dev_cap.max_jfr_sge) {
				max_sge = dev_attr.dev_cap.max_jfr_sge;
			}
		}
	}
	{
		urma_jfc_cfg_t fc = {};

		fc.depth = jfc_depth;
		jfc = urma_create_jfc(ctx, &fc);
		if (jfc == NULL) {
			printf("create_jfc failed\n");
			return 1;
		}
	}
	{
		urma_jfr_cfg_t fr = {};

		fr.depth = jfr_depth;
		fr.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
		fr.trans_mode = URMA_TM_RM;
		fr.max_sge = (uint8_t)max_sge;
		fr.min_rnr_timer = 12;
		fr.token_value.token = jfr_token;
		fr.jfc = jfc;
		jfr = urma_create_jfr(ctx, &fr);
		if (jfr == NULL) {
			printf("create_jfr failed\n");
			return 1;
		}
	}
	{
		urma_jetty_cfg_t jcfg = {};

		jcfg.jfs_cfg.depth = jetty_depth;
		jcfg.jfs_cfg.trans_mode = URMA_TM_RM;
		jcfg.jfs_cfg.priority = (uint8_t)env_u32("JETTY_PRIORITY", 15);
		jcfg.jfs_cfg.max_sge = (uint8_t)max_sge;
		jcfg.jfs_cfg.rnr_retry = (uint8_t)env_u32("RNR_RETRY", 7);
		jcfg.jfs_cfg.err_timeout = (uint8_t)env_u32("ERR_TIMEOUT", 17);
		jcfg.jfs_cfg.jfc = jfc;
		jcfg.flag.bs.share_jfr = 1;
		jcfg.shared.jfr = jfr;
		jcfg.shared.jfc = jfc;
		jetty = urma_create_jetty(ctx, &jcfg);
		if (jetty == NULL) {
			printf("create_jetty failed\n");
			return 1;
		}
	}
	printf("[p237] device=%s eid_index=%u jetty=%u eid=", dev->name,
	       eid_index, jetty->jetty_id.id);
	print_eid(self_eid.raw);
	printf("\n");

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}
	if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) != 0) {
		perror("setsockopt SO_REUSEADDR");
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("141.61.133.123");
	addr.sin_port = htons((uint16_t)port);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	if (listen(lfd, 1) != 0) {
		perror("listen");
		return 1;
	}
	printf("[p237] listening on 141.61.133.123:%d ...\n", port);
	cfd = accept(lfd, NULL, NULL);
	if (cfd < 0) {
		perror("accept");
		return 1;
	}
	printf("[p237] peer connected\n");

	if (recv(cfd, &hdr, sizeof(hdr), MSG_WAITALL) != sizeof(hdr)) {
		printf("[p237] read HELLO hdr failed\n");
		return 1;
	}
	if (hdr.magic != SPDK_URMA_WIRE_MAGIC || hdr.type != SPDK_URMA_MSG_HELLO ||
	    hdr.length != sizeof(desc)) {
		printf("[p237] bad HELLO (type=%u len=%u)\n", hdr.type, hdr.length);
		return 1;
	}
	if (recv(cfd, &desc, sizeof(desc), MSG_WAITALL) != sizeof(desc)) {
		printf("[p237] read HELLO desc failed\n");
		return 1;
	}
	printf("[p237] HELLO from peer: eid=");
	print_eid(desc.eid);
	printf(" jetty=%u\n", desc.jetty_id);

	/* reply with our own jetty coordinates */
	{
		struct dbg_endpoint_desc rsp_desc;

		hdr.type = SPDK_URMA_MSG_HELLO_RSP;
		memset(&rsp_desc, 0, sizeof(rsp_desc));
		memcpy(rsp_desc.eid, self_eid.raw, 16);
		rsp_desc.jetty_id = jetty->jetty_id.id;
		rsp_desc.transport_mode = (uint32_t)URMA_TM_RM;
		rsp_desc.max_queue_depth = 128;
		rsp_desc.max_io_size = 131072;
		if (send(cfd, &hdr, sizeof(hdr), 0) != sizeof(hdr) ||
		    send(cfd, &rsp_desc, sizeof(rsp_desc), 0) != sizeof(rsp_desc)) {
			printf("[p237] send HELLO_RSP failed\n");
			return 1;
		}
	}
	printf("[p237] HELLO_RSP sent (own eid=");
	print_eid(self_eid.raw);
	printf(" jetty=%u) — peer should now urma_import_jetty us\n",
	       jetty->jetty_id.id);
	printf("press Enter to exit (keep resources alive)...\n");
	(void)getchar();
	return 0;
}
