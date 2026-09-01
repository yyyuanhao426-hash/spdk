/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

/* Experimental independent NVMe/URMA initiator transport.  TCP is used only
 * for bootstrap and NVMe capsule exchange; payload bytes move through URMA. */

#include "nvme_internal.h"
#include "nvme_urma_internal.h"

#include "spdk/dma.h"
#include "spdk/nvmf.h"

#include <netdb.h>
#include <sys/ioctl.h>

struct nvme_urma_req {
	struct nvme_request *req;
	struct spdk_nvme_urma_memory_region *region;
	TAILQ_ENTRY(nvme_urma_req) link;
};

struct nvme_urma_qpair {
	struct spdk_nvme_qpair qpair;
	struct spdk_urma_device *device;
	urma_jetty_t *jetty;
	urma_target_jetty_t *target_jetty;
	int fd;
	uint32_t num_entries;
	/* Modified by Yin: 新增 transport-level CID 计数器（submit 时分配唯一 cid） */
	uint16_t next_cid;
	TAILQ_HEAD(, nvme_urma_req) outstanding;
};

struct nvme_urma_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_urma_transport_opts opts;
};

struct nvme_urma_poll_group {
	struct spdk_nvme_transport_poll_group group;
};

static inline struct nvme_urma_qpair *
nvme_urma_qpair(struct spdk_nvme_qpair *qpair)
{
	return SPDK_CONTAINEROF(qpair, struct nvme_urma_qpair, qpair);
}

static int
nvme_urma_write_full(int fd, const void *buf, size_t length)
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
nvme_urma_read_full(int fd, void *buf, size_t length)
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
nvme_urma_connect_socket(const struct spdk_nvme_transport_id *trid)
{
	struct addrinfo hints = {}, *result = NULL, *it;
	int fd = -1, rc;

	hints.ai_family = trid->adrfam == SPDK_NVMF_ADRFAM_IPV6 ? AF_INET6 : AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(trid->traddr, trid->trsvcid, &hints, &result);
	if (rc != 0) {
		return -EINVAL;
	}
	for (it = result; it != NULL; it = it->ai_next) {
		fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
		if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
			break;
		}
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(result);
	return fd < 0 ? -errno : fd;
}

static int
nvme_urma_create_jetty(struct nvme_urma_qpair *uqpair)
{
	urma_jfs_cfg_t jfs = {};
	urma_jetty_cfg_t cfg = {};

	jfs.depth = uqpair->device->opts.jetty_depth;
	jfs.trans_mode = uqpair->device->opts.transport_mode;
	jfs.priority = SPDK_URMA_DEFAULT_PRIORITY;
	jfs.max_sge = SPDK_URMA_DEFAULT_MAX_SGE;
	jfs.rnr_retry = SPDK_URMA_DEFAULT_RNR_RETRY;
	jfs.err_timeout = SPDK_URMA_DEFAULT_ERR_TIMEOUT;
	jfs.jfc = uqpair->device->jfcs[0];
	/* Modified by Yin: UB transport 强制 share_jfr=1，改用 device 预建的共享 jfr */
	cfg.jfs_cfg = jfs;
	cfg.flag.bs.share_jfr = 1;
	cfg.shared.jfr = uqpair->device->jfr;
	cfg.shared.jfc = uqpair->device->jfcs[0];
	uqpair->jetty = urma_create_jetty(uqpair->device->context, &cfg);
	return uqpair->jetty == NULL ? -EIO : 0;
}

