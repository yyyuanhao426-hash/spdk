/* SPDX-License-Identifier: BSD-3-Clause */
/* The production bdev adapter is compiled unchanged; only the lower SPDK I/O
 * backend is replaced. Requests pass through its real submit/retry/completion. */
#include "../../module/bdev/readcache/vbdev_readcache.c"

#define BLOCK 4096
#define IOS 32
static unsigned char disk[BLOCK * 8];
static unsigned int reads, writes, pending_count, completed;
static uintptr_t current_channel;
static int submit_error, queue_error;
static bool fail_get_buf;
static bool fail_dma;
static uint64_t test_now_us = 1;
static unsigned int dma_live, channel_holds;
static struct spdk_io_channel *held_channels[128];
static struct spdk_bdev_io_wait_entry *queued;

/* Typed local SPDK framework mocks. Production create/delete callbacks below
 * exercise claim, registration failure, channel cleanup and hot removal. */
struct spdk_bdev_desc {
	struct spdk_bdev *base;
};
static struct spdk_bdev framework_base, *framework_registered;
static struct spdk_bdev_desc framework_desc;
static unsigned int opens, claims, io_devices;
static int register_error, claim_error;
static bool defer_unregister;
static spdk_io_device_unregister_cb deferred_unregister;
static void *deferred_device;
static void *framework_device, *framework_event_ctx;
static spdk_bdev_event_cb_t framework_event;
static spdk_io_channel_create_cb framework_create;
static spdk_io_channel_destroy_cb framework_destroy;
static uint32_t framework_ctx_size;
static const struct spdk_thread *framework_thread = (void *)1;
static spdk_poller_fn cleanup_fn;
static void *cleanup_ctx;
static struct spdk_poller *cleanup_poller;

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type type)
{
	return true;
}

struct spdk_io_channel *
spdk_get_io_channel(void *ctx)
{
	struct spdk_io_channel *ch;

	assert(ctx == framework_device);
	ch = calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE + framework_ctx_size);
	assert(ch);
	if (framework_create(ctx, spdk_io_channel_get_ctx(ch)) != 0) {
		free(ch);
		return NULL;
	}
	return ch;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	assert(desc == &framework_desc);
	return calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE);
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	unsigned int i;

	for (i = 0; i < SPDK_COUNTOF(held_channels); i++) {
		if (held_channels[i] == ch) {
			held_channels[i] = NULL;
			channel_holds--;
			return;
		}
	}
	free(ch);
}

struct spdk_io_channel *
spdk_io_channel_ref(struct spdk_io_channel *ch)
{
	unsigned int i;

	for (i = 0; i < SPDK_COUNTOF(held_channels); i++) {
		if (!held_channels[i]) {
			held_channels[i] = ch;
			channel_holds++;
			return ch;
		}
	}
	assert(false);
	return NULL;
}

uint64_t
spdk_get_ticks(void)
{
	return test_now_us;
}

uint64_t
spdk_get_ticks_hz(void)
{
	return 1000000;
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *unused)
{
	void *p = NULL;

	if (fail_dma) {
		fail_dma = false;
		return NULL;
	}
	assert(!posix_memalign(&p, align, size));
	dma_live++;
	return p;
}

void
spdk_dma_free(void *p)
{
	assert(p && dma_live);
	dma_live--;
	free(p);
}

#ifndef READCACHE_REAL_JSON
void
spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	 const char *format, ...)
{
}
#endif

struct spdk_poller *
spdk_poller_register_named(spdk_poller_fn fn, void *ctx, uint64_t period, const char *name)
{
	struct spdk_poller *poller = malloc(1);

	if (!strcmp(name, "readcache_cleanup_poll") && poller) {
		cleanup_fn = fn;
		cleanup_ctx = ctx;
		cleanup_poller = poller;
	}
	return poller;
}

void
spdk_poller_unregister(struct spdk_poller **poller)
{
	if (*poller == cleanup_poller) {
		cleanup_poller = NULL;
	}
	free(*poller);
	*poller = NULL;
}

