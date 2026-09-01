/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

/* Experimental independent NVMe/URMA target transport.  The bootstrap socket
 * carries only endpoint descriptors, NVMe capsules and completions. */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/json.h"

#include "nvmf_internal.h"
#include "transport.h"
#include "../nvme/nvme_urma_internal.h"

#include <netdb.h>
#include <sys/ioctl.h>

enum nvmf_urma_req_state {
	NVMF_URMA_REQ_FREE = 0,
	NVMF_URMA_REQ_NEED_BUFFER,
	NVMF_URMA_REQ_PULLING,
	NVMF_URMA_REQ_EXECUTING,
	NVMF_URMA_REQ_PUSHING,
};

struct nvmf_urma_transport;
struct nvmf_urma_poll_group;

struct nvmf_urma_req {
	struct spdk_nvmf_request req;
	union nvmf_h2c_msg cmd;
	union nvmf_c2h_msg rsp;
	struct spdk_urma_data_desc remote_data;
	urma_target_seg_t *remote_seg;
	struct spdk_nvme_urma_memory_region *local_region;
	enum nvmf_urma_req_state state;
	TAILQ_ENTRY(nvmf_urma_req) link;
};

struct nvmf_urma_qpair {
	struct spdk_nvmf_qpair qpair;
	struct nvmf_urma_poll_group *group;
	struct nvmf_urma_transport *transport;
	struct spdk_urma_device *device;
	urma_jetty_t *jetty;
	urma_target_jetty_t *target_jetty;
	int fd;
	char peer_addr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	char local_addr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	char service[SPDK_NVMF_TRSVCID_MAX_LEN + 1];
	uint32_t resource_count;
	uint32_t max_io_size;
	struct nvmf_urma_req *reqs;
	TAILQ_HEAD(, nvmf_urma_req) free_reqs;
	TAILQ_HEAD(, nvmf_urma_req) working_reqs;
	TAILQ_ENTRY(nvmf_urma_qpair) link;
	spdk_nvmf_transport_qpair_fini_cb fini_cb;
	void *fini_arg;
};

struct nvmf_urma_port {
	struct spdk_nvme_transport_id trid;
	int fd;
	TAILQ_ENTRY(nvmf_urma_port) link;
};

struct nvmf_urma_poll_group {
	struct spdk_nvmf_transport_poll_group group;
	TAILQ_HEAD(, nvmf_urma_qpair) qpairs;
	TAILQ_ENTRY(nvmf_urma_poll_group) link;
};

struct nvmf_urma_transport {
	struct spdk_nvmf_transport transport;
	struct spdk_urma_transport_opts urma_opts;
	struct spdk_urma_device *device;
	struct spdk_poller *accept_poller;
	TAILQ_HEAD(, nvmf_urma_port) ports;
	TAILQ_HEAD(, nvmf_urma_poll_group) poll_groups;
};

struct nvmf_urma_json_opts {
	char *dev_name;
	char *trans_mode;
	int32_t active_port;
	uint32_t eid_index;
	uint32_t jfc_count;
	uint32_t jfc_depth;
	uint32_t jetty_count;
	uint32_t jetty_depth;
	bool bonding_balance;
	bool bonding_multipath;
};

static const struct spdk_json_object_decoder g_urma_opts_decoder[] = {
	{"dev_name", offsetof(struct nvmf_urma_json_opts, dev_name), spdk_json_decode_string, true},
	{"trans_mode", offsetof(struct nvmf_urma_json_opts, trans_mode), spdk_json_decode_string, true},
	{"active_port", offsetof(struct nvmf_urma_json_opts, active_port), spdk_json_decode_int32, true},
	{"eid_index", offsetof(struct nvmf_urma_json_opts, eid_index), spdk_json_decode_uint32, true},
	{"jfc_count", offsetof(struct nvmf_urma_json_opts, jfc_count), spdk_json_decode_uint32, true},
	{"jfc_depth", offsetof(struct nvmf_urma_json_opts, jfc_depth), spdk_json_decode_uint32, true},
	{"jetty_count", offsetof(struct nvmf_urma_json_opts, jetty_count), spdk_json_decode_uint32, true},
	{"jetty_depth", offsetof(struct nvmf_urma_json_opts, jetty_depth), spdk_json_decode_uint32, true},
	{"bonding_balance", offsetof(struct nvmf_urma_json_opts, bonding_balance), spdk_json_decode_bool, true},
	{"bonding_multipath", offsetof(struct nvmf_urma_json_opts, bonding_multipath), spdk_json_decode_bool, true},
};

