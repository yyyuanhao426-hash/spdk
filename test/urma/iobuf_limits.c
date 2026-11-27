/* SPDX-License-Identifier: BSD-3-Clause */
/* Real URMA create/dump + real JSON and common code; no disks/provider device. */
#include "nvme_urma_internal.h"
#include "nvmf_internal.h"
#include "spdk/json.h"
#include "spdk_internal/urma_lifetime.h"
#define URMA_STAGE_SIDE 2
#include "spdk_internal/urma_stage_buffer.h"

/* REAL_TYPES */

static struct spdk_iobuf_opts pool;
static struct spdk_urma_device device;
static unsigned int opens, closes;
static bool defer_buffer;
static struct spdk_iobuf_entry *waiting_entry;
static spdk_iobuf_get_cb waiting_cb;
static unsigned int allocated, returned, ready;
static char json[8192];
static size_t json_used;

static int nvmf_urma_accept(void *ctx) { return SPDK_POLLER_IDLE; }
static void nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf);

/* REAL_FUNCTIONS */

void
spdk_log(enum spdk_log_level level, const char *file, int line, const char *func,
	 const char *fmt, ...)
{
}

uint64_t spdk_get_ticks_hz(void) { return 1000000; }
void spdk_log_register_flag(const char *name, struct spdk_log_flag *flag) {}

void
spdk_iobuf_get_opts(struct spdk_iobuf_opts *out, size_t size)
{
	assert(size == sizeof(*out));
	*out = pool;
}

int
spdk_iobuf_for_each_pool_memory(spdk_iobuf_pool_memory_cb cb, void *ctx)
{
	return 0;
}

int
__wrap_spdk_urma_device_open(const struct spdk_urma_transport_opts *opts,
			     struct spdk_urma_device **out)
{
	opens++;
	device.opts = *opts;
	strcpy(device.dev_name, "test-device");
	*out = &device;
	return 0;
}

void
__wrap_spdk_urma_device_close(struct spdk_urma_device *dev)
{
	assert(dev == &device);
	closes++;
}

urma_target_seg_t *
urma_register_seg(urma_context_t *context, urma_seg_cfg_t *cfg)
{
	assert(false);
	return NULL;
}

urma_status_t urma_unregister_seg(urma_target_seg_t *seg) { assert(false); return URMA_FAIL; }

struct spdk_poller *
spdk_poller_register_named(spdk_poller_fn fn, void *ctx, uint64_t period, const char *name)
{
	return (void *)1;
}

void *
spdk_iobuf_get(struct spdk_iobuf_channel *ch, uint64_t len, struct spdk_iobuf_entry *entry,
	       spdk_iobuf_get_cb cb)
{
	assert(len <= spdk_max(pool.small_bufsize, pool.large_bufsize));
	if (defer_buffer) {
		defer_buffer = false;
		waiting_entry = entry;
		waiting_cb = cb;
		assert(entry && cb);
		return NULL;
	}
	allocated++;
	return malloc(len <= pool.small_bufsize ? pool.small_bufsize : pool.large_bufsize);
}

void
spdk_iobuf_put(struct spdk_iobuf_channel *ch, void *buf, uint64_t len)
{
	returned++;
	free(buf);
}

static void buffers_ready(struct spdk_nvmf_request *req) { ready++; }