int
spdk_bdev_open_ext(const char *name, bool write, spdk_bdev_event_cb_t cb, void *ctx,
		   struct spdk_bdev_desc **desc)
{
	if (strcmp(name, "base") != 0) {
		return -ENODEV;
	}
	framework_desc.base = &framework_base;
	framework_event = cb;
	framework_event_ctx = ctx;
	*desc = &framework_desc;
	opens++;
	return 0;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->base;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	assert(desc == &framework_desc && opens);
	opens--;
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *base, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module *module)
{
	assert(base == &framework_base);
	if (claim_error) {
		return claim_error;
	}
	assert(!claims);
	claims++;
	return 0;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	assert(bdev == &framework_base && claims);
	claims--;
}

void
spdk_io_device_register(void *ctx, spdk_io_channel_create_cb create,
			spdk_io_channel_destroy_cb destroy, uint32_t size, const char *name)
{
	assert(!io_devices);
	io_devices++;
	framework_device = ctx;
	framework_create = create;
	framework_destroy = destroy;
	framework_ctx_size = size;
}

void
spdk_io_device_unregister(void *ctx, spdk_io_device_unregister_cb cb)
{
	assert(io_devices && framework_device == ctx);
	if (defer_unregister) {
		deferred_unregister = cb;
		deferred_device = ctx;
		return;
	}
	io_devices--;
	framework_device = NULL;
	cb(ctx);
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	if (register_error) {
		return register_error;
	}
	assert(!framework_registered);
	framework_registered = bdev;
	return 0;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb, void *arg)
{
	assert(bdev == framework_registered);
	framework_registered = NULL;
	bdev->internal.unregister_cb = cb;
	bdev->internal.unregister_ctx = arg;
	assert(bdev->fn_table->destruct(bdev->ctxt) == 1);
}

void
spdk_bdev_destruct_done(struct spdk_bdev *bdev, int rc)
{
	if (bdev->internal.unregister_cb) {
		bdev->internal.unregister_cb(bdev->internal.unregister_ctx, rc);
	}
}

int
spdk_bdev_unregister_by_name(const char *name, struct spdk_bdev_module *module,
			     spdk_bdev_unregister_cb cb, void *arg)
{
	if (!framework_registered || strcmp(framework_registered->name, name)) {
		return -ENODEV;
	}
	spdk_bdev_unregister(framework_registered, cb, arg);
	return 0;
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;
	return 0;
}

void
spdk_uuid_generate(struct spdk_uuid *uuid)
{
	memset(uuid, 1, sizeof(*uuid));
}

struct spdk_thread *
spdk_get_thread(void)
{
	return (struct spdk_thread *)framework_thread;
}

int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	const struct spdk_thread *previous = framework_thread;

	framework_thread = thread;
	fn(ctx);
	framework_thread = previous;
	return 0;
}

#ifndef READCACHE_REAL_JSON
int
spdk_json_write_named_string(struct spdk_json_write_ctx *w, const char *name, const char *value)
{
	return 0;
}

