/* SPDX-License-Identifier: BSD-3-Clause */
#include "readcache.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY (128 * 1024)
#define BLOCK 4096

void *__real_malloc(size_t size);
static bool fail_malloc;

void *
__wrap_malloc(size_t size)
{
	if (fail_malloc) {
		fail_malloc = false;
		return NULL;
	}
	return __real_malloc(size);
}

static void
fill(struct readcache *c, struct readcache_token *t, unsigned char value)
{
	unsigned char data[BLOCK];
	struct iovec iov[] = {{data, 1000}, {data + 1000, BLOCK - 1000}};

	memset(data, value, sizeof(data));
	readcache_fill(c, t, iov, 2, true);
}

static void
check(struct readcache_token *t, unsigned char value)
{
	unsigned char data[BLOCK];
	struct iovec iov[] = {{data, 997}, {data + 997, BLOCK - 997}};
	unsigned int i;

	memset(data, 0, sizeof(data));
	assert(readcache_copy(t, iov, 2));
	for (i = 0; i < sizeof(data); i++) {
		assert(data[i] == value);
	}
}

static void
fanout(void)
{
	struct readcache *c = readcache_create(CAPACITY, BLOCK);
	struct readcache_token t[10], hot;
	struct readcache_stats s;
	int i;

	assert(c);
	assert(readcache_acquire(c, 0, BLOCK, &t[0]) == READCACHE_MISS);
	for (i = 1; i < 10; i++) {
		assert(readcache_acquire(c, 0, BLOCK, &t[i]) == READCACHE_WAIT);
		assert(!readcache_token_ready(&t[i]));
	}
	fill(c, &t[0], 0x5a);
	for (i = 0; i < 10; i++) {
		check(&t[i], 0x5a);
		readcache_release(c, &t[i]);
	}
	assert(readcache_acquire(c, 0, BLOCK, &hot) == READCACHE_HIT);
	check(&hot, 0x5a);
	readcache_release(c, &hot);
	readcache_get_stats(c, &s);
	assert(s.miss_leaders == 1 && s.coalesced == 9 && s.hits == 1);
	assert(readcache_destroy(c) == 0);
	puts("PASS fanout: ten cold reads, one miss leader and nine merged readers");
}

static void
overwrite(void)
{
	struct readcache *c = readcache_create(CAPACITY, BLOCK);
	struct readcache_token old, waiter, during, fresh, hot;
	struct readcache_mutation w;

	assert(readcache_acquire(c, 0, BLOCK, &old) == READCACHE_MISS);
	assert(readcache_acquire(c, 0, BLOCK, &waiter) == READCACHE_WAIT);
	readcache_begin_mutation(c, BLOCK / 2, 1, &w);
	assert(readcache_acquire(c, 0, BLOCK, &during) == READCACHE_BYPASS);
	readcache_end_mutation(c, &w, NULL, 0, false);
	assert(readcache_acquire(c, 0, BLOCK, &fresh) == READCACHE_MISS);
	fill(c, &fresh, 0x22);
	readcache_release(c, &fresh);
	/* Old backend completion arrives after the newer read; it must not
	 * replace the current cache value. Already attached old readers may finish. */
	fill(c, &old, 0x11);
	check(&waiter, 0x11);
	readcache_release(c, &waiter);
	readcache_release(c, &old);
	assert(readcache_acquire(c, 0, BLOCK, &hot) == READCACHE_HIT);
	check(&hot, 0x22);
	readcache_release(c, &hot);
	assert(readcache_destroy(c) == 0);
	puts("PASS partial overwrite: old read completion cannot republish stale data");
}