static inline struct nvmf_urma_qpair *
nvmf_urma_qpair(struct spdk_nvmf_qpair *qpair)
{
	return SPDK_CONTAINEROF(qpair, struct nvmf_urma_qpair, qpair);
}

static inline struct nvmf_urma_req *
nvmf_urma_req(struct spdk_nvmf_request *req)
{
	return SPDK_CONTAINEROF(req, struct nvmf_urma_req, req);
}

static int
nvmf_urma_write_full(int fd, const void *buf, size_t length)
{
	const uint8_t *pos = buf;
	while (length != 0) {
		ssize_t rc = send(fd, pos, length, MSG_NOSIGNAL);
		if (rc < 0 && errno == EINTR) {
			continue;
		}
		if (rc <= 0) {
			return rc == 0 ? -ECONNRESET : -errno;
		}
		pos += rc;
		length -= rc;
	}
	return 0;
}

static int
nvmf_urma_read_full(int fd, void *buf, size_t length)
{
	uint8_t *pos = buf;
	while (length != 0) {
		ssize_t rc = recv(fd, pos, length, 0);
		if (rc < 0 && errno == EINTR) {
			continue;
		}
		if (rc <= 0) {
			return rc == 0 ? -ECONNRESET : -errno;
		}
		pos += rc;
		length -= rc;
	}
	return 0;
}

static int
nvmf_urma_create_jetty(struct nvmf_urma_qpair *uqpair)
{
	struct spdk_urma_device *device = uqpair->device;
	urma_jfs_cfg_t jfs = {};
	urma_jetty_cfg_t cfg = {};

	jfs.depth = device->opts.jetty_depth;
	jfs.trans_mode = device->opts.transport_mode;
	jfs.priority = SPDK_URMA_DEFAULT_PRIORITY;
	jfs.max_sge = SPDK_URMA_DEFAULT_MAX_SGE;
	jfs.rnr_retry = SPDK_URMA_DEFAULT_RNR_RETRY;
	jfs.err_timeout = SPDK_URMA_DEFAULT_ERR_TIMEOUT;
	jfs.jfc = device->jfcs[0];
	/* Modified by Yin: UB transport 强制 share_jfr=1，改用 device 预建的共享 jfr */
	cfg.jfs_cfg = jfs;
	cfg.flag.bs.share_jfr = 1;
	cfg.shared.jfr = device->jfr;
	cfg.shared.jfc = device->jfcs[0];
	uqpair->jetty = urma_create_jetty(device->context, &cfg);
	return uqpair->jetty == NULL ? -EIO : 0;
}

static int
nvmf_urma_handshake(struct nvmf_urma_qpair *uqpair)
{
	struct spdk_urma_device *device = uqpair->device;
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_endpoint_desc local = {}, remote = {};
	urma_rjetty_t rjetty = {};
	int rc;

	rc = nvmf_urma_read_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc != 0 || hdr.magic != SPDK_URMA_WIRE_MAGIC ||
	    hdr.version != SPDK_URMA_WIRE_VERSION || hdr.type != SPDK_URMA_MSG_HELLO ||
	    hdr.length != sizeof(remote)) {
		return rc != 0 ? rc : -EPROTO;
	}
	if (nvmf_urma_read_full(uqpair->fd, &remote, sizeof(remote)) != 0 ||
	    remote.transport_mode != device->opts.transport_mode) {
		return -EPROTONOSUPPORT;
	}
	if (remote.max_queue_depth == 0 || remote.max_io_size == 0) {
		return -EPROTO;
	}
	uqpair->qpair.qid = hdr.qid;
	uqpair->qpair.sq_head_max = spdk_min(remote.max_queue_depth,
					      uqpair->transport->transport.opts.max_queue_depth) - 1;
	uqpair->max_io_size = spdk_min(device->opts.max_io_size, remote.max_io_size);
	rjetty.jetty_id.eid = remote.eid;
	rjetty.jetty_id.id = remote.jetty_id;
	rjetty.trans_mode = remote.transport_mode;
	rjetty.type = URMA_JETTY;
	rjetty.tp_type = URMA_CTP;
	{
		urma_token_t token = {.token = SPDK_URMA_DEFAULT_TOKEN};
		uqpair->target_jetty = urma_import_jetty(device->context, &rjetty, &token);
	}
	if (uqpair->target_jetty == NULL) {
		return -EIO;
	}
	if (device->opts.transport_mode == URMA_TM_RC) {
		urma_status_t status = urma_bind_jetty(uqpair->jetty, uqpair->target_jetty);

		if (status != URMA_SUCCESS && status != URMA_EEXIST) {
			return -EIO;
		}
	}
	hdr.type = SPDK_URMA_MSG_HELLO_RSP;
	hdr.length = sizeof(local);
	local.eid = device->eid;
	local.jetty_id = uqpair->jetty->jetty_id.id;
	local.transport_mode = device->opts.transport_mode;
	local.max_queue_depth = uqpair->transport->transport.opts.max_queue_depth;
	local.max_io_size = uqpair->transport->transport.opts.max_io_size;
	rc = nvmf_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	return rc == 0 ? nvmf_urma_write_full(uqpair->fd, &local, sizeof(local)) : rc;
}