int
spdk_json_write_named_object_begin(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int
spdk_json_write_named_uint64(struct spdk_json_write_ctx *w, const char *name, uint64_t value)
{
	return 0;
}

int
spdk_json_write_named_bool(struct spdk_json_write_ctx *w, const char *name, bool value)
{
	return 0;
}

int
spdk_json_write_object_end(struct spdk_json_write_ctx *w)
{
	return 0;
}
#endif
static void
framework_tick_cleanup(void)
{
	if (cleanup_poller) {
		struct readcache_bdev *dev = cleanup_ctx;
		const struct spdk_thread *previous = framework_thread;

		framework_thread = dev->open_thread;
		cleanup_fn(cleanup_ctx);
		framework_thread = previous;
	}
}

static struct {
	struct spdk_bdev_io *bio;
	spdk_bdev_io_completion_cb cb;
	void *arg;
	uintptr_t channel;
	unsigned char read_data[BLOCK * 8];
} pending[IOS];

void
spdk_bdev_module_list_add(struct spdk_bdev_module *module)
{
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bio, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	struct readcache_io *io = (void *)bio->driver_ctx;

	if (fail_get_buf) {
		fail_get_buf = false;
		cb(io->ch, bio, false);
	} else {
		cb(io->ch, bio, true);
	}
}

static int
backend(struct spdk_io_channel *ch, uint64_t offset, uint64_t length, struct iovec *iov,
	enum spdk_bdev_io_type type, spdk_bdev_io_completion_cb cb, void *arg)
{
	struct spdk_bdev_io *bio;

	if (submit_error) {
		int rc = submit_error;

		submit_error = 0;
		return rc;
	}
	assert(pending_count < IOS);
	bio = calloc(1, sizeof(*bio));
	assert(bio);
	bio->type = type;
	bio->u.bdev.offset_blocks = offset;
	bio->u.bdev.num_blocks = length;
	bio->u.bdev.iovs = iov;
	bio->u.bdev.iovcnt = 1;
	pending[pending_count].bio = bio;
	pending[pending_count].cb = cb;
	pending[pending_count].arg = arg;
	pending[pending_count].channel = current_channel;
	if (type == SPDK_BDEV_IO_TYPE_READ) {
		assert(offset + length <= 8);
		memcpy(pending[pending_count].read_data, disk + offset * BLOCK, length * BLOCK);
	}
	pending_count++;
	reads += type == SPDK_BDEV_IO_TYPE_READ;
	writes += type == SPDK_BDEV_IO_TYPE_WRITE;
	return 0;
}

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *d, struct spdk_io_channel *ch, struct iovec *iov,
			   int cnt, uint64_t offset, uint64_t length, spdk_bdev_io_completion_cb cb,
			   void *arg, struct spdk_bdev_ext_io_opts *opts)
{
	assert(cnt == 1);
	return backend(ch, offset, length, iov, SPDK_BDEV_IO_TYPE_READ, cb, arg);
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *d, struct spdk_io_channel *ch, struct iovec *iov,
			    int cnt, uint64_t offset, uint64_t length,
			    spdk_bdev_io_completion_cb cb, void *arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	int rc;

	assert(cnt == 1);
	rc = backend(ch, offset, length, iov, SPDK_BDEV_IO_TYPE_WRITE, cb, arg);
	if (!rc) {
		pending[pending_count - 1].bio->u.bdev.nvme_cdw12 = opts->nvme_cdw12;
		pending[pending_count - 1].bio->u.bdev.nvme_cdw13 = opts->nvme_cdw13;
	}
	return rc;
}

#define BACKEND_RANGE(fn, type)                                                                    \
	int fn(struct spdk_bdev_desc *d, struct spdk_io_channel *ch, uint64_t offset,              \
	       uint64_t length, spdk_bdev_io_completion_cb cb, void *arg)                          \
	{                                                                                          \
		return backend(ch, offset, length, NULL, type, cb, arg);                           \
	}
BACKEND_RANGE(spdk_bdev_unmap_blocks, SPDK_BDEV_IO_TYPE_UNMAP)
BACKEND_RANGE(spdk_bdev_write_zeroes_blocks, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)
BACKEND_RANGE(spdk_bdev_flush_blocks, SPDK_BDEV_IO_TYPE_FLUSH)

int
spdk_bdev_reset(struct spdk_bdev_desc *d, struct spdk_io_channel *ch, spdk_bdev_io_completion_cb cb,
		void *arg)
{
	return backend(ch, 0, 0, NULL, SPDK_BDEV_IO_TYPE_RESET, cb, arg);
}

int
spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			struct spdk_bdev_io_wait_entry *entry)
{
	assert(!queued);
	if (queue_error) {
		return queue_error;
	}
	queued = entry;
	return 0;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bio, enum spdk_bdev_io_status status)
{
	struct readcache_io *io = (void *)bio->driver_ctx;

	assert(io->ch == (struct spdk_io_channel *)bio->internal.caller_ctx);
	assert(current_channel == (uintptr_t)io->ch);
	assert(bio->internal.status == SPDK_BDEV_IO_STATUS_PENDING);
	bio->internal.status = status;
	completed++;
}

void
spdk_bdev_io_complete_base_io_status(struct spdk_bdev_io *bio, const struct spdk_bdev_io *base)
{
	spdk_bdev_io_complete(bio, base->internal.status);
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bio)
{
	free(bio);
}