static void
write_warm_and_conflict(void)
{
	struct readcache *c = readcache_create(CAPACITY, BLOCK);
	struct readcache_mutation a, b;
	struct readcache_token t;
	unsigned char data[BLOCK];
	struct iovec iov = {data, sizeof(data)};
	struct readcache_stats s;

	memset(data, 0x33, sizeof(data));
	readcache_begin_mutation(c, 0, BLOCK, &a);
	readcache_end_mutation(c, &a, &iov, 1, true);
	assert(readcache_acquire(c, 0, BLOCK, &t) == READCACHE_HIT);
	check(&t, 0x33);
	readcache_release(c, &t);
	readcache_begin_mutation(c, 0, BLOCK, &a);
	readcache_begin_mutation(c, BLOCK - 1, BLOCK, &b);
	readcache_end_mutation(c, &b, &iov, 1, true);
	readcache_end_mutation(c, &a, &iov, 1, true);
	assert(readcache_acquire(c, 0, BLOCK, &t) == READCACHE_MISS);
	readcache_fill(c, &t, NULL, 0, false);
	readcache_release(c, &t);
	readcache_get_stats(c, &s);
	assert(s.write_fills == 1);
	assert(readcache_destroy(c) == 0);
	puts("PASS write-through warming; conflicting writes never infer media value from "
	     "completion order");
}

static void
failure_and_budget(void)
{
	struct readcache *c = readcache_create(16384, BLOCK);
	struct readcache_token a, b, next, bypass;
	struct readcache_mutation w;
	struct readcache_stats s;

	assert(c);
	assert(readcache_acquire(c, 0, BLOCK, &a) == READCACHE_MISS);
	assert(readcache_acquire(c, 0, BLOCK, &b) == READCACHE_WAIT);
	assert(readcache_destroy(c) == -EBUSY);
	readcache_begin_mutation(c, 0, BLOCK, &w);
	readcache_end_mutation(c, &w, NULL, 0, false);
	/* Invalidated entries with outstanding references still consume budget. */
	assert(readcache_acquire(c, BLOCK, BLOCK, &bypass) == READCACHE_BYPASS);
	assert(readcache_destroy(c) == -EBUSY);
	readcache_fill(c, &a, NULL, 0, false);
	assert(readcache_token_ready(&b) && !b.success);
	readcache_release(c, &a);
	readcache_release(c, &b);
	assert(readcache_acquire(c, BLOCK, BLOCK, &next) == READCACHE_MISS);
	fill(c, &next, 0x44);
	readcache_release(c, &next);
	assert(readcache_acquire(c, BLOCK * 2, BLOCK, &next) == READCACHE_MISS);
	fill(c, &next, 0x55);
	readcache_release(c, &next);
	assert(readcache_acquire(c, 0, BLOCK + 1, &bypass) == READCACHE_BYPASS);
	readcache_get_stats(c, &s);
	assert(s.fill_errors == 1 && s.evictions == 1 && s.peak_bytes <= s.capacity_bytes);
	assert(readcache_destroy(c) == 0);
	puts("PASS failure fanout, pin-aware budget, eviction, oversize bypass and destroy guard");
}

struct concurrent_arg {
	struct readcache *cache;
	pthread_barrier_t *barrier;
	atomic_uint *leaders;
	int id;
};

static void *
concurrent_reader(void *arg)
{
	struct concurrent_arg *a = arg;
	struct readcache_token t;
	enum readcache_result result;

	result = readcache_acquire(a->cache, 0, BLOCK, &t);
	if (result == READCACHE_MISS) {
		atomic_fetch_add(a->leaders, 1);
	} else {
		assert(result == READCACHE_WAIT);
	}
	pthread_barrier_wait(a->barrier);
	if (result == READCACHE_MISS) {
		fill(a->cache, &t, 0x66);
	}
	while (!readcache_token_ready(&t)) {
		sched_yield();
	}
	check(&t, 0x66);
	readcache_release(a->cache, &t);
	return NULL;
}

static void
concurrency(void)
{
	pthread_t threads[10];
	struct concurrent_arg args[10];
	pthread_barrier_t barrier;
	atomic_uint leaders = 0;
	struct readcache *c = readcache_create(CAPACITY, BLOCK);
	unsigned int i;

	assert(pthread_barrier_init(&barrier, NULL, 10) == 0);
	for (i = 0; i < 10; i++) {
		args[i] = (struct concurrent_arg){c, &barrier, &leaders, i};
		assert(pthread_create(&threads[i], NULL, concurrent_reader, &args[i]) == 0);
	}
	for (i = 0; i < 10; i++) {
		assert(pthread_join(threads[i], NULL) == 0);
	}
	assert(atomic_load(&leaders) == 1);
	pthread_barrier_destroy(&barrier);
	assert(readcache_destroy(c) == 0);
	puts("PASS ten OS threads: one backend leader, atomic publication and independent reader "
	     "lifetimes");
}