static int
nvmf_urma_get_socket_addresses(int fd, struct nvmf_urma_qpair *uqpair)
{
	struct sockaddr_storage peer = {}, local = {};
	socklen_t peer_len = sizeof(peer), local_len = sizeof(local);
	char service[NI_MAXSERV];

	if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0 ||
	    getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
	    getnameinfo((struct sockaddr *)&peer, peer_len, uqpair->peer_addr,
			sizeof(uqpair->peer_addr), NULL, 0, NI_NUMERICHOST) != 0 ||
	    getnameinfo((struct sockaddr *)&local, local_len, uqpair->local_addr,
			sizeof(uqpair->local_addr), service, sizeof(service),
			NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
		return -errno;
	}
	snprintf(uqpair->service, sizeof(uqpair->service), "%s", service);
	return 0;
}

static int
nvmf_urma_accept(void *arg)
{
	struct nvmf_urma_transport *transport = arg;
	struct nvmf_urma_port *port;
	int accepted = 0;

	TAILQ_FOREACH(port, &transport->ports, link) {
		while (accepted < 16) {
			struct nvmf_urma_qpair *uqpair;
			int fd = accept4(port->fd, NULL, NULL, SOCK_CLOEXEC);
			if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				break;
			}
			if (fd < 0) {
				break;
			}
			uqpair = calloc(1, sizeof(*uqpair));
			if (uqpair == NULL) {
				close(fd);
				break;
			}
			uqpair->fd = fd;
			uqpair->transport = transport;
			uqpair->qpair.transport = &transport->transport;
			uqpair->qpair.state = SPDK_NVMF_QPAIR_UNINITIALIZED;
			TAILQ_INIT(&uqpair->free_reqs);
			TAILQ_INIT(&uqpair->working_reqs);
			/* Modified by Yin: 拆分 accept 复合条件为独立 rc，逐阶段打错误日志便于定位 */
			int rc_dev = spdk_urma_device_open(&transport->urma_opts, &uqpair->device);
			int rc_addr = rc_dev ? -1 : nvmf_urma_get_socket_addresses(fd, uqpair);
			int rc_jetty = rc_addr ? -1 : nvmf_urma_create_jetty(uqpair);
			int rc_hs = rc_jetty ? -1 : nvmf_urma_handshake(uqpair);
			if (rc_dev != 0) {
				SPDK_ERRLOG("accept: spdk_urma_device_open failed rc=%d\n", rc_dev);
			} else if (rc_addr != 0) {
				SPDK_ERRLOG("accept: get_socket_addresses failed rc=%d\n", rc_addr);
			} else if (rc_jetty != 0) {
				SPDK_ERRLOG("accept: create_jetty failed rc=%d\n", rc_jetty);
			} else if (rc_hs != 0) {
				SPDK_ERRLOG("accept: handshake failed rc=%d\n", rc_hs);
			}
			if (rc_dev != 0 || rc_addr != 0 || rc_jetty != 0 || rc_hs != 0) {
				if (uqpair->target_jetty != NULL) {
					urma_unimport_jetty(uqpair->target_jetty);
				}
				if (uqpair->jetty != NULL) {
					urma_delete_jetty(uqpair->jetty);
				}
				spdk_urma_device_close(uqpair->device);
				close(fd);
				free(uqpair);
				continue;
			}
			spdk_nvmf_tgt_new_qpair(transport->transport.tgt, &uqpair->qpair);
			accepted++;
		}
	}
	return accepted != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_urma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth = 128;
	opts->max_qpairs_per_ctrlr = 128;
	opts->in_capsule_data_size = 0;
	opts->max_io_size = 131072;
	opts->io_unit_size = 131072;
	opts->max_aq_depth = 128;
	opts->iobuf_small_cache_size = UINT32_MAX;
	opts->iobuf_large_cache_size = UINT32_MAX;
	opts->abort_timeout_sec = 1;
	opts->transport_specific = NULL;
}