static void
finish(unsigned int index, bool success)
{
	struct spdk_bdev_io *bio = pending[index].bio;
	spdk_bdev_io_completion_cb cb = pending[index].cb;
	void *arg = pending[index].arg;
	unsigned char *data = disk + bio->u.bdev.offset_blocks * BLOCK;

	current_channel = pending[index].channel;
	if (success) {
		if (bio->type == SPDK_BDEV_IO_TYPE_READ) {
			memcpy(bio->u.bdev.iovs[0].iov_base, pending[index].read_data,
			       bio->u.bdev.num_blocks * BLOCK);
		} else if (bio->type == SPDK_BDEV_IO_TYPE_WRITE) {
			memcpy(data, bio->u.bdev.iovs[0].iov_base, bio->u.bdev.num_blocks * BLOCK);
		} else if (bio->type == SPDK_BDEV_IO_TYPE_UNMAP ||
			   bio->type == SPDK_BDEV_IO_TYPE_WRITE_ZEROES) {
			memset(data, 0, bio->u.bdev.num_blocks * BLOCK);
		}
	}
	bio->internal.status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	memmove(&pending[index], &pending[index + 1],
		(--pending_count - index) * sizeof(pending[0]));
	cb(bio, success, arg);
}

static struct spdk_bdev_io *
request(struct readcache_bdev *dev, struct spdk_io_channel *channel, enum spdk_bdev_io_type type,
	uint64_t offset, unsigned char value)
{
	struct spdk_bdev_io *bio = calloc(1, sizeof(*bio) + sizeof(struct readcache_io));
	struct iovec *iov = calloc(1, sizeof(*iov));

	assert(bio && iov);
	iov->iov_base = malloc(BLOCK);
	iov->iov_len = BLOCK;
	assert(iov->iov_base);
	memset(iov->iov_base, value, BLOCK);
	bio->bdev = &dev->bdev;
	bio->type = type;
	bio->u.bdev.iovs = iov;
	bio->u.bdev.iovcnt = 1;
	bio->u.bdev.offset_blocks = offset;
	bio->u.bdev.num_blocks = 1;
	bio->internal.caller_ctx = (void *)channel;
	return bio;
}

static void
submit(struct spdk_bdev_io *bio)
{
	current_channel = (uintptr_t)bio->internal.caller_ctx;
	readcache_submit(bio->internal.caller_ctx, bio);
}

static void
check_done(struct spdk_bdev_io *bio, unsigned char value, bool success)
{
	unsigned char *data = bio->u.bdev.iovs[0].iov_base;
	unsigned int i;

	assert(bio->internal.status ==
	       (success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED));
	if (success && bio->type == SPDK_BDEV_IO_TYPE_READ) {
		for (i = 0; i < BLOCK; i++) {
			assert(data[i] == value);
		}
	}
	free(data);
	free(bio->u.bdev.iovs);
	free(bio);
}

static void
deleted(void *arg, int rc)
{
	*(int *)arg = rc;
}

static void
lifecycle(void)
{
	struct spdk_io_channel *ch;
	struct readcache_bdev *dev;
	struct readcache_stats stats;
	struct readcache_token token;
	int rc = -1;

	framework_base.blocklen = BLOCK;
	framework_base.blockcnt = 8;
	framework_base.name = "base";
	assert(bdev_readcache_create("base", "cache", 128 * 1024, BLOCK, false) == 0);
	assert(bdev_readcache_get_stats("cache", &stats) == 0);
	assert(stats.capacity_bytes == 128 * 1024);
	dev = framework_registered->ctxt;
	ch = dev->bdev.fn_table->get_io_channel(dev);
	assert(ch);
	assert(!dev->bdev.fn_table->io_type_supported(dev, SPDK_BDEV_IO_TYPE_COPY));
	assert(!dev->bdev.fn_table->io_type_supported(dev, SPDK_BDEV_IO_TYPE_ZCOPY));
	assert(readcache_acquire(dev->cache, 0, BLOCK, &token) == READCACHE_MISS);
	readcache_fill(dev->cache, &token, NULL, 0, false);
	readcache_release(dev->cache, &token);
	framework_base.blockcnt = 16;
	framework_event(SPDK_BDEV_EVENT_RESIZE, &framework_base, framework_event_ctx);
	assert(framework_registered->blockcnt == 16);
	framework_destroy(dev, spdk_io_channel_get_ctx(ch));
	free(ch);
	framework_thread = (void *)2; /* descriptor closes on its original thread */
	bdev_readcache_delete("cache", deleted, &rc);
	assert(rc == -1 && opens == 1);
	framework_tick_cleanup();
	assert(rc == 0 && !opens && !claims && !io_devices && !framework_registered);
	assert(bdev_readcache_get_stats("cache", &stats) == -ENODEV);
	bdev_readcache_delete("cache", deleted, &rc);
	assert(rc == -ENODEV);

	framework_base.md_len = 8;
	assert(bdev_readcache_create("base", "cache", 128 * 1024, BLOCK, false) == -ENOTSUP);
	framework_base.md_len = 0;
	framework_base.zoned = true;
	assert(bdev_readcache_create("base", "cache", 128 * 1024, BLOCK, false) == -ENOTSUP);
	framework_base.zoned = false;
	claim_error = -EBUSY;
	assert(bdev_readcache_create("base", "cache", 128 * 1024, BLOCK, false) == -EBUSY);
	claim_error = 0;
	register_error = -EEXIST;
	assert(bdev_readcache_create("base", "cache", 128 * 1024, BLOCK, false) == -EEXIST);
	register_error = 0;
	framework_tick_cleanup();
	assert(!opens && !claims && !io_devices);
	assert(bdev_readcache_create("missing", "cache", 128 * 1024, BLOCK, false) == -ENODEV);
	assert(bdev_readcache_create("base", "cache", 128 * 1024, BLOCK, false) == 0);
	framework_event(SPDK_BDEV_EVENT_REMOVE, &framework_base, framework_event_ctx);
	framework_tick_cleanup();
	assert(!opens && !claims && !io_devices && !framework_registered);
	puts("PASS production lifecycle: create, channel cleanup, resize, delete on another "
	     "thread, hot-remove and rollback");
}

