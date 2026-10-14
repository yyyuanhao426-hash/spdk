/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * p231_fake_ini — P2-31 bare-process initiator handshake probe (NDS).
 *
 * A bare (non-DPDK) process that speaks the SPDK NVMe/URMA control
 * plane: creates URMA resources exactly like SPDK's transport, connects
 * TCP to an nvmf_tgt URMA listener, sends the HELLO capsule with its
 * {eid, jetty_id}, waits for HELLO_RSP, imports the target's jetty and
 * binds. Purpose: make nvmf_tgt import a jetty created by a BARE
 * process — isolates the "DPDK importer" variable (P2-30 verdict:
 * creator-side and importer-side resource states are exonerated; the
 * only remaining variable is the DPDK EAL runtime itself).
 *
 * Usage:
 *   p231 <target_ip> <port>
 * Env knobs: DEV_NAME  EID_INDEX  MODE(RM|RC)  IMPORT_TOKEN (0xACFE)
 *            JFC_DEPTH(4096) JFR_DEPTH(2048) JETTY_DEPTH(2048)
 *            JETTY_PRIORITY(15) MAX_SGE(5) RNR_RETRY(7) ERR_TIMEOUT(17)
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

/* wire structs — must match lib/nvme/nvme_urma_internal.h exactly */
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

static int tcp_connect(const char *ip, int port)
{
	struct sockaddr_in addr = {};
	int fd, ret;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	ret = inet_pton(AF_INET, ip, &addr.sin_addr);
	if (ret != 1) {
		printf("bad ip %s\n", ip);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, 0);

		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
	char *p = buf;

	while (len > 0) {
		ssize_t n = recv(fd, p, len, 0);

		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
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
	const char *ip;
	int port, fd;
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
	urma_transport_mode_t mode = URMA_TM_RM;
	const char *dev_name = getenv("DEV_NAME");
	int count = 0, i;
	struct dbg_msg_hdr hdr = {};
	struct dbg_endpoint_desc desc = {};

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc < 3) {
		printf("usage: %s <target_ip> <port>\n", argv[0]);
		return 2;
	}
	ip = argv[1];
	port = atoi(argv[2]);

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
		if (urma_query_device(dev, &dev_attr) == URMA_SUCCESS) {
			jfc_depth = env_u32("JFC_DEPTH", 4096);
			jfr_depth = env_u32("JFR_DEPTH", 2048);
			max_sge = env_u32("MAX_SGE", 5);
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
	jfr_token = env_u32("JFR_TOKEN", 0xACFE);
	jetty_depth = env_u32("JETTY_DEPTH", 2048);
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
		fr.trans_mode = mode;
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
		jcfg.jfs_cfg.trans_mode = mode;
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
	printf("[p231] device=%s eid_index=%u jetty=%u eid=", dev->name,
	       eid_index, jetty->jetty_id.id);
	print_eid(self_eid.raw);
	printf("\n");

	fd = tcp_connect(ip, port);
	if (fd < 0) {
		return 1;
	}
	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_HELLO;
	hdr.length = sizeof(desc);
	hdr.qid = 1;
	desc.jetty_id = jetty->jetty_id.id;
	desc.transport_mode = (uint32_t)mode;
	desc.max_queue_depth = 128;
	desc.max_io_size = 131072;
	memcpy(desc.eid, self_eid.raw, 16);
	if (send_all(fd, &hdr, sizeof(hdr)) != 0 ||
	    send_all(fd, &desc, sizeof(desc)) != 0) {
		printf("send HELLO failed\n");
		return 1;
	}
	printf("[p231] HELLO sent (eid=");
	print_eid(desc.eid);
	printf(" id=%u), waiting HELLO_RSP...\n", desc.jetty_id);

	if (recv_all(fd, &hdr, sizeof(hdr)) != 0) {
		printf("[p231] peer closed before HELLO_RSP (target import likely failed)\n");
		return 1;
	}
	if (hdr.magic != SPDK_URMA_WIRE_MAGIC || hdr.type != SPDK_URMA_MSG_HELLO_RSP ||
	    hdr.length != sizeof(desc)) {
		printf("[p231] unexpected rsp hdr (type=%u len=%u)\n", hdr.type, hdr.length);
		return 1;
	}
	if (recv_all(fd, &desc, sizeof(desc)) != 0) {
		printf("[p231] read rsp desc failed\n");
		return 1;
	}
	{
		urma_rjetty_t rjetty = {};
		urma_token_t token = {};
		urma_target_jetty_t *tj;
		urma_status_t st;

		memcpy(rjetty.jetty_id.eid.raw, desc.eid, 16);
		rjetty.jetty_id.uasid = 0;
		rjetty.jetty_id.id = desc.jetty_id;
		rjetty.trans_mode = (urma_transport_mode_t)desc.transport_mode;
		rjetty.type = URMA_JETTY;
		rjetty.tp_type = URMA_CTP;
		token.token = env_u32("IMPORT_TOKEN", 0xACFE);
		tj = urma_import_jetty(ctx, &rjetty, &token);
		if (tj == NULL) {
			printf("[p231] import target jetty FAILED (id=%u)\n",
			       rjetty.jetty_id.id);
			return 1;
		}
		printf("[p231] import target jetty OK (id=%u)\n", rjetty.jetty_id.id);
		st = urma_bind_jetty(jetty, tj);
		printf("[p231] bind_jetty %s (status=%d)\n",
		       st == URMA_SUCCESS ? "OK" : "FAILED", (int)st);
	}
	printf("[p231] FULL BARE HANDSHAKE OK — press Enter to exit...\n");
	(void)getchar();
	return 0;
}