static void
random_overwrites(void)
{
	struct readcache *c = readcache_create(CAPACITY, BLOCK);
	unsigned char disk[BLOCK * 8] = {0}, data[BLOCK];
	struct iovec iov = {data, sizeof(data)};
	struct readcache_mutation m;
	struct readcache_token t;
	uint32_t random = 930;
	unsigned int i, offset, length;
	enum readcache_result result;

	for (i = 0; i < 20000; i++) {
		random = random * 1664525u + 1013904223u;
		offset = (random >> 8) % (sizeof(disk) - BLOCK);
		if (i % 3 == 0) {
			length = 1 + (random % BLOCK);
			readcache_begin_mutation(c, offset, length, &m);
			memset(disk + offset, random & 255, length);
			readcache_end_mutation(c, &m, NULL, 0, false);
		} else {
			/* Revisit overlapping and different-length windows, including
			 * boundaries between address hash buckets. Compare every byte. */
			offset = (offset / 256) * 256;
			result = readcache_acquire(c, offset, BLOCK, &t);
			if (result == READCACHE_MISS) {
				memcpy(data, disk + offset, BLOCK);
				readcache_fill(c, &t, &iov, 1, true);
			}
			assert(result == READCACHE_MISS || result == READCACHE_HIT);
			assert(readcache_copy(&t, &iov, 1));
			assert(memcmp(data, disk + offset, BLOCK) == 0);
			readcache_release(c, &t);
		}
	}
	readcache_begin_mutation(c, 0, UINT64_MAX, &m);
	readcache_end_mutation(c, &m, NULL, 0, false);
	assert(readcache_destroy(c) == 0);
	puts("PASS 20000 seeded differential reads/partial overwrites and full-device "
	     "invalidation");
}

static void
allocation_failure_and_isolation(void)
{
	struct readcache *a = readcache_create(CAPACITY, BLOCK);
	struct readcache *b = readcache_create(CAPACITY, BLOCK);
	struct readcache_token t, other;
	struct readcache_stats s;

	fail_malloc = true;
	assert(readcache_acquire(a, 0, BLOCK, &t) == READCACHE_BYPASS);
	readcache_get_stats(a, &s);
	assert(s.entries == 0 && s.bypasses == 1);
	assert(readcache_acquire(a, 0, BLOCK, &t) == READCACHE_MISS);
	fill(a, &t, 0xaa);
	readcache_release(a, &t);
	assert(readcache_acquire(b, 0, BLOCK, &other) == READCACHE_MISS);
	fill(b, &other, 0xbb);
	readcache_release(b, &other);
	assert(readcache_acquire(a, 0, BLOCK, &t) == READCACHE_HIT);
	check(&t, 0xaa);
	readcache_release(a, &t);
	assert(readcache_acquire(b, 0, BLOCK, &other) == READCACHE_HIT);
	check(&other, 0xbb);
	readcache_release(b, &other);
	assert(readcache_acquire(a, UINT64_MAX - 1, BLOCK, &t) == READCACHE_BYPASS);
	assert(readcache_destroy(a) == 0 && readcache_destroy(b) == 0);
	puts("PASS allocation failure bypass, independent bdev data and overflow-range rejection");
}