static void
single_buffer(struct spdk_nvmf_transport *base, uint32_t length, bool wait)
{
	const struct spdk_nvmf_transport_ops ops = {.req_get_buffers_done = buffers_ready};
	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_transport_poll_group tgroup = {.transport = base};
	struct spdk_nvmf_qpair qpair = {.transport = base, .group = &group};
	struct spdk_nvmf_request req = {.qpair = &qpair};
	unsigned int before = ready;
	int rc;

	/* These slot fields are populated by transport.c after create returns. */
	base->small_bufsize = pool.small_bufsize;
	base->large_bufsize = pool.large_bufsize;
	base->ops = &ops;
	TAILQ_INIT(&group.tgroups);
	TAILQ_INSERT_TAIL(&group.tgroups, &tgroup, link);
	defer_buffer = wait;
	rc = spdk_nvmf_request_get_buffers(&req, &tgroup, base, length);
	if (wait) {
		assert(rc == -ENOMEM && !req.iovcnt && req.iobuf.remaining_length == length);
		assert(ready == before);
		allocated++;
		waiting_cb(waiting_entry,
			   malloc(length <= pool.small_bufsize ? pool.small_bufsize : pool.large_bufsize));
		assert(ready == before + 1);
	} else {
		assert(!rc);
	}
	assert(req.iovcnt == 1 && req.iov[0].iov_len == length);
	spdk_nvmf_request_free_buffers(&req, &tgroup, base);
	assert(!req.iovcnt && allocated == returned);
}

static int
json_output(void *ctx, const void *data, size_t length)
{
	assert(json_used + length < sizeof(json));
	memcpy(json + json_used, data, length);
	json_used += length;
	json[json_used] = 0;
	return 0;
}

static void
check(uint32_t requested, uint32_t large, uint32_t expected)
{
	struct spdk_nvmf_transport_opts opts = {
		.opts_size = sizeof(opts), .max_io_size = requested,
		.iobuf_small_cache_size = 1024, .iobuf_large_cache_size = 32,
	};
	struct spdk_nvmf_transport *base;
	struct nvmf_urma_transport *transport;
	struct spdk_json_write_ctx *w;
	char expected_field[128];

	pool = (struct spdk_iobuf_opts) {
		.opts_size = sizeof(pool), .small_bufsize = 8192, .large_bufsize = large,
		.small_pool_count = 16384, .large_pool_count = 1024,
	};
	base = nvmf_urma_create(&opts);
	assert(base);
	transport = SPDK_CONTAINEROF(base, struct nvmf_urma_transport, transport);
	if (transport->urma_opts.max_io_size != expected) {
		fprintf(stderr, "FAIL requested=%u large_slot=%u effective=%u expected=%u\n",
			requested, large, transport->urma_opts.max_io_size, expected);
		abort();
	}
	assert(base->opts.max_io_size == requested);
	assert(pool.large_pool_count == 1024 && pool.small_pool_count == 16384);
	json_used = 0;
	w = spdk_json_write_begin(json_output, NULL, 0);
	assert(w);
	spdk_json_write_object_begin(w);
	nvmf_urma_dump_opts(base, w);
	spdk_json_write_object_end(w);
	assert(!spdk_json_write_end(w));
	snprintf(expected_field, sizeof(expected_field), "\"effective_max_io_size\":%u", expected);
	assert(strstr(json, expected_field));
	snprintf(expected_field, sizeof(expected_field), "\"iobuf_large_bufsize\":%u", large);
	assert(strstr(json, expected_field));
	single_buffer(base, expected, false);
	single_buffer(base, expected, true);
	single_buffer(base, 512, false);
	nvmf_urma_unregister_iobuf_pool(transport);
	spdk_urma_device_close(transport->device);
	pthread_rwlock_destroy(&transport->import_cache_lock);
	free(transport);
}

int
main(void)
{
	check(8388608, 262144, 262144);
	check(8388608, 524288, 524288);
	check(8388608, 8388608, 8388608);
	check(131072, 262144, 131072);
	check(131072, 135168, 131072);
	check(8388608, 135168, 131072);
	check(8388608, 393216, 262144);
	check(8388608, 8192, 8192);
	{
		struct spdk_nvmf_transport_opts opts = {.max_io_size = 8388608};
		unsigned int before = opens;

		pool.small_bufsize = pool.large_bufsize = 0;
		assert(!nvmf_urma_create(&opts) && opens == before);
	}
	assert(opens == closes);
	puts("PASS bare URMA create: external large slots govern effective transfer limit; "
	     "requested max, defaults and pool counts preserved; real JSON state and single-buffer "
	     "allocation/retry paths");
	return 0;
}
