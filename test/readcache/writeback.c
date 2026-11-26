/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise the production bdev adapter, including real cache and ordering
 * code. Only SPDK framework/DMA/backend are local deterministic mocks. */
#define main original_bdev_main
#include "bdev.c"
#undef main
#include <pthread.h>
#include <sched.h>

static struct spdk_io_channel *channels[16];
static struct readcache_bdev *dev;

static void
poll_channel(unsigned int i)
{
	current_channel = (uintptr_t)channels[i];
	readcache_poll(spdk_io_channel_get_ctx(channels[i]));
}

static void
open_writeback(uint64_t capacity, uint64_t slots)
{
	const struct writeback_options o = {
		.capacity_bytes = capacity, .max_pending_writes = slots, .max_dirty_age_us = 1000,
	};
	unsigned int i;

	assert(!pending_count && !queued && !dma_live && !channel_holds);
	framework_base = (struct spdk_bdev){.blocklen = BLOCK, .blockcnt = 8, .name = "base"};
	assert(!bdev_readcache_create_ext("base", "wb", 128 * 1024, BLOCK, false, &o));
	dev = framework_registered->ctxt;
	assert(dev->bdev.write_cache); /* VWC remains true even with a write-through base. */
	for (i = 0; i < SPDK_COUNTOF(channels); i++) {
		channels[i] = readcache_get_channel(dev);
		assert(channels[i]);
	}
	memset(disk, 0x11, sizeof(disk));
	reads = writes = completed = 0;
	queue_error = submit_error = 0;
}

static struct writeback_stats
stats(void)
{
	struct readcache_stats s;

	assert(!bdev_readcache_get_stats("wb", &s));
	assert(s.writeback.accepted == s.writeback.committed + s.writeback.failed +
	       s.writeback.pending);
	assert(s.writeback.accepted_bytes == s.writeback.committed_bytes + s.writeback.failed_bytes +
	       s.writeback.pending_bytes);
	assert(s.writeback.allocated_bytes <= s.writeback.capacity_bytes);
	return s.writeback;
}

static void
close_writeback(void)
{
	struct writeback_stats s = stats();
	unsigned int i;
	int rc = -999;

	assert(!s.pending && !s.allocated_bytes && !dma_live && !channel_holds);
	assert(!s.active_operations);
	for (i = 0; i < SPDK_COUNTOF(channels); i++) {
		framework_destroy(dev, spdk_io_channel_get_ctx(channels[i]));
		free(channels[i]);
	}
	bdev_readcache_delete("wb", deleted, &rc);
	framework_tick_cleanup();
	assert(!rc && !opens && !claims && !io_devices);
}

