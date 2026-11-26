/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/nvme_spec.h"
#include "vbdev_readcache.h"

struct readcache_bdev {
	struct spdk_bdev bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *base;
	struct spdk_thread *open_thread;
	struct readcache *cache;
	struct writeback *writeback;
	atomic_bool populate_on_write;
	bool destructing;
	atomic_bool channels_gone;
	struct spdk_poller *cleanup_poller;
	TAILQ_ENTRY(readcache_bdev) link;
};

struct readcache_io {
	struct spdk_bdev_io *bio;
	struct spdk_io_channel *ch;
	struct readcache_token token;
	struct readcache_mutation mutation;
	struct spdk_bdev_io_wait_entry wait;
	bool mutating;
	bool gated;
	bool fua_flush;
	struct writeback_node gate;
	TAILQ_ENTRY(readcache_io) link;
};

struct readcache_channel {
	struct spdk_io_channel *base_ch;
	struct spdk_poller *poller;
	TAILQ_HEAD(, readcache_io) pending;
	TAILQ_HEAD(, readcache_io) gated;
	TAILQ_HEAD(, readcache_background) background;
};

/* Never retain a foreground bdev_io, CID, qpair, or its iobuf after ACK. The
 * extra wrapper channel reference keeps both device and base channel alive. */
struct readcache_background {
	struct readcache_bdev *dev;
	struct spdk_io_channel *channel;
	struct writeback_node gate;
	struct readcache_mutation mutation;
	struct spdk_bdev_io_wait_entry wait;
	struct iovec iov;
	bool submitted, mutating;
	TAILQ_ENTRY(readcache_background) link;
};

static TAILQ_HEAD(, readcache_bdev) g_readcache = TAILQ_HEAD_INITIALIZER(g_readcache);
static void readcache_submit_base(struct spdk_bdev_io *bio);
static struct readcache_bdev *readcache_device(struct spdk_bdev_io *bio);
static void readcache_start(struct readcache_io *io);
static void readcache_background_submit(void *arg);

static uint64_t
readcache_now_us(void)
{
	uint64_t ticks = spdk_get_ticks(), hz = spdk_get_ticks_hz();

	return ticks / hz * 1000000 + ticks % hz * 1000000 / hz;
}

static void
readcache_gate_done(struct readcache_io *io, bool success)
{
	if (io->gated) {
		writeback_leave(readcache_device(io->bio)->writeback, &io->gate, success,
				readcache_now_us());
		io->gated = false;
	}
}

static int
readcache_module_init(void)
{
	return 0;
}

static int
readcache_ctx_size(void)
{
	return sizeof(struct readcache_io);
}

static struct spdk_bdev_module readcache_module = {
	.name = "readcache",
	.module_init = readcache_module_init,
	.get_ctx_size = readcache_ctx_size,
};
SPDK_BDEV_MODULE_REGISTER(readcache, &readcache_module)

static struct readcache_bdev *
readcache_device(struct spdk_bdev_io *bio)
{
	return bio->bdev->ctxt;
}

static uint64_t
readcache_offset(struct spdk_bdev_io *bio)
{
	return bio->u.bdev.offset_blocks * bio->bdev->blocklen;
}

static uint64_t
readcache_length(struct spdk_bdev_io *bio)
{
	return bio->u.bdev.num_blocks * bio->bdev->blocklen;
}

static bool
readcache_host_buffers(struct spdk_bdev_io *bio)
{
	/* Remote/GPU memory and accel sequences cannot be accessed by memcpy.
	 * The wrapper advertises host memory only; retain a safe bypass as well. */
	return bio->u.bdev.memory_domain == NULL && bio->u.bdev.accel_sequence == NULL &&
	       bio->u.bdev.md_buf == NULL;
}