static struct spdk_nvmf_transport *
nvmf_urma_create(struct spdk_nvmf_transport_opts *opts)
{
	struct nvmf_urma_transport *transport = calloc(1, sizeof(*transport));
	struct nvmf_urma_json_opts json_opts = {};

	if (transport == NULL) {
		return NULL;
	}
	transport->transport.opts = *opts;
	spdk_urma_opts_init(&transport->urma_opts);
	json_opts.active_port = transport->urma_opts.active_port;
	json_opts.eid_index = transport->urma_opts.eid_index;
	json_opts.jfc_count = transport->urma_opts.jfc_count;
	json_opts.jfc_depth = transport->urma_opts.jfc_depth;
	json_opts.jetty_count = transport->urma_opts.jetty_count;
	json_opts.jetty_depth = transport->urma_opts.jetty_depth;
	json_opts.bonding_balance = transport->urma_opts.bonding_balance;
	json_opts.bonding_multipath = transport->urma_opts.bonding_multipath;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, g_urma_opts_decoder,
					    SPDK_COUNTOF(g_urma_opts_decoder), &json_opts) != 0) {
		free(json_opts.dev_name);
		free(json_opts.trans_mode);
		free(transport);
		return NULL;
	}
	if (json_opts.dev_name != NULL) {
		snprintf(transport->urma_opts.dev_name, sizeof(transport->urma_opts.dev_name),
			 "%s", json_opts.dev_name);
	}
	if (json_opts.trans_mode != NULL) {
		if (strcasecmp(json_opts.trans_mode, "RC") == 0) {
			transport->urma_opts.transport_mode = URMA_TM_RC;
		} else if (strcasecmp(json_opts.trans_mode, "UM") == 0) {
			transport->urma_opts.transport_mode = URMA_TM_UM;
		} else if (strcasecmp(json_opts.trans_mode, "RM") == 0) {
			transport->urma_opts.transport_mode = URMA_TM_RM;
		} else {
			free(json_opts.dev_name);
			free(json_opts.trans_mode);
			free(transport);
			return NULL;
		}
	}
	transport->urma_opts.active_port = json_opts.active_port;
	transport->urma_opts.eid_index = json_opts.eid_index;
	transport->urma_opts.jfc_count = json_opts.jfc_count;
	transport->urma_opts.jfc_depth = json_opts.jfc_depth;
	transport->urma_opts.jetty_count = json_opts.jetty_count;
	transport->urma_opts.jetty_depth = json_opts.jetty_depth;
	transport->urma_opts.bonding_balance = json_opts.bonding_balance;
	transport->urma_opts.bonding_multipath = json_opts.bonding_multipath;
	free(json_opts.dev_name);
	free(json_opts.trans_mode);
	transport->urma_opts.max_io_size = opts->max_io_size;
	if (spdk_urma_device_open(&transport->urma_opts, &transport->device) != 0) {
		free(transport);
		return NULL;
	}
	TAILQ_INIT(&transport->ports);
	TAILQ_INIT(&transport->poll_groups);
	transport->accept_poller = SPDK_POLLER_REGISTER(nvmf_urma_accept, transport, 1000);
	return &transport->transport;
}

static void
nvmf_urma_dump_opts(struct spdk_nvmf_transport *base, struct spdk_json_write_ctx *w)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	const char *mode = transport->urma_opts.transport_mode == URMA_TM_RC ? "RC" :
			   transport->urma_opts.transport_mode == URMA_TM_UM ? "UM" : "RM";

	spdk_json_write_named_string(w, "dev_name", transport->device->context->dev->name);
	spdk_json_write_named_string(w, "trans_mode", mode);
	spdk_json_write_named_int32(w, "active_port", transport->device->active_port);
	spdk_json_write_named_uint32(w, "jfc_count", transport->device->jfc_count);
	spdk_json_write_named_uint32(w, "jfc_depth", transport->urma_opts.jfc_depth);
	spdk_json_write_named_uint32(w, "jetty_count", transport->urma_opts.jetty_count);
	spdk_json_write_named_bool(w, "bonding_balance", transport->urma_opts.bonding_balance);
	spdk_json_write_named_bool(w, "bonding_multipath", transport->urma_opts.bonding_multipath);
}