static void
write_and_release(unsigned int channel, uint64_t offset, unsigned char value)
{
	struct spdk_bdev_io *w = request(dev, channels[channel], SPDK_BDEV_IO_TYPE_WRITE, offset, value);

	submit(w);
	assert(w->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* Reuse and free the complete foreground request and original iobuf
	 * before DMA submission. ASan catches any background reference to them. */
	memset(w->u.bdev.iovs[0].iov_base, 0xee, BLOCK);
	check_done(w, 0, true);
}

static void
fanout_and_ordering(void)
{
	struct spdk_bdev_io *r[10], *latest;
	struct spdk_bdev_io *many[64];
	struct writeback_stats s;
	unsigned int i;

	open_writeback(128 * 1024, 32);
	write_and_release(0, 0, 0x41);
	assert(!writes && stats().pending == 1 && dma_live == 1 && channel_holds == 1);
	for (i = 0; i < 10; i++) {
		r[i] = request(dev, channels[i + 1], SPDK_BDEV_IO_TYPE_READ, 0, 0);
		submit(r[i]);
		assert(r[i]->internal.status == SPDK_BDEV_IO_STATUS_PENDING);
	}
	write_and_release(11, 0, 0x42);
	/* Poll the younger writer first: it cannot overtake the queued write or
	 * the ten reads. An unrelated LBA remains independently runnable. */
	poll_channel(11);
	assert(!pending_count && !reads);
	write_and_release(12, 1, 0x51);
	poll_channel(12);
	poll_channel(0);
	assert(pending_count == 2 && writes == 2);
	finish(0, true); /* unrelated completion may arrive first */
	finish(0, true);
	poll_channel(11);
	assert(!pending_count);
	for (i = 0; i < 10; i++) {
		poll_channel(i + 1);
		check_done(r[i], 0x41, true);
	}
	assert(!reads);
	poll_channel(11);
	assert(pending_count == 1);
	finish(0, true);
	latest = request(dev, channels[1], SPDK_BDEV_IO_TYPE_READ, 0, 0);
	submit(latest);
	check_done(latest, 0x42, true);
	s = stats();
	assert(s.accepted == 3 && s.committed == 3 && !s.failed && s.read_waits == 10);
	write_and_release(0, 0, 0x43);
	for (i = 0; i < SPDK_COUNTOF(many); i++) {
		many[i] = request(dev, channels[i % 16], SPDK_BDEV_IO_TYPE_READ, 0, 0);
		submit(many[i]);
	}
	poll_channel(0);
	finish(0, true);
	for (i = 0; i < 16; i++) {
		poll_channel(i);
	}
	for (i = 0; i < SPDK_COUNTOF(many); i++) {
		check_done(many[i], 0x43, true);
	}
	assert(!reads && stats().read_waits == 74);
	close_writeback();
	puts("PASS W2: early ACK, destroyed source/request, ten gated readers, committed cache hits, "
	     "younger overwrite fairness and independent LBA completion");
}

static void
multi_range_and_scatter(void)
{
	struct spdk_bdev_io *r, *w;
	unsigned char *data;
	unsigned int i;

	open_writeback(128 * 1024, 32);
	write_and_release(0, 0, 0xc1);
	write_and_release(1, 1, 0xc2);
	r = request(dev, channels[2], SPDK_BDEV_IO_TYPE_READ, 0, 0);
	r->u.bdev.iovs[0].iov_base = realloc(r->u.bdev.iovs[0].iov_base, 2 * BLOCK);
	r->u.bdev.iovs[0].iov_len = 2 * BLOCK;
	r->u.bdev.num_blocks = 2;
	submit(r);
	write_and_release(3, 1, 0xc3);
	poll_channel(0);
	poll_channel(1);
	finish(1, true); /* out-of-order disjoint completions */
	poll_channel(2);
	assert(pending_count == 1);
	finish(0, true);
	poll_channel(2);
	poll_channel(3);
	assert(pending_count == 1 && pending[0].bio->type == SPDK_BDEV_IO_TYPE_READ);
	finish(0, true);
	data = r->u.bdev.iovs[0].iov_base;
	for (i = 0; i < 2 * BLOCK; i++) {
		assert(data[i] == (i < BLOCK ? 0xc1 : 0xc2));
	}
	check_done(r, 0xc1, true);
	poll_channel(3);
	finish(0, true);
	assert(disk[BLOCK] == 0xc3);

	w = request(dev, channels[0], SPDK_BDEV_IO_TYPE_WRITE, 0, 0xc4);
	w->u.bdev.iovs[0].iov_base = realloc(w->u.bdev.iovs[0].iov_base, 2 * BLOCK);
	memset(w->u.bdev.iovs[0].iov_base, 0xc4, 2 * BLOCK);
	w->u.bdev.iovs[0].iov_len = 2 * BLOCK;
	w->u.bdev.num_blocks = 2;
	/* NVMf passes the full CDW12 including zero-based NLB. These low bits
	 * must not force every real multi-block write onto the strict path. */
	w->u.bdev.nvme_cdw12.raw = 1;
	submit(w);
	check_done(w, 0, true);
	write_and_release(1, 1, 0xc5);
	poll_channel(1);
	assert(!pending_count);
	poll_channel(0);
	finish(0, true);
	poll_channel(1);
	finish(0, true);
	for (i = 0; i < 2 * BLOCK; i++) {
		assert(disk[i] == (i < BLOCK ? 0xc4 : 0xc5));
	}
	w = request(dev, channels[0], SPDK_BDEV_IO_TYPE_WRITE, 3, 0xc6);
	w->u.bdev.iovs = realloc(w->u.bdev.iovs, 2 * sizeof(struct iovec));
	w->u.bdev.iovs[0].iov_len = BLOCK / 2;
	w->u.bdev.iovs[1] = (struct iovec) {
		.iov_base = (char *)w->u.bdev.iovs[0].iov_base + BLOCK / 2, .iov_len = BLOCK / 2,
	};
	w->u.bdev.iovcnt = 2;
	submit(w);
	memset(w->u.bdev.iovs[0].iov_base, 0xee, BLOCK);
	check_done(w, 0, true);
	poll_channel(0);
	finish(0, true);
	for (i = 3 * BLOCK; i < 4 * BLOCK; i++) {
		assert(disk[i] == 0xc6);
	}
	close_writeback();
	puts("PASS W2: 64 readers, multi-range read waits for all prior writes, partial overlapping "
	     "writes preserve media order, complete scatter payload ownership");
}

static void
barriers_and_bypasses(void)
{
	struct spdk_bdev_io *flush, *fua, *r, *zero;
	const struct readcache_options off = {.has_enabled = true, .enabled = false, .clear = true,
		.reset_stats = true};
	unsigned int mode;

	for (mode = 0; mode < 4; mode++) {
		open_writeback(128 * 1024, 32);
		write_and_release(0, 0, 0x63);
		if (mode == 0) {
			assert(!bdev_readcache_set_options("wb", &off));
		}
		r = request(dev, channels[1], SPDK_BDEV_IO_TYPE_READ, 0, 0);
		if (mode == 1) {
			r->u.bdev.nvme_cdw12.raw = 1;
		} else if (mode == 2) {
			r->u.bdev.memory_domain = (void *)1;
		} else if (mode == 3) {
			r->u.bdev.iovs[0].iov_base = realloc(r->u.bdev.iovs[0].iov_base, BLOCK * 2);
			r->u.bdev.iovs[0].iov_len = BLOCK * 2;
			r->u.bdev.num_blocks = 2; /* exceeds CLEAN max_io, still gated */
		}
		submit(r);
		assert(!pending_count && stats().pending == 1);
		poll_channel(1);
		assert(!pending_count);
		poll_channel(0);
		finish(0, true);
		poll_channel(1);
		assert(pending_count == 1 && reads == 1);
		finish(0, true);
		check_done(r, 0x63, true); /* check_done checks first block */
		close_writeback();
	}

	open_writeback(128 * 1024, 32);
	write_and_release(0, 0, 0x71);
	flush = request(dev, channels[1], SPDK_BDEV_IO_TYPE_FLUSH, 7, 0);
	submit(flush); /* namespace-wide even if flush range is unrelated */
	write_and_release(2, 2, 0x72);
	poll_channel(2);
	poll_channel(1);
	assert(!pending_count);
	poll_channel(0);
	finish(0, true);
	poll_channel(1);
	assert(pending_count == 1 && pending[0].bio->type == SPDK_BDEV_IO_TYPE_FLUSH);
	poll_channel(2);
	assert(pending_count == 1);
	finish(0, true);
	check_done(flush, 0, true);
	poll_channel(2);
	finish(0, true);
	fua = request(dev, channels[3], SPDK_BDEV_IO_TYPE_WRITE, 2, 0x73);
	fua->u.bdev.nvme_cdw12.raw = 1u << 30;
	submit(fua);
	assert(fua->internal.status == SPDK_BDEV_IO_STATUS_PENDING && stats().accepted == 2);
	assert(pending[0].bio->u.bdev.nvme_cdw12.raw == 1u << 30);
	submit_error = -ENOMEM; /* resource pressure for the required FUA Flush */
	finish(0, true);
	assert(fua->internal.status == SPDK_BDEV_IO_STATUS_PENDING && queued && !pending_count);
	{
		struct spdk_bdev_io_wait_entry *entry = queued;

		queued = NULL;
		entry->cb_fn(entry->cb_arg);
	}
	assert(pending_count == 1 && pending[0].bio->type == SPDK_BDEV_IO_TYPE_FLUSH);
	finish(0, true);
	check_done(fua, 0, true);

	write_and_release(0, 2, 0x74);
	zero = request(dev, channels[4], SPDK_BDEV_IO_TYPE_WRITE_ZEROES, 2, 0);
	submit(zero);
	write_and_release(5, 2, 0x75);
	poll_channel(5);
	assert(!pending_count);
	poll_channel(0);
	finish(0, true);
	poll_channel(4);
	assert(pending_count == 1 && pending[0].bio->type == SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	finish(0, true);
	check_done(zero, 0, true);
	poll_channel(5);
	finish(0, true);
	assert(disk[2 * BLOCK] == 0x75);
	close_writeback();
	puts("PASS W2: disabled/cleared cache, special flags, external buffers and oversize reads "
	     "obey gate; Flush covers queued writes, FUA stays strict, zeroes ordered");
}

static void
control_and_removal(void)
{
	struct spdk_bdev_io *bio;
	struct iovec *saved_iov;
	enum spdk_bdev_io_type types[] = {SPDK_BDEV_IO_TYPE_UNMAP, SPDK_BDEV_IO_TYPE_RESET};
	unsigned int i;

	for (i = 0; i < SPDK_COUNTOF(types); i++) {
		open_writeback(128 * 1024, 32);
		write_and_release(0, 0, 0xb1);
		bio = request(dev, channels[1], types[i], 0, 0);
		saved_iov = bio->u.bdev.iovs;
		if (types[i] == SPDK_BDEV_IO_TYPE_RESET) {
			/* RESET has no initialized bdev union payload. */
			memset(&bio->u.bdev, 0xa5, sizeof(bio->u.bdev));
		}
		submit(bio);
		write_and_release(2, 0, 0xb2);
		poll_channel(2);
		assert(!pending_count);
		poll_channel(0);
		finish(0, true);
		poll_channel(1);
		assert(pending_count == 1 && pending[0].bio->type == types[i]);
		finish(0, true);
		if (types[i] == SPDK_BDEV_IO_TYPE_RESET) {
			/* Restore only the harness-owned buffer before its cleanup. */
			bio->u.bdev.iovs = saved_iov;
		}
		check_done(bio, 0, true);
		poll_channel(2);
		finish(0, true);
		assert(disk[0] == 0xb2);
		close_writeback();
	}

	open_writeback(128 * 1024, 32);
	write_and_release(0, 0, 0xb3);
	submit_error = -ENOMEM;
	poll_channel(0);
	assert(queued);
	{
		struct spdk_bdev_io_wait_entry *entry = queued;

		queued = NULL;
		entry->cb_fn(entry->cb_arg);
	}
	finish(0, true);
	assert(stats().committed == 1 && !stats().failed);
	write_and_release(0, 0, 0xb4);
	framework_event(SPDK_BDEV_EVENT_RESIZE, &framework_base, framework_event_ctx);
	poll_channel(0);
	assert(stats().poisoned && stats().failed == 1 && !dma_live);
	close_writeback();

	open_writeback(128 * 1024, 32);
	write_and_release(0, 0, 0xb5);
	poll_channel(0); /* one in-flight and one queued background write */
	write_and_release(1, 1, 0xb6);
	defer_unregister = true;
	framework_event(SPDK_BDEV_EVENT_REMOVE, &framework_base, framework_event_ctx);
	poll_channel(1);
	assert(dma_live == 1 && channel_holds == 1);
	finish(0, false);
	assert(!dma_live && !channel_holds);
	for (i = 0; i < SPDK_COUNTOF(channels); i++) {
		framework_destroy(dev, spdk_io_channel_get_ctx(channels[i]));
		free(channels[i]);
	}
	defer_unregister = false;
	io_devices--;
	framework_device = NULL;
	deferred_unregister(deferred_device);
	deferred_unregister = NULL;
	framework_tick_cleanup();
	assert(!opens && !claims && !io_devices);
	puts("PASS W2: UNMAP/RESET ordering, successful resource retry, resize generation poison "
	     "and hot removal drain queued/inflight background ownership");
}

static void
pressure_and_errors(void)
{
	struct spdk_bdev_io *w, *r, *flush;
	struct writeback_stats s;
	const struct readcache_options clear = {.clear = true, .reset_stats = true};
	unsigned int fault;

	open_writeback(8192, 1);
	write_and_release(0, 0, 0x81);
	w = request(dev, channels[1], SPDK_BDEV_IO_TYPE_WRITE, 0, 0x82);
	submit(w);
	assert(w->internal.status == SPDK_BDEV_IO_STATUS_PENDING && stats().backpressure == 1);
	poll_channel(0);
	finish(0, true);
	poll_channel(1);
	finish(0, true);
	check_done(w, 0, true);
	fail_dma = true;
	w = request(dev, channels[1], SPDK_BDEV_IO_TYPE_WRITE, 0, 0x83);
	submit(w);
	assert(w->internal.status == SPDK_BDEV_IO_STATUS_PENDING && !dma_live);
	finish(0, true);
	check_done(w, 0, true);
	write_and_release(0, 0, 0x84);
	test_now_us += 1001;
	w = request(dev, channels[1], SPDK_BDEV_IO_TYPE_WRITE, 1, 0x85);
	submit(w);
	assert(w->internal.status == SPDK_BDEV_IO_STATUS_PENDING && stats().throttled);
	finish(0, true); /* independent strict write can drain during backpressure */
	check_done(w, 0, true);
	poll_channel(0);
	finish(0, true);
	write_and_release(0, 0, 0x86); /* admission recovers after draining */
	poll_channel(0);
	finish(0, true);
	close_writeback();

	for (fault = 0; fault < 4; fault++) {
		open_writeback(128 * 1024, 32);
		write_and_release(0, 0, 0x91);
		write_and_release(1, 0, 0x92);
		r = request(dev, channels[2], SPDK_BDEV_IO_TYPE_READ, 0, 0);
		submit(r);
		if (fault == 0 || fault == 1) {
			submit_error = -ENOMEM;
			queue_error = fault == 1 ? -EINVAL : 0;
		} else if (fault == 2) {
			submit_error = -EIO;
		}
		poll_channel(0);
		if (fault == 0) {
			struct spdk_bdev_io_wait_entry *entry = queued;

			assert(entry && !pending_count);
			queued = NULL;
			entry->cb_fn(entry->cb_arg);
			finish(0, false);
		} else if (fault == 3) {
			finish(0, false);
		}
		poll_channel(1); /* queued ACKed write fails explicitly, not silently discarded */
		poll_channel(2);
		check_done(r, 0, false);
		assert(!reads && disk[0] == 0x11);
		s = stats();
		assert(s.accepted == 2 && s.failed == 2 && !s.pending && s.poisoned);
		assert(!bdev_readcache_set_options("wb", &clear));
		assert(stats().failed == 2 && stats().poisoned); /* reset cannot erase evidence */
		flush = request(dev, channels[1], SPDK_BDEV_IO_TYPE_FLUSH, 0, 0);
		submit(flush);
		check_done(flush, 0, false);
		w = request(dev, channels[1], SPDK_BDEV_IO_TYPE_WRITE, 1, 0x99);
		submit(w);
		check_done(w, 0, false);
		assert(!pending_count);
		close_writeback();
	}
	open_writeback(128 * 1024, 32);
	w = request(dev, channels[0], SPDK_BDEV_IO_TYPE_WRITE, 0, 0x99);
	w->u.bdev.nvme_cdw12.raw = 1u << 30;
	submit(w);
	r = request(dev, channels[1], SPDK_BDEV_IO_TYPE_READ, 0, 0);
	submit(r);
	finish(0, true);
	assert(w->internal.status == SPDK_BDEV_IO_STATUS_PENDING);
	assert(pending_count == 1 && pending[0].bio->type == SPDK_BDEV_IO_TYPE_FLUSH);
	finish(0, false); /* failed durability condition must fail the original FUA write */
	check_done(w, 0, false);
	poll_channel(1);
	check_done(r, 0, false);
	assert(stats().poisoned && !reads);
	close_writeback();
	puts("PASS W2: bounded slots/DMA, synchronous fallback, allocation failure, age backpressure "
	     "and recovery; ENOMEM retry, queue/submit/completion failure poison without stale reads");
}

static void
background_lifetime(void)
{
	int rc = -999;
	unsigned int i;

	open_writeback(128 * 1024, 32);
	write_and_release(0, 0, 0xa1);
	defer_unregister = true;
	bdev_readcache_delete("wb", deleted, &rc);
	assert(rc == -999 && channel_holds == 1 && opens && claims);
	poll_channel(0); /* background can submit after foreground request is gone */
	finish(0, true);
	assert(disk[0] == 0xa1 && !channel_holds && !dma_live);
	for (i = 0; i < SPDK_COUNTOF(channels); i++) {
		framework_destroy(dev, spdk_io_channel_get_ctx(channels[i]));
		free(channels[i]);
	}
	defer_unregister = false;
	io_devices--;
	framework_device = NULL;
	deferred_unregister(deferred_device);
	deferred_unregister = NULL;
	framework_tick_cleanup();
	assert(!rc && !opens && !claims && !io_devices);
	puts("PASS W2: delete after ACK retains background channel, base claim and descriptor "
	     "until independent DMA and channel drain");
}

static struct writeback *stress_wb;
static atomic_uint stress_readers[4], stress_writers[4];

static void *
stress_worker(void *arg)
{
	uintptr_t id = (uintptr_t)arg;
	unsigned int i;

	for (i = 0; i < 1000; i++) {
		struct writeback_node n;
		unsigned int range = (id + i) % 4;
		bool write = (id + i) % 3 == 0;

		assert(writeback_enter(stress_wb, &n, range * BLOCK, BLOCK, write, false, 0, 1));
		while (!writeback_ready(stress_wb, &n, 1)) {
			sched_yield();
		}
		if (write) {
			assert(atomic_fetch_add(&stress_writers[range], 1) == 0);
			assert(!atomic_load(&stress_readers[range]));
			sched_yield();
			atomic_fetch_sub(&stress_writers[range], 1);
		} else {
			atomic_fetch_add(&stress_readers[range], 1);
			assert(!atomic_load(&stress_writers[range]));
			sched_yield();
			atomic_fetch_sub(&stress_readers[range], 1);
		}
		writeback_leave(stress_wb, &n, true, 1);
	}
	return NULL;
}

static void
threaded_ordering(void)
{
	struct writeback_options o = {128 * 1024, 32, 1000};
	struct writeback_stats s;
	pthread_t threads[10];
	uintptr_t i;

	stress_wb = writeback_create(&o);
	assert(stress_wb);
	for (i = 0; i < SPDK_COUNTOF(threads); i++) {
		assert(!pthread_create(&threads[i], NULL, stress_worker, (void *)i));
	}
	for (i = 0; i < SPDK_COUNTOF(threads); i++) {
		assert(!pthread_join(threads[i], NULL));
	}
	writeback_get_stats(stress_wb, &s, 1);
	assert(s.last_sequence == 10000 && !s.poisoned);
	writeback_destroy(stress_wb);
	puts("PASS W2: ten OS threads, 10000 interleaved range reservations, concurrent readers "
	     "and exclusive overlapping writers");
}

int
main(void)
{
	fanout_and_ordering();
	multi_range_and_scatter();
	barriers_and_bypasses();
	control_and_removal();
	pressure_and_errors();
	background_lifetime();
	threaded_ordering();
	return 0;
}