static int
nvme_urma_exchange_hello(struct nvme_urma_qpair *uqpair)
{
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_endpoint_desc local = {}, remote = {};
	urma_rjetty_t rjetty = {};
	int rc;

	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_HELLO;
	hdr.length = sizeof(local);
	hdr.qid = uqpair->qpair.id;
	local.eid = uqpair->device->eid;
	local.jetty_id = uqpair->jetty->jetty_id.id;
	local.transport_mode = uqpair->device->opts.transport_mode;
	local.max_queue_depth = uqpair->num_entries + 1;
	local.max_io_size = uqpair->device->opts.max_io_size;
	rc = nvme_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc == 0) {
		rc = nvme_urma_write_full(uqpair->fd, &local, sizeof(local));
	}
	if (rc == 0) {
		rc = nvme_urma_read_full(uqpair->fd, &hdr, sizeof(hdr));
	}
	if (rc == 0 && (hdr.magic != SPDK_URMA_WIRE_MAGIC ||
			 hdr.version != SPDK_URMA_WIRE_VERSION ||
			 hdr.type != SPDK_URMA_MSG_HELLO_RSP || hdr.length != sizeof(remote))) {
		rc = -EPROTO;
	}
	if (rc == 0) {
		rc = nvme_urma_read_full(uqpair->fd, &remote, sizeof(remote));
	}
	if (rc != 0 || remote.transport_mode != uqpair->device->opts.transport_mode) {
		return rc != 0 ? rc : -EPROTONOSUPPORT;
	}
	if (remote.max_queue_depth < 2 || remote.max_io_size == 0) {
		return -EPROTO;
	}
	uqpair->num_entries = spdk_min(uqpair->num_entries, remote.max_queue_depth - 1);
	uqpair->device->opts.max_io_size = spdk_min(uqpair->device->opts.max_io_size,
					 remote.max_io_size);
	rjetty.jetty_id.eid = remote.eid;
	rjetty.jetty_id.id = remote.jetty_id;
	rjetty.trans_mode = remote.transport_mode;
	rjetty.type = URMA_JETTY;
	rjetty.tp_type = URMA_CTP;
	{
		urma_token_t token = {.token = SPDK_URMA_DEFAULT_TOKEN};
		uqpair->target_jetty = urma_import_jetty(uqpair->device->context, &rjetty, &token);
	}
	if (uqpair->target_jetty == NULL) {
		return -EIO;
	}
	if (uqpair->device->opts.transport_mode == URMA_TM_RC) {
		urma_status_t status = urma_bind_jetty(uqpair->jetty, uqpair->target_jetty);

		if (status != URMA_SUCCESS && status != URMA_EEXIST) {
			urma_unimport_jetty(uqpair->target_jetty);
			uqpair->target_jetty = NULL;
			return -EIO;
		}
	}
	return 0;
}

static enum spdk_nvme_urma_memory_type
nvme_urma_req_memory_type(const struct nvme_request *req)
{
	const char *id;

	if (req->payload.opts == NULL || req->payload.opts->memory_domain == NULL ||
	    req->payload.opts->memory_domain == spdk_memory_domain_get_system_domain()) {
		return SPDK_NVME_URMA_MEM_HOST;
	}
	id = spdk_memory_domain_get_dma_device_id(req->payload.opts->memory_domain);
	if (id != NULL && strcasestr(id, "cuda") != NULL) {
		return SPDK_NVME_URMA_MEM_CUDA;
	}
	if (id != NULL && strcasestr(id, "rocm") != NULL) {
		return SPDK_NVME_URMA_MEM_ROCM;
	}
	if (id != NULL && (strcasestr(id, "npu") != NULL || strcasestr(id, "ascend") != NULL)) {
		return SPDK_NVME_URMA_MEM_NPU;
	}
	return SPDK_NVME_URMA_MEM_XDS;
}

static int
nvme_urma_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	struct nvme_urma_req *ureq;
	struct spdk_urma_msg_hdr hdr = {};
	struct spdk_urma_capsule_cmd capsule = {};
	void *addr = NULL;
	int rc;

	if (qpair->queue_depth >= uqpair->num_entries) {
		return -EAGAIN;
	}
	if (req->payload.size != 0 && nvme_req_payload_type(req) != NVME_PAYLOAD_TYPE_CONTIG) {
		return -ENOTSUP;
	}
	ureq = calloc(1, sizeof(*ureq));
	if (ureq == NULL) {
		return -ENOMEM;
	}
	ureq->req = req;
	/* Modified by Yin: submit 前分配唯一 cid，防 completion 匹配到错误 request */
	req->cmd.cid = uqpair->next_cid++;
	if (uqpair->next_cid >= uqpair->num_entries) {
		uqpair->next_cid = 0;
	}
	capsule.cmd = req->cmd;
	if (req->payload.size != 0) {
		capsule.cmd.dptr.sgl1.address = 0;
		capsule.cmd.dptr.sgl1.unkeyed.length = req->payload.size;
		capsule.cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
		capsule.cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
		addr = (void *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload.offset);
		rc = spdk_nvme_urma_register_memory(uqpair->device->context, addr,
				req->payload.size, nvme_urma_req_memory_type(req), &ureq->region);
		if (rc != 0) {
			free(ureq);
			return rc;
		}
		capsule.data.seg = spdk_urma_memory_region_get_tseg(ureq->region)->seg;
		capsule.data.address = (uint64_t)addr;
		capsule.data.length = req->payload.size;
	}
	hdr.magic = SPDK_URMA_WIRE_MAGIC;
	hdr.version = SPDK_URMA_WIRE_VERSION;
	hdr.type = SPDK_URMA_MSG_CAPSULE_CMD;
	hdr.length = sizeof(capsule);
	hdr.qid = qpair->id;
	rc = nvme_urma_write_full(uqpair->fd, &hdr, sizeof(hdr));
	if (rc == 0) {
		rc = nvme_urma_write_full(uqpair->fd, &capsule, sizeof(capsule));
	}
	if (rc != 0) {
		spdk_nvme_urma_unregister_memory(ureq->region);
		free(ureq);
		return rc;
	}
	TAILQ_INSERT_TAIL(&uqpair->outstanding, ureq, link);
	qpair->queue_depth++;
	return 0;
}

