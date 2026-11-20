/* SPDX-License-Identifier: BSD-3-Clause */
/* Real initiator/target HELLO functions are inserted by handshake_build.py. */
#include "nvme_urma_internal.h"
#include "spdk/config.h"
#include <sys/wait.h>

struct nvmf_urma_transport {
	struct {
		struct {
			uint32_t max_queue_depth, max_io_size;
		} opts;
	} transport;
	struct {
		uint32_t max_io_size;
	} urma_opts;
};
struct nvmf_urma_qpair {
	struct spdk_urma_device *device;
	struct spdk_urma_msg_hdr hs_hdr;
	struct spdk_urma_endpoint_desc hs_remote;
	struct {
		struct spdk_urma_msg_hdr hdr;
		struct spdk_urma_endpoint_desc local;
	} hs_rsp;
	struct {
		uint32_t qid, sq_head_max;
	} qpair;
	struct nvmf_urma_transport *transport;
	urma_jfr_t **jfrs;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	uint32_t jetty_count, max_io_size;
	enum spdk_urma_capsule_transport capsule_transport;
	int fd;
	size_t hs_off0, hs_off1, hs_off2, hs_remote_length, hs_local_length;
	uint8_t *hs_remote_data, *hs_local_data;
	enum { NVMF_URMA_HS_RECV_HELLO, NVMF_URMA_HS_SEND_RSP, NVMF_URMA_HS_DONE } hs_state;
};
struct nvme_urma_qpair {
	struct spdk_urma_device *device;
	struct {
		uint32_t id;
	} qpair;
	urma_jfr_t **jfrs;
	urma_jetty_t **jettys;
	urma_target_jetty_t **target_jettys;
	uint32_t jetty_count, max_io_size, num_entries;
	enum spdk_urma_capsule_transport capsule_transport;
	int fd;
};
static bool bonding;
static urma_transport_mode_t mode;
static int exports, releases, imports, binds, fail_export = -1;
static unsigned export_length;
static urma_target_jetty_t imported[64];

void
spdk_log(enum spdk_log_level l, const char *f, const int n, const char *fn, const char *fmt, ...)
{
}

/* The provider fixture requires every extension byte, as the supplied ubagg
 * fast path does. A locally reconstructed plain rjetty cannot satisfy it. */
urma_status_t
urma_get_rjetty(urma_jetty_t *j, urma_rjetty_t **out, uint32_t *length)
{
	int i = exports++;
	unsigned extra = bonding ? 23 + j->jetty_id.id % 7 : 0;
	*length = export_length ? export_length : sizeof(**out) + extra;
	*out = NULL;
	if (i == fail_export)
		return URMA_FAIL;
	*out = calloc(1, *length);
	assert(*out);
	(*out)->jetty_id = j->jetty_id;
	(*out)->trans_mode = mode;
	(*out)->tp_type = URMA_CTP;
	(*out)->type = URMA_JETTY;
	if (extra && *length >= sizeof(**out) + extra) {
		(*out)->flag.bs.has_drv_ext = 1;
		uint8_t *p = (uint8_t *)(*out + 1);
		p[0] = extra;
		for (unsigned k = 1; k < extra; k++)
			p[k] = j->jetty_id.id + k;
	}
	return URMA_SUCCESS;
}
void
urma_put_rjetty(urma_rjetty_t *r)
{
	releases++;
	free(r);
}
urma_target_jetty_t *
urma_import_jetty(urma_context_t *c, urma_rjetty_t *r, urma_token_t *token)
{
	assert(token->token == SPDK_URMA_DEFAULT_TOKEN);
	assert(r->trans_mode == mode && r->tp_type == URMA_CTP);
	if (bonding) {
		if (!r->flag.bs.has_drv_ext) {
			errno = EIO;
			return NULL;
		}
		const uint8_t *p = (const uint8_t *)(r + 1);
		unsigned extra = 23 + r->jetty_id.id % 7;
		assert(p[0] == extra);
		for (unsigned k = 1; k < extra; k++)
			assert(p[k] == (uint8_t)(r->jetty_id.id + k));
	}
	assert(imports < 64);
	return &imported[imports++];
}
urma_status_t
urma_bind_jetty(urma_jetty_t *j, urma_target_jetty_t *t)
{
	binds++;
	return URMA_SUCCESS;
}