static void
nvmf_urma_destroy(struct spdk_nvmf_transport *base,
		   spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_port *port, *tmp;

	spdk_poller_unregister(&transport->accept_poller);
	TAILQ_FOREACH_SAFE(port, &transport->ports, link, tmp) {
		TAILQ_REMOVE(&transport->ports, port, link);
		close(port->fd);
		free(port);
	}
	spdk_urma_device_close(transport->device);
	free(transport);
	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

static int
nvmf_urma_listen(struct spdk_nvmf_transport *base, const struct spdk_nvme_transport_id *trid,
		  struct spdk_nvmf_listen_opts *opts)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_port *port;
	struct addrinfo hints = {}, *result = NULL, *it;
	int rc = -EINVAL, one = 1;

	hints.ai_family = trid->adrfam == SPDK_NVMF_ADRFAM_IPV6 ? AF_INET6 : AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(trid->traddr[0] == '\0' ? NULL : trid->traddr, trid->trsvcid,
			&hints, &result) != 0) {
		return -EINVAL;
	}
	port = calloc(1, sizeof(*port));
	if (port == NULL) {
		freeaddrinfo(result);
		return -ENOMEM;
	}
	port->fd = -1;
	for (it = result; it != NULL; it = it->ai_next) {
		port->fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
				  it->ai_protocol);
		if (port->fd < 0) {
			continue;
		}
		setsockopt(port->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(port->fd, it->ai_addr, it->ai_addrlen) == 0 && listen(port->fd, 512) == 0) {
			rc = 0;
			break;
		}
		close(port->fd);
		port->fd = -1;
	}
	freeaddrinfo(result);
	if (rc != 0) {
		free(port);
		return rc;
	}
	port->trid = *trid;
	TAILQ_INSERT_TAIL(&transport->ports, port, link);
	return 0;
}

static void
nvmf_urma_stop_listen(struct spdk_nvmf_transport *base, const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_port *port, *tmp;
	TAILQ_FOREACH_SAFE(port, &transport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(&port->trid, trid) == 0) {
			TAILQ_REMOVE(&transport->ports, port, link);
			close(port->fd);
			free(port);
			return;
		}
	}
}

static struct spdk_nvmf_transport_poll_group *
nvmf_urma_poll_group_create(struct spdk_nvmf_transport *base, struct spdk_nvmf_poll_group *group)
{
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	struct nvmf_urma_poll_group *ugroup = calloc(1, sizeof(*ugroup));
	if (ugroup == NULL) {
		return NULL;
	}
	ugroup->group.transport = base;
	ugroup->group.group = group;
	TAILQ_INIT(&ugroup->qpairs);
	TAILQ_INSERT_TAIL(&transport->poll_groups, ugroup, link);
	return &ugroup->group;
}

static void
nvmf_urma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *base)
{
	struct nvmf_urma_poll_group *group = SPDK_CONTAINEROF(base, struct nvmf_urma_poll_group, group);
	struct nvmf_urma_transport *transport = SPDK_CONTAINEROF(base->transport,
							 struct nvmf_urma_transport, transport);
	TAILQ_REMOVE(&transport->poll_groups, group, link);
	free(group);
}

static int
nvmf_urma_poll_group_add(struct spdk_nvmf_transport_poll_group *base, struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_urma_poll_group *group = SPDK_CONTAINEROF(base, struct nvmf_urma_poll_group, group);
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(qpair);

	uqpair->resource_count = base->transport->opts.max_queue_depth;
	uqpair->reqs = calloc(uqpair->resource_count, sizeof(*uqpair->reqs));
	if (uqpair->reqs == NULL) {
		return -ENOMEM;
	}
	for (uint32_t i = 0; i < uqpair->resource_count; i++) {
		struct nvmf_urma_req *ureq = &uqpair->reqs[i];
		ureq->req.qpair = qpair;
		ureq->req.cmd = &ureq->cmd;
		ureq->req.rsp = &ureq->rsp;
		TAILQ_INSERT_TAIL(&uqpair->free_reqs, ureq, link);
	}
	uqpair->group = group;
	TAILQ_INSERT_TAIL(&group->qpairs, uqpair, link);
	return 0;
}

static int
nvmf_urma_poll_group_remove(struct spdk_nvmf_transport_poll_group *base, struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(qpair);
	if (uqpair->group != NULL) {
		TAILQ_REMOVE(&uqpair->group->qpairs, uqpair, link);
		uqpair->group = NULL;
	}
	return 0;
}

static void nvmf_urma_release_req(struct nvmf_urma_req *ureq);

static int
nvmf_urma_send_response(struct nvmf_urma_req *ureq)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(ureq->req.qpair);
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_capsule_rsp rsp = {.cpl = ureq->rsp.nvme_cpl};
	int rc;

	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_CAPSULE_RSP;
	hdr.length = sizeof(rsp);
	hdr.qid = uqpair->qpair.qid;
	rc = nvmf_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc == 0) {
		rc = nvmf_urma_write_full(uqpair->fd, &rsp, sizeof(rsp));
	}
	nvmf_urma_release_req(ureq);
	return rc;
}