static void
subrange_and_control(void)
{
	struct readcache *c = readcache_create(CAPACITY, BLOCK);
	struct readcache_token leader, sub, hot, during, second;
	struct readcache_options options = {};
	struct readcache_mutation write;
	struct readcache_stats before, stats;
	unsigned char data[BLOCK], small[512];
	struct iovec full = {data, BLOCK}, part = {small, sizeof(small)};
	unsigned int i;

	for (i = 0; i < BLOCK; i++) {
		data[i] = (i * 17 + i / 256) & 255;
	}
	assert(readcache_acquire(c, BLOCK - 1000, BLOCK, &leader) == READCACHE_MISS);
	assert(readcache_acquire(c, BLOCK + 24, 512, &sub) == READCACHE_WAIT);
	readcache_fill(c, &leader, &full, 1, true);
	assert(readcache_copy(&sub, &part, 1));
	assert(!memcmp(small, data + 1024, sizeof(small)));
	readcache_release(c, &sub);
	readcache_release(c, &leader);
	assert(readcache_acquire(c, BLOCK + 24, 512, &hot) == READCACHE_HIT);
	assert(readcache_copy(&hot, &part, 1));
	assert(!memcmp(small, data + 1024, sizeof(small)));
	readcache_get_stats(c, &before);
	assert(before.pinned_bytes > 0 && before.pinned_entries == 1);
	assert(before.coalesced_bytes == 512 && before.hit_bytes == 512);
	options = (struct readcache_options){.clear = true};
	assert(readcache_configure(c, &options) == 0);
	readcache_get_stats(c, &stats);
	assert(stats.pinned_bytes == before.pinned_bytes && stats.ready_entries == 0);
	assert(stats.generation == before.generation + 1);
	assert(readcache_copy(&hot, &part, 1));
	readcache_release(c, &hot);

	assert(readcache_acquire(c, 0, BLOCK, &leader) == READCACHE_MISS);
	assert(readcache_acquire(c, BLOCK, BLOCK, &second) == READCACHE_MISS);
	options = (struct readcache_options){.has_capacity = true,
					     .capacity_bytes = 16384,
					     .has_enabled = true,
					     .enabled = false,
					     .clear = true};
	assert(readcache_configure(c, &options) == -EBUSY);
	readcache_get_stats(c, &stats);
	assert(stats.enabled && stats.capacity_bytes == CAPACITY && stats.loading_entries == 2);
	readcache_begin_mutation(c, BLOCK * 3, BLOCK, &write);
	options = (struct readcache_options){.has_enabled = true, .enabled = false};
	assert(readcache_configure(c, &options) == 0);
	assert(readcache_acquire(c, 0, BLOCK, &during) == READCACHE_BYPASS);
	readcache_fill(c, &leader, &full, 1, true);
	readcache_fill(c, &second, &full, 1, true);
	readcache_release(c, &leader);
	readcache_release(c, &second);
	options = (struct readcache_options){.has_enabled = true,
					     .enabled = true,
					     .has_capacity = true,
					     .capacity_bytes = 16384};
	assert(readcache_configure(c, &options) == 0);
	readcache_end_mutation(c, &write, &full, 1, true);
	assert(readcache_acquire(c, BLOCK * 3, BLOCK, &hot) == READCACHE_MISS);
	readcache_fill(c, &hot, &full, 1, true);
	readcache_release(c, &hot);
	options = (struct readcache_options){.reset_stats = true};
	assert(readcache_configure(c, &options) == 0);
	readcache_get_stats(c, &stats);
	assert(stats.stats_epoch == 2 && stats.read_requests == 0 && stats.entries == 1);
	assert(readcache_acquire(c, BLOCK * 3, BLOCK, &hot) == READCACHE_HIT);
	readcache_release(c, &hot);
	readcache_record_external_bypass(c, BLOCK);
	readcache_get_stats(c, &stats);
	assert(stats.read_requests ==
	       stats.hits + stats.coalesced + stats.miss_leaders + stats.bypasses);
	assert(stats.read_bytes ==
	       stats.hit_bytes + stats.coalesced_bytes + stats.miss_bytes + stats.bypass_bytes);
	assert(stats.bypass_external == 1 && stats.allocated_bytes <= stats.capacity_bytes);
	assert(readcache_destroy(c) == 0);
	puts("PASS contained ranges across buckets, online control, pin-safe resize, generation "
	     "fencing and counter reset");
}

int
main(void)
{
	subrange_and_control();
	fanout();
	overwrite();
	write_warm_and_conflict();
	failure_and_budget();
	concurrency();
	random_overwrites();
	allocation_failure_and_isolation();
	return 0;
}