static void
delayed_lifecycle(void)
{
	struct readcache_bdev *dev;
	struct spdk_io_channel *a, *b;
	struct spdk_bdev_io *leader, *follower;
	int result = -999;

	assert(bdev_readcache_create("base", "delayed", 128 * 1024, BLOCK, false) == 0);
	dev = framework_registered->ctxt;
	a = readcache_get_channel(dev);
	b = readcache_get_channel(dev);
	leader = request(dev, a, SPDK_BDEV_IO_TYPE_READ, 0, 0xff);
	follower = request(dev, b, SPDK_BDEV_IO_TYPE_READ, 0, 0xff);
	submit(leader);
	submit(follower);
	assert(pending_count == 1);
	defer_unregister = true;
	bdev_readcache_delete("delayed", deleted, &result);
	assert(result == -999 && opens == 1 && io_devices == 1 && claims == 1);
	finish(0, true);
	current_channel = (uintptr_t)b;
	readcache_poll(spdk_io_channel_get_ctx(b));
	check_done(leader, 0, true);
	check_done(follower, 0, true);
	framework_destroy(dev, spdk_io_channel_get_ctx(a));
	framework_destroy(dev, spdk_io_channel_get_ctx(b));
	free(a);
	free(b);
	assert(result == -999 && opens == 1);
	defer_unregister = false;
	io_devices--;
	framework_device = NULL;
	deferred_unregister(deferred_device);
	deferred_unregister = NULL;
	assert(result == -999 && opens == 1);
	framework_tick_cleanup();
	assert(result == 0 && !opens && !io_devices);
	puts("PASS delayed unregister: descriptor/cache and delete RPC live until all merged I/O "
	     "and channels drain");
}