/* REAL_FUNCTIONS */

struct endpoint {
	struct spdk_urma_device d;
	urma_jetty_t js[3];
	urma_jetty_t *jp[3];
	urma_jfr_t fr[3];
	urma_jfr_t *frp[3];
	urma_target_jetty_t *targets[3];
};
static void
endpoint_init(struct endpoint *e, uint8_t eid, unsigned count)
{
	memset(e, 0, sizeof(*e));
	strcpy(e->d.dev_name, bonding ? "bonding_dev_75" : "udmac0d1e3");
	e->d.eid.raw[0] = eid;
	e->d.opts.transport_mode = mode;
	e->d.opts.tp_type = URMA_CTP;
	e->d.opts.capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_URMA;
	for (unsigned i = 0; i < count; i++) {
		e->js[i].jetty_id.eid = e->d.eid;
		e->js[i].jetty_id.id = eid * 10 + i;
		e->jp[i] = &e->js[i];
		e->frp[i] = &e->fr[i];
		e->fr[i].jfr_cfg.depth = 32;
	}
}
static void
target_init(struct nvmf_urma_qpair *q, struct endpoint *e, struct nvmf_urma_transport *t,
	    unsigned count, int fd)
{
	memset(q, 0, sizeof(*q));
	memset(t, 0, sizeof(*t));
	t->transport.opts.max_queue_depth = 32;
	t->transport.opts.max_io_size = 8388608;
	t->urma_opts.max_io_size = 8388608;
	q->device = &e->d;
	q->transport = t;
	q->jettys = e->jp;
	q->jfrs = e->frp;
	q->target_jettys = e->targets;
	q->jetty_count = count;
	q->fd = fd;
}
static void
counters_reset(void)
{
	exports = releases = imports = binds = 0;
	fail_export = -1;
	export_length = 0;
}
static void
roundtrip(bool bond, urma_transport_mode_t m, unsigned count)
{
	bonding = bond;
	mode = m;
	counters_reset();
	struct endpoint a, b;
	endpoint_init(&a, 1, count);
	endpoint_init(&b, 2, count);
	int fds[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	struct timeval tv = {.tv_sec = 2};
	assert(setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		alarm(4);
		close(fds[0]);
		assert(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
		struct nvmf_urma_qpair q;
		struct nvmf_urma_transport t;
		target_init(&q, &b, &t, count, fds[1]);
		int rc;
		do {
			rc = nvmf_urma_handshake_progress(&q);
			if (rc == -EAGAIN) {
				struct pollfd p = {fds[1], POLLIN, 0};
				poll(&p, 1, 1);
			}
		} while (rc == -EAGAIN);
		assert(rc == 0 && imports == (int)count && exports == releases);
		assert(binds == (mode == URMA_TM_RC ? (int)count : 0));
		free(q.hs_local_data);
		free(q.hs_remote_data);
		close(fds[1]);
		_exit(0);
	}
	close(fds[1]);
	struct nvme_urma_qpair q = {.device = &a.d,
				    .jfrs = a.frp,
				    .jettys = a.jp,
				    .target_jettys = a.targets,
				    .jetty_count = count,
				    .max_io_size = 8388608,
				    .num_entries = 31,
				    .fd = fds[0]};
	assert(nvme_urma_exchange_hello(&q) == 0);
	assert(imports == (int)count && exports == releases);
	assert(binds == (mode == URMA_TM_RC ? (int)count : 0));
	close(fds[0]);
	int status;
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void
validation(void)
{
	bonding = true;
	mode = URMA_TM_RM;
	counters_reset();
	struct endpoint e;
	endpoint_init(&e, 3, 2);
	struct spdk_urma_endpoint_desc d = {.eid = e.d.eid, .jetty_count = 2};
	for (unsigned i = 0; i < 2; i++)
		d.jetty_ids[i] = e.js[i].jetty_id.id;
	uint8_t *data;
	size_t size;
	assert(spdk_urma_export_rjettys(&e.d, e.jp, &d, &data) == 0 && exports == releases);
	size = d.rjetty_lengths[0] + d.rjetty_lengths[1];
	size_t decoded;
	assert(spdk_urma_hello_data_length(&d, sizeof(d) + size, &decoded) == 0 && decoded == size);
	assert(spdk_urma_hello_data_length(&d, sizeof(d) + size - 1, &decoded) == -EPROTO);
	unsigned old = d.rjetty_lengths[0];
	d.rjetty_lengths[0] = SPDK_URMA_MAX_RJETTY_SIZE + 1;
	assert(spdk_urma_hello_data_length(&d, sizeof(d) + size, &decoded) == -EPROTO);
	d.rjetty_lengths[0] = 0;
	assert(spdk_urma_hello_data_length(&d, sizeof(d) + size, &decoded) == -EPROTO);
	d.rjetty_lengths[0] = old;
	urma_target_jetty_t *t;
	assert(spdk_urma_import_rjetty(&e.d, &d, 0, data, &t) == 0);
	d.jetty_ids[0]++;
	assert(spdk_urma_import_rjetty(&e.d, &d, 0, data, &t) == -EPROTO);
	d.jetty_ids[0]--;
	d.eid.raw[0]++;
	assert(spdk_urma_import_rjetty(&e.d, &d, 0, data, &t) == -EPROTO);
	d.eid.raw[0]--;
	((urma_rjetty_t *)data)->trans_mode = URMA_TM_RC;
	assert(spdk_urma_import_rjetty(&e.d, &d, 0, data, &t) == -EPROTO);
	free(data);
	counters_reset();
	fail_export = 1;
	assert(spdk_urma_export_rjettys(&e.d, e.jp, &d, &data) == -EIO && data == NULL &&
	       releases == 1);
	counters_reset();
	export_length = SPDK_URMA_MAX_RJETTY_SIZE + 1;
	assert(spdk_urma_export_rjettys(&e.d, e.jp, &d, &data) == -EPROTO && data == NULL &&
	       releases == 1);
	counters_reset();
	export_length = sizeof(urma_rjetty_t);
	assert(spdk_urma_export_rjettys(&e.d, e.jp, &d, &data) == -EPROTO && data == NULL);
}
static void
fragmented(void)
{
	bonding = true;
	mode = URMA_TM_RM;
	counters_reset();
	export_length = 4096;
	struct endpoint a, b;
	endpoint_init(&a, 1, 2);
	endpoint_init(&b, 2, 2);
	struct spdk_urma_endpoint_desc d = {.eid = a.d.eid,
					    .jetty_count = 2,
					    .transport_mode = mode,
					    .tp_type = URMA_CTP,
					    .max_queue_depth = 32,
					    .max_io_size = 8388608,
					    .capsule_transport = SPDK_URMA_CAPSULE_TRANSPORT_URMA};
	for (unsigned i = 0; i < 2; i++)
		d.jetty_ids[i] = a.js[i].jetty_id.id;
	uint8_t *data;
	assert(spdk_urma_export_rjettys(&a.d, a.jp, &d, &data) == 0);
	size_t size = d.rjetty_lengths[0] + d.rjetty_lengths[1];
	struct spdk_urma_msg_hdr h = {.magic = SPDK_URMA_WIRE_MAGIC,
				      .version = SPDK_URMA_WIRE_VERSION,
				      .type = SPDK_URMA_MSG_HELLO,
				      .length = sizeof(d) + size};
	size_t n = sizeof(h) + h.length;
	uint8_t *frame = malloc(n);
	memcpy(frame, &h, sizeof(h));
	memcpy(frame + sizeof(h), &d, sizeof(d));
	memcpy(frame + sizeof(h) + sizeof(d), data, size);
	free(data);
	int fd[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	assert(fcntl(fd[1], F_SETFL, O_NONBLOCK) == 0);
	int sendbuf = 1024;
	assert(setsockopt(fd[1], SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf)) == 0);
	struct nvmf_urma_qpair q;
	struct nvmf_urma_transport t;
	target_init(&q, &b, &t, 2, fd[1]);
	for (size_t i = 0; i < n; i++) {
		assert(send(fd[0], frame + i, 1, 0) == 1);
		int rc = nvmf_urma_handshake_progress(&q);
		if (i + 1 < n) {
			assert(rc == -EAGAIN);
			assert(imports == 0);
		} else {
			assert((rc == 0 || rc == -EAGAIN) && imports == 2);
		}
	}
	free(frame);
	/* Backpressure must resume the blob without importing the HELLO twice. */
	assert(q.hs_state == NVMF_URMA_HS_SEND_RSP);
	assert(fcntl(fd[0], F_SETFL, O_NONBLOCK) == 0);
	size_t reply_size = sizeof(h) + sizeof(d) + q.hs_local_length;
	uint8_t *reply = malloc(reply_size), *expected = malloc(reply_size);
	memcpy(expected, &q.hs_rsp.hdr, sizeof(h));
	memcpy(expected + sizeof(h), &q.hs_rsp.local, sizeof(d));
	memcpy(expected + sizeof(h) + sizeof(d), q.hs_local_data, q.hs_local_length);
	size_t received = 0;
	while (received < reply_size) {
		ssize_t count = recv(fd[0], reply + received, reply_size - received, 0);
		if (count > 0) {
			received += count;
		} else {
			assert(count < 0 && errno == EAGAIN);
		}
		int rc = nvmf_urma_handshake_progress(&q);
		assert(rc == 0 || rc == -EAGAIN);
	}
	assert(imports == 2 && q.hs_state == NVMF_URMA_HS_DONE);
	assert(memcmp(reply, expected, reply_size) == 0);
	free(reply);
	free(expected);
	free(q.hs_remote_data);
	free(q.hs_local_data);
	close(fd[0]);
	close(fd[1]);
	assert(exports == releases);
	/* An old peer must fail on the header, without waiting for a larger v5 body. */
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	assert(fcntl(fd[1], F_SETFL, O_NONBLOCK) == 0);
	target_init(&q, &b, &t, 2, fd[1]);
	h.version = 4;
	assert(send(fd[0], &h, sizeof(h), 0) == sizeof(h));
	assert(nvmf_urma_handshake_progress(&q) == -EPROTO && q.hs_remote_data == NULL);
	close(fd[0]);
	close(fd[1]);
}
int
main(void)
{
	alarm(15);
#ifdef SPDK_CONFIG_URMA_RJETTY
	for (unsigned n = 1; n <= 3; n += 2) {
		roundtrip(true, URMA_TM_RM, n);
		roundtrip(true, URMA_TM_RC, n);
		roundtrip(false, URMA_TM_RM, n);
	}
	validation();
	fragmented();
	puts("PASS complete provider bytes, both real HELLO paths, multi-jetty RM/RC, fragmented "
	     "TCP, invalid frames and cleanup");
#else
	roundtrip(false, URMA_TM_RM, 1);
	bonding = true;
	mode = URMA_TM_RM;
	struct endpoint e;
	endpoint_init(&e, 1, 1);
	struct spdk_urma_endpoint_desc d = {.jetty_count = 1};
	uint8_t *data;
	assert(spdk_urma_export_rjettys(&e.d, e.jp, &d, &data) == -ENOTSUP && data == NULL);
	puts("PASS legacy SDK raw handshake and explicit bonding rejection");
#endif
	return 0;
}