static int32_t
nvme_urma_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	uint32_t completed = 0;

	if (max_completions == 0) {
		max_completions = UINT32_MAX;
	}
	while (completed < max_completions) {
		struct spdk_urma_msg_hdr hdr;
		struct spdk_urma_capsule_rsp rsp;
		struct nvme_urma_req *ureq;
		ssize_t rc = recv(uqpair->fd, &hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}
		if (rc == 0) {
			return -ECONNRESET;
		}
		if (rc < 0) {
			return -errno;
		}
		if ((size_t)rc < sizeof(hdr)) {
			break;
		}
		{
			int available = 0;

			if (ioctl(uqpair->fd, FIONREAD, &available) != 0) {
				return -errno;
			}
			if ((size_t)available < sizeof(hdr) + hdr.length) {
				break;
			}
		}
		if (nvme_urma_read_full(uqpair->fd, &hdr, sizeof(hdr)) != 0 ||
		    hdr.magic != SPDK_URMA_WIRE_MAGIC || hdr.type != SPDK_URMA_MSG_CAPSULE_RSP ||
		    hdr.length != sizeof(rsp) || nvme_urma_read_full(uqpair->fd, &rsp, sizeof(rsp)) != 0) {
			return -EPROTO;
		}
		TAILQ_FOREACH(ureq, &uqpair->outstanding, link) {
			if (ureq->req->cmd.cid == rsp.cpl.cid) {
				break;
			}
		}
		if (ureq == NULL) {
			/* Modified by Yin: 诊断用：completion 的 cid 无匹配 outstanding request */
			SPDK_ERRLOG("process_completions: no matching ureq for cid=%u\n",
				    rsp.cpl.cid);
			return -EPROTO;
		}
		TAILQ_REMOVE(&uqpair->outstanding, ureq, link);
		qpair->queue_depth--;
		spdk_nvme_urma_unregister_memory(ureq->region);
		nvme_complete_request(ureq->req->cb_fn, ureq->req->cb_arg, qpair, ureq->req, &rsp.cpl);
		free(ureq);
		completed++;
	}
	if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
		/* Modified by Yin: 加 in_connect_poll 守卫，切断 connect_poll 互递归 */
		if (qpair->in_connect_poll) {
			return completed;
		}
		qpair->in_connect_poll = true;
		int rc = nvme_fabric_qpair_connect_poll(qpair);
		qpair->in_connect_poll = false;
		if (rc == 0) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (rc != -EAGAIN) {
			return rc;
		}
	}
	return completed;
}

static void
nvme_urma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	struct nvme_urma_req *ureq, *tmp;
	struct spdk_nvme_cpl cpl = {};

	cpl.sqid = qpair->id;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.dnr = dnr;
	TAILQ_FOREACH_SAFE(ureq, &uqpair->outstanding, link, tmp) {
		TAILQ_REMOVE(&uqpair->outstanding, ureq, link);
		spdk_nvme_urma_unregister_memory(ureq->region);
		nvme_complete_request(ureq->req->cb_fn, ureq->req->cb_arg, qpair, ureq->req, &cpl);
		free(ureq);
	}
	qpair->queue_depth = 0;
}