int
main(void)
{
	struct readcache_bdev dev = {};
	struct spdk_io_channel *channels[10];
	struct spdk_bdev_io *bio[10], *write_io, *old, *fresh;
	struct readcache_channel *ch;
	struct readcache_stats s;
	unsigned int i;

	dev.cache = readcache_create(128 * 1024, BLOCK);
	dev.bdev.ctxt = &dev;
	dev.bdev.blocklen = BLOCK;
	dev.populate_on_write = true;
	assert(dev.cache);
	for (i = 0; i < 10; i++) {
		channels[i] = calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE + sizeof(*ch));
		assert(channels[i]);
		ch = spdk_io_channel_get_ctx(channels[i]);
		TAILQ_INIT(&ch->pending);
		TAILQ_INIT(&ch->gated);
		TAILQ_INIT(&ch->background);
		ch->base_ch = channels[i];
	}
	memset(disk, 0x11, sizeof(disk));
	for (i = 0; i < 10; i++) {
		bio[i] = request(&dev, channels[i], SPDK_BDEV_IO_TYPE_READ, 0, 0);
		submit(bio[i]);
	}
	assert(reads == 1 && pending_count == 1 && completed == 0);
	finish(0, true);
	for (i = 0; i < 10; i++) {
		current_channel = (uintptr_t)channels[i];
		readcache_poll(spdk_io_channel_get_ctx(channels[i]));
		check_done(bio[i], 0x11, true);
	}
	assert(completed == 10);
	puts("PASS production bdev: ten channel reads -> one backend read, all buffers verified");

	write_io = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_WRITE, 0, 0x22);
	submit(write_io);
	assert(writes == 1 && completed == 10); /* no early write acknowledgment */
	finish(0, true);
	check_done(write_io, 0, true);
	bio[0] = request(&dev, channels[1], SPDK_BDEV_IO_TYPE_READ, 0, 0);
	submit(bio[0]);
	check_done(bio[0], 0x22, true);
	assert(reads == 1);
	puts("PASS production bdev: successful SSD write precedes cache warm and completion");

	old = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_READ, 1, 0);
	submit(old);
	write_io = request(&dev, channels[1], SPDK_BDEV_IO_TYPE_WRITE, 1, 0x33);
	submit(write_io);
	finish(1, true); /* write finishes before old read */
	check_done(write_io, 0, true);
	finish(0, true);
	check_done(old, 0x11, true);
	fresh = request(&dev, channels[2], SPDK_BDEV_IO_TYPE_READ, 1, 0);
	submit(fresh);
	check_done(fresh, 0x33, true);
	puts("PASS production bdev: write/read completion interleaving preserves current data");

	submit_error = -ENOMEM;
	bio[0] = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_READ, 2, 0);
	submit(bio[0]);
	bio[1] = request(&dev, channels[1], SPDK_BDEV_IO_TYPE_READ, 2, 0);
	submit(bio[1]);
	assert(queued && pending_count == 0);
	{
		struct spdk_bdev_io_wait_entry *entry = queued;

		queued = NULL;
		current_channel = (uintptr_t)channels[0];
		entry->cb_fn(entry->cb_arg);
	}
	assert(pending_count == 1);
	finish(0, false);
	current_channel = (uintptr_t)channels[1];
	readcache_poll(spdk_io_channel_get_ctx(channels[1]));
	check_done(bio[0], 0, false);
	check_done(bio[1], 0, false);
	readcache_get_stats(dev.cache, &s);
	assert(s.active_mutations == 0 && s.fill_errors == 1);
	puts("PASS production bdev: backend ENOMEM retry retains miss leader; read failure wakes "
	     "followers");

	{
		enum spdk_bdev_io_type operations[] = {SPDK_BDEV_IO_TYPE_UNMAP,
						       SPDK_BDEV_IO_TYPE_WRITE_ZEROES,
						       SPDK_BDEV_IO_TYPE_RESET};
		unsigned int op;

		for (op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
			bio[0] = request(&dev, channels[0], operations[op], 0, 0);
			submit(bio[0]);
			finish(0, true);
			check_done(bio[0], 0, true);
			bio[0] = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_READ, 0, 0xff);
			submit(bio[0]);
			assert(pending_count == 1);
			finish(0, true);
			check_done(bio[0], 0, true);
		}
	}
	submit_error = -ENOMEM;
	queue_error = -EINVAL;
	bio[0] = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_WRITE, 3, 0xff);
	submit(bio[0]);
	check_done(bio[0], 0, false);
	readcache_get_stats(dev.cache, &s);
	assert(s.active_mutations == 0);
	puts("PASS production bdev: UNMAP/WRITE_ZEROES/RESET invalidation and queue-registration "
	     "failure cleanup");
	fail_get_buf = true;
	bio[0] = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_READ, 4, 0);
	submit(bio[0]);
	check_done(bio[0], 0, false);
	bio[0] = request(&dev, channels[0], SPDK_BDEV_IO_TYPE_READ, 4, 0);
	bio[0]->u.bdev.nvme_cdw12.raw = 1;
	submit(bio[0]);
	assert(pending_count == 1);
	finish(0, true);
	check_done(bio[0], 0x11, true);
	readcache_get_stats(dev.cache, &s);
	assert(s.bypass_external == 1 && s.io_errors == 4);
	puts("PASS request buffer failure and special-flag bypass: errors and all read bytes are "
	     "accounted");
	for (i = 0; i < 10; i++) {
		ch = spdk_io_channel_get_ctx(channels[i]);
		assert(TAILQ_EMPTY(&ch->pending));
		free(channels[i]);
	}
	assert(readcache_destroy(dev.cache) == 0);
	lifecycle();
	delayed_lifecycle();
	return 0;
}