static int
nvmf_urma_post_data(struct nvmf_urma_req *ureq, bool push)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(ureq->req.qpair);
	struct spdk_urma_device *device = uqpair->device;
	urma_import_seg_flag_t import_flag = {};
	urma_token_t token = {.token = SPDK_URMA_DEFAULT_TOKEN};
	urma_sge_t local_sge = {}, remote_sge = {};
	urma_jfs_wr_t wr = {}, *bad_wr = NULL;
	int rc;

	if (ureq->req.iovcnt != 1) {
		return -ENOTSUP;
	}
	import_flag.bs.cacheable = URMA_NON_CACHEABLE;
	import_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
	import_flag.bs.mapping = URMA_SEG_NOMAP;
	ureq->remote_seg = urma_import_seg(device->context, &ureq->remote_data.seg,
					    &token, 0, import_flag);
	if (ureq->remote_seg == NULL) {
		return -EIO;
	}
	rc = spdk_nvme_urma_register_memory(device->context, ureq->req.iov[0].iov_base,
			ureq->req.length, SPDK_NVME_URMA_MEM_HOST, &ureq->local_region);
	if (rc != 0) {
		return rc;
	}
	local_sge.addr = (uint64_t)ureq->req.iov[0].iov_base;
	local_sge.len = ureq->req.length;
	local_sge.tseg = spdk_urma_memory_region_get_tseg(ureq->local_region);
	remote_sge.addr = ureq->remote_data.address;
	remote_sge.len = ureq->req.length;
	remote_sge.tseg = ureq->remote_seg;
	wr.user_ctx = (uint64_t)ureq;
	wr.opcode = push ? URMA_OPC_WRITE : URMA_OPC_READ;
	wr.rw.src.sge = push ? &local_sge : &remote_sge;
	wr.rw.src.num_sge = 1;
	wr.rw.dst.sge = push ? &remote_sge : &local_sge;
	wr.rw.dst.num_sge = 1;
	wr.flag.bs.complete_enable = 1;
	wr.tjetty = uqpair->target_jetty;
	ureq->state = push ? NVMF_URMA_REQ_PUSHING : NVMF_URMA_REQ_PULLING;
	return urma_post_jetty_send_wr(uqpair->jetty, &wr, &bad_wr) == URMA_SUCCESS ? 0 : -EIO;
}

static void
nvmf_urma_buffers_ready(struct nvmf_urma_req *ureq)
{
	if (ureq->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		if (nvmf_urma_post_data(ureq, false) != 0) {
			ureq->rsp.nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			ureq->rsp.nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			nvmf_urma_send_response(ureq);
		}
	} else {
		ureq->state = NVMF_URMA_REQ_EXECUTING;
		spdk_nvmf_request_exec(&ureq->req);
	}
}

static void
nvmf_urma_req_get_buffers_done(struct spdk_nvmf_request *req)
{
	nvmf_urma_buffers_ready(nvmf_urma_req(req));
}

static int
nvmf_urma_receive_capsule(struct nvmf_urma_qpair *uqpair)
{
	struct spdk_urma_msg_hdr hdr;
	struct spdk_urma_capsule_cmd capsule;
	struct nvmf_urma_req *ureq;
	ssize_t rc;

	rc = recv(uqpair->fd, &hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
	if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	}
	if (rc <= 0) {
		return -ECONNRESET;
	}
	if ((size_t)rc < sizeof(hdr)) {
		return 0;
	}
	{
		int available = 0;

		if (ioctl(uqpair->fd, FIONREAD, &available) != 0) {
			return -errno;
		}
		if ((size_t)available < sizeof(hdr) + hdr.length) {
			return 0;
		}
	}
	if (nvmf_urma_read_full(uqpair->fd, &hdr, sizeof(hdr)) != 0 ||
	    hdr.magic != SPDK_URMA_WIRE_MAGIC || hdr.version != SPDK_URMA_WIRE_VERSION ||
	    hdr.type != SPDK_URMA_MSG_CAPSULE_CMD || hdr.length != sizeof(capsule) ||
	    nvmf_urma_read_full(uqpair->fd, &capsule, sizeof(capsule)) != 0) {
		return -EPROTO;
	}
	if (capsule.data.length > uqpair->max_io_size) {
		return -EMSGSIZE;
	}
	ureq = TAILQ_FIRST(&uqpair->free_reqs);
	if (ureq == NULL) {
		return -ENOBUFS;
	}
	TAILQ_REMOVE(&uqpair->free_reqs, ureq, link);
	TAILQ_INSERT_TAIL(&uqpair->working_reqs, ureq, link);
	memset(&ureq->rsp, 0, sizeof(ureq->rsp));
	ureq->cmd.nvme_cmd = capsule.cmd;
	ureq->remote_data = capsule.data;
	ureq->req.raw = 0;
	ureq->req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;
	ureq->req.xfer = spdk_nvmf_req_get_xfer(&ureq->req);
	ureq->req.length = capsule.data.length;
	uqpair->qpair.queue_depth++;
	if (ureq->req.xfer == SPDK_NVME_DATA_NONE || ureq->req.length == 0) {
		ureq->state = NVMF_URMA_REQ_EXECUTING;
		spdk_nvmf_request_exec(&ureq->req);
		return 1;
	}
	ureq->state = NVMF_URMA_REQ_NEED_BUFFER;
	if (spdk_nvmf_request_get_buffers(&ureq->req, &uqpair->group->group,
					  &uqpair->transport->transport, ureq->req.length) == 0) {
		nvmf_urma_buffers_ready(ureq);
	}
	return 1;
}