static struct spdk_nvme_qpair *
nvme_urma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid, uint32_t qsize,
			     enum spdk_nvme_qprio qprio, uint32_t requests, bool async)
{
	struct nvme_urma_qpair *uqpair = calloc(1, sizeof(*uqpair));

	if (uqpair == NULL) {
		return NULL;
	}
	uqpair->fd = -1;
	uqpair->num_entries = qsize - 1;
	TAILQ_INIT(&uqpair->outstanding);
	if (nvme_qpair_init(&uqpair->qpair, qid, ctrlr, qprio, requests, async) != 0) {
		free(uqpair);
		return NULL;
	}
	return &uqpair->qpair;
}

static struct spdk_nvme_qpair *
nvme_urma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_urma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests, opts->async_mode);
}

static void
nvme_urma_qpair_release_transport(struct nvme_urma_qpair *uqpair)
{
	if (uqpair->target_jetty != NULL) {
		if (uqpair->device != NULL && uqpair->device->opts.transport_mode == URMA_TM_RC) {
			urma_unbind_jetty(uqpair->jetty);
		}
		urma_unimport_jetty(uqpair->target_jetty);
		uqpair->target_jetty = NULL;
	}
	if (uqpair->jetty != NULL) {
		urma_delete_jetty(uqpair->jetty);
		uqpair->jetty = NULL;
	}
	if (uqpair->fd >= 0) {
		close(uqpair->fd);
	}
	uqpair->fd = -1;
	spdk_urma_device_close(uqpair->device);
	uqpair->device = NULL;
}

static int
nvme_urma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_urma_ctrlr *uctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_urma_ctrlr, ctrlr);
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);
	int rc;

	rc = spdk_urma_device_open(&uctrlr->opts, &uqpair->device);
	if (rc != 0) {
		return rc;
	}
	rc = nvme_urma_create_jetty(uqpair);
	if (rc != 0) {
		goto fail;
	}
	uqpair->fd = nvme_urma_connect_socket(&ctrlr->trid);
	if (uqpair->fd < 0) {
		rc = uqpair->fd;
		goto fail;
	}
	rc = nvme_urma_exchange_hello(uqpair);
	if (rc != 0) {
		goto fail;
	}
	uctrlr->opts.max_io_size = spdk_min(uctrlr->opts.max_io_size,
					 uqpair->device->opts.max_io_size);
	rc = nvme_fabric_qpair_connect_async(qpair, uqpair->num_entries + 1);
	if (rc < 0) {
		goto fail;
	}
	return 0;

fail:
	nvme_urma_qpair_release_transport(uqpair);
	return rc;
}

static void
nvme_urma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);

	nvme_urma_qpair_abort_reqs(qpair, qpair->abort_dnr);
	if (uqpair->fd >= 0) {
		close(uqpair->fd);
		uqpair->fd = -1;
	}
	nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
	nvme_transport_ctrlr_disconnect_qpair_done(qpair);
}

static int
nvme_urma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_urma_qpair *uqpair = nvme_urma_qpair(qpair);

	nvme_urma_qpair_abort_reqs(qpair, qpair->abort_dnr);
	nvme_urma_qpair_release_transport(uqpair);
	nvme_qpair_deinit(qpair);
	free(uqpair);
	return 0;
}

static struct spdk_nvme_ctrlr *
nvme_urma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts, void *devhandle)
{
	struct nvme_urma_ctrlr *uctrlr = calloc(1, sizeof(*uctrlr));

	if (uctrlr == NULL) {
		return NULL;
	}
	uctrlr->ctrlr.opts = *opts;
	uctrlr->ctrlr.trid = *trid;
	spdk_urma_opts_init(&uctrlr->opts);
	if (nvme_ctrlr_construct(&uctrlr->ctrlr) != 0) {
		free(uctrlr);
		return NULL;
	}
	uctrlr->ctrlr.adminq = nvme_urma_ctrlr_create_qpair(&uctrlr->ctrlr, 0,
				       opts->admin_queue_size, 0, opts->admin_queue_size, true);
	if (uctrlr->ctrlr.adminq == NULL) {
		nvme_ctrlr_destruct_finish(&uctrlr->ctrlr);
		free(uctrlr);
		return NULL;
	}
	if (nvme_ctrlr_add_process(&uctrlr->ctrlr, 0) != 0) {
		nvme_urma_ctrlr_delete_io_qpair(&uctrlr->ctrlr, uctrlr->ctrlr.adminq);
		uctrlr->ctrlr.adminq = NULL;
		nvme_ctrlr_destruct_finish(&uctrlr->ctrlr);
		free(uctrlr);
		return NULL;
	}
	return &uctrlr->ctrlr;
}