static void
readcache_finish_local(struct readcache_io *io)
{
	struct spdk_bdev_io *bio = io->bio;
	struct readcache_bdev *dev = readcache_device(bio);
	bool success = readcache_copy(&io->token, bio->u.bdev.iovs, bio->u.bdev.iovcnt);

	if (!success) {
		readcache_record_io_error(dev->cache);
	}

	/* Copy into the request's original iobuf. Cache memory is never exposed to
	 * URMA or returned to the iobuf allocator; normal data-CQE ownership remains. */
	readcache_release(dev->cache, &io->token);
	readcache_gate_done(io, success);
	spdk_bdev_io_complete(bio,
			      success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static int
readcache_poll(void *arg)
{
	struct readcache_channel *ch = arg;
	struct readcache_io *io, *next;
	struct readcache_background *bg, *bg_next;
	int count = 0;
	int rc;

	for (bg = TAILQ_FIRST(&ch->background); bg != NULL; bg = bg_next) {
		bg_next = TAILQ_NEXT(bg, link);
		if (!bg->submitted && writeback_ready(bg->dev->writeback, &bg->gate,
						     readcache_now_us()) != 0) {
			bg->submitted = true;
			readcache_background_submit(bg);
			count++;
		}
	}
	for (io = TAILQ_FIRST(&ch->gated); io != NULL; io = next) {
		next = TAILQ_NEXT(io, link);
		rc = writeback_ready(readcache_device(io->bio)->writeback, &io->gate,
				     readcache_now_us());
		if (rc != 0) {
			TAILQ_REMOVE(&ch->gated, io, link);
			readcache_start(io);
			count++;
		}
	}

	/* Each waiter is completed on its own channel's SPDK thread. The core only
	 * publishes ready flags, so cross-reactor reads require no message allocation. */
	for (io = TAILQ_FIRST(&ch->pending); io != NULL; io = next) {
		next = TAILQ_NEXT(io, link);
		if (readcache_token_ready(&io->token)) {
			TAILQ_REMOVE(&ch->pending, io, link);
			readcache_finish_local(io);
			count++;
		}
	}
	return count ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
readcache_end_io(struct spdk_bdev_io *bio, bool success)
{
	struct readcache_io *io = (void *)bio->driver_ctx;
	struct readcache_bdev *dev = readcache_device(bio);

	if (!success) {
		readcache_record_io_error(dev->cache);
	}

	if (io->token.entry != NULL) {
		readcache_fill(dev->cache, &io->token, bio->u.bdev.iovs, bio->u.bdev.iovcnt,
			       success);
		readcache_release(dev->cache, &io->token);
	}
	if (io->mutating) {
		bool is_write = bio->type == SPDK_BDEV_IO_TYPE_WRITE;

		readcache_end_mutation(dev->cache, &io->mutation,
				       is_write ? bio->u.bdev.iovs : NULL,
				       is_write ? bio->u.bdev.iovcnt : 0,
				       success && bio->type == SPDK_BDEV_IO_TYPE_WRITE &&
					       atomic_load(&dev->populate_on_write) &&
					       readcache_host_buffers(bio));
		io->mutating = false;
	}
	readcache_gate_done(io, success);
}

static void
readcache_background_finish(struct readcache_background *bg, bool success)
{
	struct readcache_bdev *dev = bg->dev;
	struct spdk_io_channel *channel = bg->channel;
	struct readcache_channel *ch = spdk_io_channel_get_ctx(channel);
	uint64_t charge = bg->gate.charge;

	if (bg->mutating) {
		/* W2 publishes its committed payload even when R1 warming is off. */
		readcache_end_mutation(dev->cache, &bg->mutation, &bg->iov, 1, success);
	}
	if (!success) {
		readcache_record_io_error(dev->cache);
		SPDK_ERRLOG("readcache %s: asynchronous write sequence %" PRIu64
			    " failed; namespace requires a new experiment generation\n",
			    dev->bdev.name, bg->gate.sequence);
	}
	TAILQ_REMOVE(&ch->background, bg, link);
	spdk_dma_free(bg->iov.iov_base);
	/* Keep the range reservation through cache publication and DMA release. */
	writeback_leave(dev->writeback, &bg->gate, success, readcache_now_us());
	free(bg);
	writeback_unreserve(dev->writeback, charge);
	spdk_put_io_channel(channel);
}

static void
readcache_background_done(struct spdk_bdev_io *base, bool success, void *arg)
{
	spdk_bdev_free_io(base);
	readcache_background_finish(arg, success);
}

static void
readcache_background_submit(void *arg)
{
	struct readcache_background *bg = arg;
	struct readcache_bdev *dev = bg->dev;
	struct readcache_channel *ch = spdk_io_channel_get_ctx(bg->channel);
	struct spdk_bdev_ext_io_opts opts = {.size = sizeof(opts)};
	int rc;

	if (writeback_ready(dev->writeback, &bg->gate, readcache_now_us()) < 0) {
		readcache_background_finish(bg, false);
		return;
	}
	if (!bg->mutating) {
		readcache_begin_mutation(dev->cache, bg->gate.offset, bg->gate.length,
					 &bg->mutation);
		bg->mutating = true;
	}
	rc = spdk_bdev_writev_blocks_ext(dev->desc, ch->base_ch, &bg->iov, 1,
					 bg->gate.offset / dev->bdev.blocklen,
					 bg->gate.length / dev->bdev.blocklen,
					 readcache_background_done, bg, &opts);
	if (rc == 0) {
		return;
	}
	if (rc == -ENOMEM) {
		bg->wait.bdev = dev->base;
		bg->wait.cb_fn = readcache_background_submit;
		bg->wait.cb_arg = bg;
		if (spdk_bdev_queue_io_wait(dev->base, ch->base_ch, &bg->wait) == 0) {
			return;
		}
	}
	readcache_background_finish(bg, false);
}

static bool
readcache_early_write(struct readcache_io *io)
{
	struct spdk_bdev_io *bio = io->bio;
	struct readcache_bdev *dev = readcache_device(bio);
	struct readcache_background *bg;
	struct readcache_channel *ch = spdk_io_channel_get_ctx(io->ch);
	uint64_t length, copied = 0, charge, alloc_size;
	size_t align = (size_t)1 << dev->bdev.required_alignment;
	int i;

	/* FUA and all other flags/remote buffers retain their exact strict path. */
	if (bio->type != SPDK_BDEV_IO_TYPE_WRITE || !readcache_host_buffers(bio) ||
	    (bio->u.bdev.nvme_cdw12.raw & SPDK_NVME_IO_FLAGS_CDW12_MASK) ||
	    bio->u.bdev.nvme_cdw13.raw) {
		return false;
	}
	length = readcache_length(bio);
	if (!length) {
		return false;
	}
	align = spdk_max(align, (size_t)4096);
	if (length > SIZE_MAX - align - sizeof(*bg)) {
		return false;
	}
	alloc_size = (length + align - 1) / align * align;
	charge = sizeof(*bg) + alloc_size;
	if (!writeback_reserve(dev->writeback, charge, readcache_now_us())) {
		return false;
	}
	bg = calloc(1, sizeof(*bg));
	if (!bg) {
		goto unreserve;
	}
	bg->iov.iov_base = spdk_dma_malloc(alloc_size, align, NULL);
	if (!bg->iov.iov_base) {
		goto free_bg;
	}
	bg->iov.iov_len = length;
	for (i = 0; i < bio->u.bdev.iovcnt && copied < length; i++) {
		size_t n = spdk_min(bio->u.bdev.iovs[i].iov_len, length - copied);

		if (n && !bio->u.bdev.iovs[i].iov_base) {
			goto free_dma;
		}
		if (n) {
			memcpy((char *)bg->iov.iov_base + copied, bio->u.bdev.iovs[i].iov_base, n);
		}
		copied += n;
	}
	if (copied != length) {
		goto free_dma;
	}
	bg->channel = spdk_io_channel_ref(io->ch);
	if (!bg->channel) {
		goto free_dma;
	}
	assert(bg->channel == io->ch);
	bg->dev = dev;
	if (!writeback_enter(dev->writeback, &bg->gate, readcache_offset(bio), length, true, false,
			     charge, readcache_now_us())) {
		spdk_put_io_channel(bg->channel);
		goto free_dma;
	}
	TAILQ_INSERT_TAIL(&ch->background, bg, link);
	/* The channel poller submits independently on its next turn. No background
	 * callback can ever access or complete bio after this completion. */
	spdk_bdev_io_complete(bio, SPDK_BDEV_IO_STATUS_SUCCESS);
	return true;
free_dma:
	spdk_dma_free(bg->iov.iov_base);
free_bg:
	free(bg);
unreserve:
	writeback_unreserve(dev->writeback, charge);
	return false;
}

static void
readcache_base_done(struct spdk_bdev_io *base_io, bool success, void *arg)
{
	struct spdk_bdev_io *bio = arg;
	struct readcache_io *io = (void *)bio->driver_ctx;
	struct readcache_bdev *dev = readcache_device(bio);

	/* This fork's NVMe bdev consumes CDW12 directive bits, but does not map
	 * its FUA bit into NVMe io_flags (RAID may drop it too). In W2, enforce
	 * FUA by completing a real base Flush after the strict write. Hold the
	 * mutation and range reservation through both operations and retries. */
	if (success && dev->writeback && bio->type == SPDK_BDEV_IO_TYPE_WRITE &&
	    (bio->u.bdev.nvme_cdw12.raw & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) && !io->fua_flush) {
		spdk_bdev_free_io(base_io);
		io->fua_flush = true;
		readcache_submit_base(bio);
		return;
	}

	readcache_end_io(bio, success);
	spdk_bdev_io_complete_base_io_status(bio, base_io);
	spdk_bdev_free_io(base_io);
}

static void
readcache_resubmit(void *arg)
{
	/* Keep the miss leader or mutation reservation across ENOMEM retries. */
	readcache_submit_base(arg);
}

static void
readcache_submit_base(struct spdk_bdev_io *bio)
{
	struct readcache_io *io = (void *)bio->driver_ctx;
	struct readcache_channel *ch = spdk_io_channel_get_ctx(io->ch);
	struct readcache_bdev *dev = readcache_device(bio);
	struct spdk_bdev_ext_io_opts opts = {.size = sizeof(opts)};
	uint64_t offset = 0, length = 0;
	int rc;

	/* RESET uses another union member. Do not inspect its uninitialized bdev
	 * payload as if it were a read/write request. */
	if (bio->type != SPDK_BDEV_IO_TYPE_RESET) {
		offset = bio->u.bdev.offset_blocks;
		length = bio->u.bdev.num_blocks;
	}
	if (bio->type == SPDK_BDEV_IO_TYPE_READ || bio->type == SPDK_BDEV_IO_TYPE_WRITE) {
		opts.memory_domain = bio->u.bdev.memory_domain;
		opts.memory_domain_ctx = bio->u.bdev.memory_domain_ctx;
		opts.metadata = bio->u.bdev.md_buf;
		opts.accel_sequence = bio->u.bdev.accel_sequence;
		opts.dif_check_flags_exclude_mask = ~bio->u.bdev.dif_check_flags;
		opts.nvme_cdw12 = bio->u.bdev.nvme_cdw12;
		opts.nvme_cdw13 = bio->u.bdev.nvme_cdw13;
	}

	if (io->fua_flush) {
		rc = spdk_bdev_flush_blocks(dev->desc, ch->base_ch, offset, length,
					    readcache_base_done, bio);
		goto submitted;
	}
	switch (bio->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rc = spdk_bdev_readv_blocks_ext(dev->desc, ch->base_ch, bio->u.bdev.iovs,
						bio->u.bdev.iovcnt, offset, length,
						readcache_base_done, bio, &opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_writev_blocks_ext(dev->desc, ch->base_ch, bio->u.bdev.iovs,
						 bio->u.bdev.iovcnt, offset, length,
						 readcache_base_done, bio, &opts);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(dev->desc, ch->base_ch, offset, length,
					    readcache_base_done, bio);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(dev->desc, ch->base_ch, offset, length,
						   readcache_base_done, bio);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(dev->desc, ch->base_ch, offset, length,
					    readcache_base_done, bio);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(dev->desc, ch->base_ch, readcache_base_done, bio);
		break;
	default:
		rc = -ENOTSUP;
		break;
	}
submitted:
	if (rc == 0) {
		return;
	}
	if (rc == -ENOMEM) {
		io->wait.bdev = dev->base;
		io->wait.cb_fn = readcache_resubmit;
		io->wait.cb_arg = bio;
		rc = spdk_bdev_queue_io_wait(dev->base, ch->base_ch, &io->wait);
		if (rc == 0) {
			return;
		}
	}
	readcache_end_io(bio, false);
	spdk_bdev_io_complete(bio, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
readcache_read_ready(struct spdk_io_channel *channel, struct spdk_bdev_io *bio, bool success)
{
	struct readcache_io *io = (void *)bio->driver_ctx;
	struct readcache_bdev *dev = readcache_device(bio);
	struct readcache_channel *ch = spdk_io_channel_get_ctx(channel);
	enum readcache_result result;

	if (!success) {
		readcache_record_io_error(dev->cache);
		readcache_gate_done(io, false);
		spdk_bdev_io_complete(bio, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (!readcache_host_buffers(bio) || bio->u.bdev.nvme_cdw12.raw != 0 ||
	    bio->u.bdev.nvme_cdw13.raw != 0) {
		readcache_record_external_bypass(dev->cache, readcache_length(bio));
		readcache_submit_base(bio);
		return;
	}
	result = readcache_acquire(dev->cache, readcache_offset(bio), readcache_length(bio),
				   &io->token);
	if (result == READCACHE_HIT) {
		readcache_finish_local(io);
	} else if (result == READCACHE_WAIT) {
		TAILQ_INSERT_TAIL(&ch->pending, io, link);
	} else {
		readcache_submit_base(bio);
	}
}

static void
readcache_start(struct readcache_io *io)
{
	struct spdk_bdev_io *bio = io->bio;
	struct readcache_bdev *dev = readcache_device(bio);
	uint64_t offset, length;

	if (io->gated && writeback_ready(dev->writeback, &io->gate, readcache_now_us()) < 0) {
		readcache_end_io(bio, false);
		spdk_bdev_io_complete(bio, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (bio->type == SPDK_BDEV_IO_TYPE_READ) {
		spdk_bdev_io_get_buf(bio, readcache_read_ready, readcache_length(bio));
		return;
	}
	switch (bio->type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		offset = bio->type == SPDK_BDEV_IO_TYPE_RESET ? 0 : readcache_offset(bio);
		length = bio->type == SPDK_BDEV_IO_TYPE_RESET ? UINT64_MAX : readcache_length(bio);
		readcache_begin_mutation(dev->cache, offset, length, &io->mutation);
		io->mutating = true;
		break;
	default:
		break;
	}
	readcache_submit_base(bio);
}

static void
readcache_submit(struct spdk_io_channel *channel, struct spdk_bdev_io *bio)
{
	struct readcache_io *io = (void *)bio->driver_ctx;
	struct readcache_bdev *dev = readcache_device(bio);
	struct readcache_channel *ch = spdk_io_channel_get_ctx(channel);
	bool global = bio->type == SPDK_BDEV_IO_TYPE_RESET || bio->type == SPDK_BDEV_IO_TYPE_FLUSH;

	memset(io, 0, sizeof(*io));
	io->bio = bio;
	io->ch = channel;
	if (dev->writeback) {
		if (readcache_early_write(io)) {
			return;
		}
		writeback_enter(dev->writeback, &io->gate, global ? 0 : readcache_offset(bio),
				global ? UINT64_MAX : readcache_length(bio),
				bio->type != SPDK_BDEV_IO_TYPE_READ, global, 0, readcache_now_us());
		io->gated = true;
		if (writeback_ready(dev->writeback, &io->gate, readcache_now_us()) == 0) {
			TAILQ_INSERT_TAIL(&ch->gated, io, link);
			return;
		}
	}
	readcache_start(io);
}

static bool
readcache_io_supported(void *ctx, enum spdk_bdev_io_type type)
{
	struct readcache_bdev *dev = ctx;

	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return spdk_bdev_io_type_supported(dev->base, type);
	default:
		/* Do not advertise commands whose writes or buffer ownership we do not
		 * interpose: COPY, fused compare/write, passthrough, ABORT and ZCOPY. */
		return false;
	}
}

static struct spdk_io_channel *
readcache_get_channel(void *ctx)
{
	return spdk_get_io_channel(ctx);
}

static int
readcache_channel_create(void *ctx, void *buf)
{
	struct readcache_bdev *dev = ctx;
	struct readcache_channel *ch = buf;

	TAILQ_INIT(&ch->pending);
	TAILQ_INIT(&ch->gated);
	TAILQ_INIT(&ch->background);
	ch->base_ch = spdk_bdev_get_io_channel(dev->desc);
	if (ch->base_ch == NULL) {
		return -ENOMEM;
	}
	ch->poller = SPDK_POLLER_REGISTER(readcache_poll, ch, 0);
	if (ch->poller == NULL) {
		spdk_put_io_channel(ch->base_ch);
		return -ENOMEM;
	}
	return 0;
}

static void
readcache_channel_destroy(void *ctx, void *buf)
{
	struct readcache_channel *ch = buf;

	assert(TAILQ_EMPTY(&ch->pending));
	assert(TAILQ_EMPTY(&ch->gated));
	assert(TAILQ_EMPTY(&ch->background));
	spdk_poller_unregister(&ch->poller);
	spdk_put_io_channel(ch->base_ch);
}

static void
readcache_unregister_done(void *ctx)
{
	struct readcache_bdev *dev = ctx;
	int rc;

	rc = readcache_destroy(dev->cache);
	assert(rc == 0);
	if (rc != 0) {
		/* Never release an owner while its cache still has live references. */
		return;
	}
	if (dev->writeback) {
		writeback_destroy(dev->writeback);
	}
	if (dev->destructing) {
		spdk_bdev_destruct_done(&dev->bdev, 0);
	}
	free(dev->bdev.name);
	free(dev);
}

static int
readcache_cleanup_poll(void *ctx)
{
	struct readcache_bdev *dev = ctx;

	if (!atomic_load_explicit(&dev->channels_gone, memory_order_acquire)) {
		return SPDK_POLLER_IDLE;
	}
	assert(dev->open_thread == spdk_get_thread());
	spdk_poller_unregister(&dev->cleanup_poller);
	spdk_bdev_module_release_bdev(dev->base);
	spdk_bdev_close(dev->desc);
	readcache_unregister_done(dev);
	return SPDK_POLLER_BUSY;
}

static void
readcache_channels_gone(void *ctx)
{
	struct readcache_bdev *dev = ctx;

	/* The poller was allocated on the descriptor owner at create time. Teardown
	 * therefore needs no cross-thread message allocation under memory pressure. */
	atomic_store_explicit(&dev->channels_gone, true, memory_order_release);
}

static int
readcache_destruct(void *ctx)
{
	struct readcache_bdev *dev = ctx;

	TAILQ_REMOVE(&g_readcache, dev, link);
	dev->destructing = true;
	/* The descriptor and cache must outlive every channel, base I/O and merged
	 * reader. Signal the bdev core only when the asynchronous cleanup finishes. */
	spdk_io_device_unregister(dev, readcache_channels_gone);
	return 1;
}

static void
readcache_write_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct readcache_bdev *dev = bdev->ctxt;
	struct readcache_stats s;

	readcache_get_stats(dev->cache, &s);
	spdk_json_write_named_string(w, "method", "bdev_readcache_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "base_bdev_name", dev->base->name);
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "capacity_bytes", s.capacity_bytes);
	spdk_json_write_named_uint64(w, "max_io_bytes", s.max_io_bytes);
	spdk_json_write_named_bool(w, "populate_on_write", atomic_load(&dev->populate_on_write));
	spdk_json_write_named_bool(w, "enabled", s.enabled);
	if (dev->writeback) {
		struct writeback_stats wb;

		writeback_get_stats(dev->writeback, &wb, readcache_now_us());
		spdk_json_write_named_string(w, "write_ack_policy", "volatile_early_ack_wait_read");
		spdk_json_write_named_uint64(w, "dirty_capacity_bytes", wb.capacity_bytes);
		spdk_json_write_named_uint64(w, "max_pending_writes", wb.max_pending_writes);
		spdk_json_write_named_uint64(w, "max_dirty_age_us", wb.max_dirty_age_us);
	}
	spdk_json_write_object_end(w);
}

static int
readcache_dump_info(void *ctx, struct spdk_json_write_ctx *w)
{
	struct readcache_bdev *dev = ctx;
	struct readcache_stats s;

	readcache_get_stats(dev->cache, &s);
	spdk_json_write_named_object_begin(w, "readcache");
	spdk_json_write_named_string(w, "base_bdev_name", dev->base->name);
	spdk_json_write_named_uint64(w, "capacity_bytes", s.capacity_bytes);
	spdk_json_write_named_uint64(w, "allocated_bytes", s.allocated_bytes);
	spdk_json_write_named_bool(w, "populate_on_write", atomic_load(&dev->populate_on_write));
	spdk_json_write_named_bool(w, "enabled", s.enabled);
	spdk_json_write_object_end(w);
	return 0;
}

static const struct spdk_bdev_fn_table readcache_fn_table = {
	.destruct = readcache_destruct,
	.submit_request = readcache_submit,
	.io_type_supported = readcache_io_supported,
	.get_io_channel = readcache_get_channel,
	.dump_info_json = readcache_dump_info,
	.write_config_json = readcache_write_config,
};

static void
readcache_base_event(enum spdk_bdev_event_type type, struct spdk_bdev *base, void *ctx)
{
	struct readcache_bdev *dev = ctx;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		const struct readcache_options options = {.has_enabled = true, .enabled = false};

		readcache_configure(dev->cache, &options);
		if (dev->writeback) {
			writeback_poison(dev->writeback);
		}
		spdk_bdev_unregister(&dev->bdev, NULL, NULL);
	} else if (type == SPDK_BDEV_EVENT_RESIZE) {
		struct readcache_mutation mutation;

		if (dev->writeback) {
			/* A generation/geometry change cannot silently recycle dirty LBAs. */
			writeback_poison(dev->writeback);
		}

		readcache_begin_mutation(dev->cache, 0, UINT64_MAX, &mutation);
		readcache_end_mutation(dev->cache, &mutation, NULL, 0, false);
		spdk_bdev_notify_blockcnt_change(&dev->bdev, spdk_bdev_get_num_blocks(base));
	}
}

int
bdev_readcache_create(const char *base_name, const char *name, uint64_t capacity, uint64_t max_io,
		      bool populate_on_write)
{
	return bdev_readcache_create_ext(base_name, name, capacity, max_io, populate_on_write, NULL);
}

int
bdev_readcache_create_ext(const char *base_name, const char *name, uint64_t capacity,
			  uint64_t max_io, bool populate_on_write,
			  const struct writeback_options *writeback)
{
	struct readcache_bdev *dev;
	int rc;

	if (base_name == NULL || name == NULL || !*name || strcmp(base_name, name) == 0) {
		return -EINVAL;
	}
	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -ENOMEM;
	}
	dev->bdev.name = strdup(name);
	dev->cache = readcache_create(capacity, max_io);
	if (writeback) {
		dev->writeback = writeback_create(writeback);
	}
	if (dev->bdev.name == NULL || dev->cache == NULL || (writeback && !dev->writeback)) {
		rc = -EINVAL;
		goto free_dev;
	}
	rc = spdk_bdev_open_ext(base_name, true, readcache_base_event, dev, &dev->desc);
	if (rc != 0) {
		goto free_dev;
	}
	dev->base = spdk_bdev_desc_get_bdev(dev->desc);
	/* No metadata, zoned semantics, or raw device commands in this first host
	 * buffer implementation. Never silently weaken a base device's contract. */
	if (dev->base->md_len != 0 || dev->base->zoned || dev->base->blocklen == 0 ||
	    max_io < dev->base->blocklen || max_io % dev->base->blocklen != 0 ||
	    dev->base->blockcnt > UINT64_MAX / dev->base->blocklen) {
		rc = -ENOTSUP;
		goto close_dev;
	}
	if (writeback && (dev->base->required_alignment > 20 ||
			  !spdk_bdev_io_type_supported(dev->base, SPDK_BDEV_IO_TYPE_FLUSH))) {
		rc = -ENOTSUP;
		goto close_dev;
	}
	dev->open_thread = spdk_get_thread();
	atomic_init(&dev->populate_on_write, populate_on_write);
	dev->bdev.product_name = "readcache";
	dev->bdev.blocklen = dev->base->blocklen;
	dev->bdev.blockcnt = dev->base->blockcnt;
	dev->bdev.write_cache = writeback != NULL || dev->base->write_cache;
	dev->bdev.required_alignment = dev->base->required_alignment;
	dev->bdev.optimal_io_boundary = dev->base->optimal_io_boundary;
	dev->bdev.split_on_optimal_io_boundary = dev->base->split_on_optimal_io_boundary;
	dev->bdev.write_unit_size = dev->base->write_unit_size;
	dev->bdev.split_on_write_unit = dev->base->split_on_write_unit;
	dev->bdev.max_rw_size = dev->base->max_rw_size;
	dev->bdev.numa = dev->base->numa;
	spdk_uuid_generate(&dev->bdev.uuid);
	dev->bdev.ctxt = dev;
	dev->bdev.module = &readcache_module;
	dev->bdev.fn_table = &readcache_fn_table;
	rc = spdk_bdev_module_claim_bdev(dev->base, dev->desc, &readcache_module);
	if (rc != 0) {
		goto close_dev;
	}
	atomic_init(&dev->channels_gone, false);
	dev->cleanup_poller = SPDK_POLLER_REGISTER(readcache_cleanup_poll, dev, 1000);
	if (dev->cleanup_poller == NULL) {
		spdk_bdev_module_release_bdev(dev->base);
		rc = -ENOMEM;
		goto close_dev;
	}
	spdk_io_device_register(dev, readcache_channel_create, readcache_channel_destroy,
				sizeof(struct readcache_channel), name);
	rc = spdk_bdev_register(&dev->bdev);
	if (rc != 0) {
		spdk_io_device_unregister(dev, readcache_channels_gone);
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_readcache, dev, link);
	return 0;
close_dev:
	spdk_bdev_close(dev->desc);
free_dev:
	if (dev->writeback) {
		writeback_destroy(dev->writeback);
	}
	if (dev->cache != NULL) {
		readcache_destroy(dev->cache);
	}
	free(dev->bdev.name);
	free(dev);
	return rc;
}

void
bdev_readcache_delete(const char *name, spdk_bdev_unregister_cb cb, void *arg)
{
	int rc = spdk_bdev_unregister_by_name(name, &readcache_module, cb, arg);

	if (rc != 0) {
		cb(arg, rc);
	}
}

int
bdev_readcache_get_stats(const char *name, struct readcache_stats *stats)
{
	struct readcache_bdev *dev;

	TAILQ_FOREACH (dev, &g_readcache, link) {
		if (strcmp(dev->bdev.name, name) == 0) {
			readcache_get_stats(dev->cache, stats);
			stats->populate_on_write = atomic_load(&dev->populate_on_write);
			if (dev->writeback) {
				writeback_get_stats(dev->writeback, &stats->writeback, readcache_now_us());
			}
			return 0;
		}
	}
	return -ENODEV;
}

int
bdev_readcache_set_options(const char *name, const struct readcache_options *options)
{
	struct readcache_bdev *dev;
	int rc;

	TAILQ_FOREACH (dev, &g_readcache, link) {
		if (strcmp(dev->bdev.name, name) == 0) {
			rc = readcache_configure(dev->cache, options);
			if (rc == 0 && options->has_populate_on_write) {
				atomic_store(&dev->populate_on_write, options->populate_on_write);
			}
			return rc;
		}
	}
	return -ENODEV;
}