static int
nvmf_urma_poll_group_poll(struct spdk_nvmf_transport_poll_group *base)
{
	struct nvmf_urma_poll_group *group = SPDK_CONTAINEROF(base, struct nvmf_urma_poll_group, group);
	struct nvmf_urma_qpair *uqpair;
	urma_cr_t completions[64];
	int total = 0;

	TAILQ_FOREACH(uqpair, &group->qpairs, link) {
		for (uint32_t j = 0; j < uqpair->device->jfc_count; j++) {
			int count = urma_poll_jfc(uqpair->device->jfcs[j], SPDK_COUNTOF(completions), completions);
			if (count < 0) {
				return -EIO;
			}
			for (int i = 0; i < count; i++) {
				struct nvmf_urma_req *ureq = (void *)completions[i].user_ctx;
				if (ureq == NULL) {
					continue;
				}
				if (completions[i].status != URMA_CR_SUCCESS) {
					/* Modified by Yin: 打印 JFC completion 错误（含 status=4 LOC_ACCESS_ERR），便于定位 */
					SPDK_ERRLOG("poll_group: completion error status=%d, ureq->state=%d, opcode=%d, user_ctx=%p\n",
						    completions[i].status, ureq->state,
						    ureq->state == NVMF_URMA_REQ_PULLING ? 1 : 0,
						    completions[i].user_ctx);
					ureq->rsp.nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
					ureq->rsp.nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					nvmf_urma_send_response(ureq);
				} else if (ureq->state == NVMF_URMA_REQ_PULLING) {
					ureq->state = NVMF_URMA_REQ_EXECUTING;
					spdk_nvmf_request_exec(&ureq->req);
				} else if (ureq->state == NVMF_URMA_REQ_PUSHING) {
					nvmf_urma_send_response(ureq);
				}
				total++;
			}
		}
		int rc = nvmf_urma_receive_capsule(uqpair);
		if (rc < 0) {
			uqpair->qpair.state = SPDK_NVMF_QPAIR_ERROR;
			continue;
		}
		total += rc;
	}
	return total;
}

static void
nvmf_urma_release_req(struct nvmf_urma_req *ureq)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(ureq->req.qpair);
	if (ureq->local_region != NULL) {
		spdk_nvme_urma_unregister_memory(ureq->local_region);
		ureq->local_region = NULL;
	}
	if (ureq->remote_seg != NULL) {
		urma_unimport_seg(ureq->remote_seg);
		ureq->remote_seg = NULL;
	}
	if (ureq->req.data_from_pool) {
		spdk_nvmf_request_free_buffers(&ureq->req, &uqpair->group->group,
					       &uqpair->transport->transport);
	}
	ureq->req.iovcnt = 0;
	ureq->state = NVMF_URMA_REQ_FREE;
	TAILQ_REMOVE(&uqpair->working_reqs, ureq, link);
	TAILQ_INSERT_TAIL(&uqpair->free_reqs, ureq, link);
	uqpair->qpair.queue_depth--;
}

static void
nvmf_urma_req_free(struct spdk_nvmf_request *req)
{
	nvmf_urma_release_req(nvmf_urma_req(req));
}