static int
nvme_urma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_urma_ctrlr *uctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_urma_ctrlr, ctrlr);

	if (ctrlr->adminq != NULL) {
		nvme_urma_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}
	nvme_ctrlr_destruct_finish(ctrlr);
	free(uctrlr);
	return 0;
}

static int nvme_urma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr) { return 0; }
static int nvme_urma_qpair_reset(struct spdk_nvme_qpair *qpair) { return 0; }
static uint32_t nvme_urma_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return SPDK_CONTAINEROF(ctrlr, struct nvme_urma_ctrlr, ctrlr)->opts.max_io_size;
}
static uint16_t nvme_urma_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr) { return 1; }

static int
nvme_urma_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				 int (*iter_fn)(struct nvme_request *, void *), void *arg)
{
	struct nvme_urma_req *ureq;
	TAILQ_FOREACH(ureq, &nvme_urma_qpair(qpair)->outstanding, link) {
		int rc = iter_fn(ureq->req, arg);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int nvme_urma_qpair_authenticate(struct spdk_nvme_qpair *qpair) { return -ENOTSUP; }
static void nvme_urma_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	nvme_urma_qpair_abort_reqs(qpair, 0);
}

static struct spdk_nvme_transport_poll_group *
nvme_urma_poll_group_create(void)
{
	return calloc(1, sizeof(struct nvme_urma_poll_group));
}

static int64_t
nvme_urma_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair;
	int64_t total = 0;
	STAILQ_FOREACH(qpair, &tgroup->connected_qpairs, poll_group_stailq) {
		int rc = nvme_urma_qpair_process_completions(qpair, completions_per_qpair);
		if (rc < 0) {
			return rc;
		}
		total += rc;
	}
	return total;
}

static void
nvme_urma_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb cb)
{
	struct spdk_nvme_qpair *qpair;
	STAILQ_FOREACH(qpair, &tgroup->disconnected_qpairs, poll_group_stailq) {
		cb(qpair, tgroup->group->ctx);
	}
}

static int nvme_urma_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	free(tgroup);
	return 0;
}

static int
nvme_urma_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	if (domains != NULL && array_size > 0) {
		const struct nvme_urma_qpair *uqpair = nvme_urma_qpair(ctrlr->adminq);
		domains[0] = uqpair->device != NULL ? uqpair->device->memory_domain :
			     spdk_memory_domain_get_system_domain();
	}
	return 1;
}

const struct spdk_nvme_transport_ops urma_ops = {
	.name = "URMA",
	.type = SPDK_NVME_TRANSPORT_URMA,
	.ctrlr_construct = nvme_urma_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_urma_ctrlr_destruct,
	.ctrlr_enable = nvme_urma_ctrlr_enable,
	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,
	.ctrlr_get_max_xfer_size = nvme_urma_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_urma_ctrlr_get_max_sges,
	.ctrlr_create_io_qpair = nvme_urma_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_urma_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_urma_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_urma_ctrlr_disconnect_qpair,
	.ctrlr_get_memory_domains = nvme_urma_ctrlr_get_memory_domains,
	.qpair_abort_reqs = nvme_urma_qpair_abort_reqs,
	.qpair_reset = nvme_urma_qpair_reset,
	.qpair_submit_request = nvme_urma_qpair_submit_request,
	.qpair_process_completions = nvme_urma_qpair_process_completions,
	.qpair_iterate_requests = nvme_urma_qpair_iterate_requests,
	.qpair_authenticate = nvme_urma_qpair_authenticate,
	.admin_qpair_abort_aers = nvme_urma_admin_qpair_abort_aers,
	.poll_group_create = nvme_urma_poll_group_create,
	.poll_group_process_completions = nvme_urma_poll_group_process_completions,
	.poll_group_check_disconnected_qpairs = nvme_urma_poll_group_check_disconnected_qpairs,
	.poll_group_destroy = nvme_urma_poll_group_destroy,
};

SPDK_NVME_TRANSPORT_REGISTER(urma, &urma_ops);
SPDK_LOG_REGISTER_COMPONENT(nvme_urma)