static void
nvmf_urma_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_urma_req *ureq = nvmf_urma_req(req);
	struct spdk_nvmf_qpair *qpair = req->qpair;

	ureq->rsp.nvme_cpl.cid = ureq->cmd.nvme_cmd.cid;
	ureq->rsp.nvme_cpl.sqid = qpair->qid;
	if (qpair->ctrlr == NULL || !qpair->ctrlr->sq_flow_control_disabled) {
		qpair->sq_head = qpair->sq_head == qpair->sq_head_max ? 0 : qpair->sq_head + 1;
		ureq->rsp.nvme_cpl.sqhd = qpair->sq_head;
	}
	if (spdk_nvme_cpl_is_success(&ureq->rsp.nvme_cpl) &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST && req->length != 0) {
		if (nvmf_urma_post_data(ureq, true) == 0) {
			return;
		}
		ureq->rsp.nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		ureq->rsp.nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}
	nvmf_urma_send_response(ureq);
}

static void
nvmf_urma_qpair_fini(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct nvmf_urma_qpair *uqpair = nvmf_urma_qpair(qpair);
	struct nvmf_urma_req *ureq, *tmp;
	TAILQ_FOREACH_SAFE(ureq, &uqpair->working_reqs, link, tmp) {
		nvmf_urma_release_req(ureq);
	}
	if (uqpair->target_jetty != NULL) {
		if (uqpair->device->opts.transport_mode == URMA_TM_RC) {
			urma_unbind_jetty(uqpair->jetty);
		}
		urma_unimport_jetty(uqpair->target_jetty);
	}
	if (uqpair->jetty != NULL) {
		urma_delete_jetty(uqpair->jetty);
	}
	spdk_urma_device_close(uqpair->device);
	if (uqpair->fd >= 0) {
		close(uqpair->fd);
	}
	free(uqpair->reqs);
	free(uqpair);
	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

static int
nvmf_urma_fill_trid(struct nvmf_urma_qpair *uqpair, struct spdk_nvme_transport_id *trid,
		    bool peer)
{
	memset(trid, 0, sizeof(*trid));
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_URMA);
	trid->adrfam = strchr(peer ? uqpair->peer_addr : uqpair->local_addr, ':') != NULL ?
			 SPDK_NVMF_ADRFAM_IPV6 : SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid->traddr, sizeof(trid->traddr), "%s", peer ? uqpair->peer_addr : uqpair->local_addr);
	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", uqpair->service);
	return 0;
}

static int nvmf_urma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid)
{
	return nvmf_urma_fill_trid(nvmf_urma_qpair(qpair), trid, true);
}
static int nvmf_urma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid)
{
	return nvmf_urma_fill_trid(nvmf_urma_qpair(qpair), trid, false);
}
static int nvmf_urma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid)
{
	return nvmf_urma_fill_trid(nvmf_urma_qpair(qpair), trid, false);
}

static void
nvmf_urma_discover(struct spdk_nvmf_transport *transport,
		    struct spdk_nvme_transport_id *trid,
		    struct spdk_nvmf_discovery_log_page_entry *entry)
{
	/* URMA does not have an assigned 8-bit NVMe-oF TRTYPE yet.  Use the
	 * vendor-specific/reserved value until the standards allocation lands. */
	entry->trtype = 0xff;
	entry->adrfam = trid->adrfam;
	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');
	memcpy(entry->tsas.raw, "URMA", 4);
}
static void nvmf_urma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvmf_request *req)
{
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	spdk_nvmf_request_complete(req);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_urma = {
	.name = "URMA",
	.type = SPDK_NVME_TRANSPORT_URMA,
	.opts_init = nvmf_urma_opts_init,
	.create = nvmf_urma_create,
	.dump_opts = nvmf_urma_dump_opts,
	.destroy = nvmf_urma_destroy,
	.listen = nvmf_urma_listen,
	.stop_listen = nvmf_urma_stop_listen,
	.listener_discover = nvmf_urma_discover,
	.poll_group_create = nvmf_urma_poll_group_create,
	.poll_group_destroy = nvmf_urma_poll_group_destroy,
	.poll_group_add = nvmf_urma_poll_group_add,
	.poll_group_remove = nvmf_urma_poll_group_remove,
	.poll_group_poll = nvmf_urma_poll_group_poll,
	.req_free = nvmf_urma_req_free,
	.req_complete = nvmf_urma_req_complete,
	.req_get_buffers_done = nvmf_urma_req_get_buffers_done,
	.qpair_fini = nvmf_urma_qpair_fini,
	.qpair_get_local_trid = nvmf_urma_qpair_get_local_trid,
	.qpair_get_peer_trid = nvmf_urma_qpair_get_peer_trid,
	.qpair_get_listen_trid = nvmf_urma_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_urma_qpair_abort_request,
};

SPDK_NVMF_TRANSPORT_REGISTER(urma, &spdk_nvmf_transport_urma);
SPDK_LOG_REGISTER_COMPONENT(nvmf_urma)
