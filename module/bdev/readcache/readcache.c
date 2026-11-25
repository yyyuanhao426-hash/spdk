/* SPDX-License-Identifier: BSD-3-Clause */
#include "readcache.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define READCACHE_BUCKETS 1024

struct readcache_entry {
	uint64_t offset, length;
	unsigned int refs;
	bool indexed, loading;
	void *data;
	struct readcache_entry *hash_next;
	struct readcache_token *waiters;
	TAILQ_ENTRY(readcache_entry) lru;
	TAILQ_ENTRY(readcache_entry) all;
};

struct readcache {
	pthread_mutex_t lock;
	struct readcache_entry *buckets[READCACHE_BUCKETS];
	TAILQ_HEAD(, readcache_entry) lru;
	TAILQ_HEAD(, readcache_entry) all;
	TAILQ_HEAD(, readcache_mutation) mutations;
	struct readcache_stats stats;
};

static bool
overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	/* Subtraction avoids overflowing end addresses, including full-device reset. */
	return alen && blen && (a <= b ? b - a < alen : a - b < blen);
}

static unsigned int
bucket(struct readcache *c, uint64_t offset)
{
	return (offset / c->stats.max_io_bytes) % READCACHE_BUCKETS;
}

static bool
mutating(struct readcache *c, uint64_t offset, uint64_t length)
{
	struct readcache_mutation *m;

	TAILQ_FOREACH (m, &c->mutations, link) {
		if (overlap(offset, length, m->offset, m->length)) {
			return true;
		}
	}
	return false;
}

static void
free_entry(struct readcache *c, struct readcache_entry *e)
{
	assert(!e->indexed && !e->refs && !e->waiters);
	c->stats.allocated_bytes -= sizeof(*e) + e->length;
	c->stats.entries--;
	TAILQ_REMOVE(&c->all, e, all);
	free(e->data);
	free(e);
}

static void
unindex(struct readcache *c, struct readcache_entry *e)
{
	struct readcache_entry **p;

	if (!e->indexed) {
		return;
	}
	p = &c->buckets[bucket(c, e->offset)];
	while (*p != e) {
		assert(*p != NULL);
		p = &(*p)->hash_next;
	}
	*p = e->hash_next;
	TAILQ_REMOVE(&c->lru, e, lru);
	e->indexed = false;
	if (e->refs == 0) {
		free_entry(c, e);
	}
}

static struct readcache_entry *
allocate_entry(struct readcache *c, uint64_t offset, uint64_t length, bool *allocation_failed)
{
	struct readcache_entry *e, *victim;
	uint64_t charge = sizeof(*e) + length;
	unsigned int b;

	*allocation_failed = false;

	if (charge > c->stats.capacity_bytes - sizeof(*c)) {
		return NULL;
	}
	while (charge > c->stats.capacity_bytes - c->stats.allocated_bytes) {
		TAILQ_FOREACH (victim, &c->lru, lru) {
			if (victim->refs == 0) {
				break;
			}
		}
		if (victim == NULL) {
			return NULL;
		}
		c->stats.evictions++;
		unindex(c, victim);
	}
	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		*allocation_failed = true;
		return NULL;
	}
	e->data = malloc(length);
	if (e->data == NULL) {
		*allocation_failed = true;
		free(e);
		return NULL;
	}
	e->offset = offset;
	e->length = length;
	e->loading = true;
	e->indexed = true;
	b = bucket(c, offset);
	e->hash_next = c->buckets[b];
	c->buckets[b] = e;
	TAILQ_INSERT_TAIL(&c->lru, e, lru);
	TAILQ_INSERT_TAIL(&c->all, e, all);
	c->stats.allocated_bytes += charge;
	if (c->stats.allocated_bytes > c->stats.peak_bytes) {
		c->stats.peak_bytes = c->stats.allocated_bytes;
	}
	c->stats.entries++;
	return e;
}

struct readcache *
readcache_create(uint64_t capacity_bytes, uint64_t max_io_bytes)
{
	struct readcache *c;

	if (max_io_bytes == 0 || max_io_bytes > SIZE_MAX - sizeof(struct readcache_entry) ||
	    capacity_bytes < sizeof(*c) + sizeof(struct readcache_entry) + 1) {
		return NULL;
	}
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&c->lock, NULL) != 0) {
		free(c);
		return NULL;
	}
	TAILQ_INIT(&c->lru);
	TAILQ_INIT(&c->all);
	TAILQ_INIT(&c->mutations);
	c->stats.capacity_bytes = capacity_bytes;
	c->stats.enabled = true;
	c->stats.generation = 1;
	c->stats.stats_epoch = 1;
	c->stats.max_io_bytes = max_io_bytes;
	c->stats.allocated_bytes = c->stats.peak_bytes = sizeof(*c);
	return c;
}

int
readcache_destroy(struct readcache *c)
{
	struct readcache_entry *e;

	pthread_mutex_lock(&c->lock);
	if (!TAILQ_EMPTY(&c->mutations)) {
		pthread_mutex_unlock(&c->lock);
		return -EBUSY;
	}
	TAILQ_FOREACH (e, &c->lru, lru) {
		if (e->refs != 0) {
			pthread_mutex_unlock(&c->lock);
			return -EBUSY;
		}
	}
	/* Unindexed but pinned entries are not on the LRU. */
	if (c->stats.entries != 0) {
		uint64_t indexed = 0;

		TAILQ_FOREACH (e, &c->lru, lru) {
			indexed++;
		}
		if (indexed != c->stats.entries) {
			pthread_mutex_unlock(&c->lock);
			return -EBUSY;
		}
	}
	while ((e = TAILQ_FIRST(&c->lru)) != NULL) {
		unindex(c, e);
	}
	pthread_mutex_unlock(&c->lock);
	pthread_mutex_destroy(&c->lock);
	free(c);
	return 0;
}

enum readcache_result
readcache_acquire(struct readcache *c, uint64_t offset, uint64_t length, struct readcache_token *t)
{
	struct readcache_entry *e;
	enum readcache_result result;
	unsigned int i, b;
	bool allocation_failed;

	t->entry = NULL;
	t->next = NULL;
	t->success = false;
	t->data_offset = 0;
	t->length = length;
	atomic_init(&t->ready, false);
	pthread_mutex_lock(&c->lock);
	c->stats.read_requests++;
	c->stats.read_bytes += length;
	if (length == 0 || length - 1 > UINT64_MAX - offset) {
		c->stats.bypass_invalid++;
		goto bypass;
	}
	if (!c->stats.enabled) {
		c->stats.bypass_disabled++;
		goto bypass;
	}
	if (length > c->stats.max_io_bytes) {
		c->stats.bypass_oversize++;
		goto bypass;
	}
	if (mutating(c, offset, length)) {
		c->stats.bypass_mutation++;
		goto bypass;
	}
	/* Any containing entry has at most max_io_bytes and starts in this address
	 * bucket or the preceding one. This also merges subreads of a loading read. */
	for (i = 0; i < 2; i++) {
		b = (bucket(c, offset) + READCACHE_BUCKETS - i) % READCACHE_BUCKETS;
		for (e = c->buckets[b]; e != NULL; e = e->hash_next) {
			if (e->offset <= offset && offset - e->offset <= e->length &&
			    length <= e->length - (offset - e->offset)) {
				e->refs++;
				t->entry = e;
				t->data_offset = offset - e->offset;
				TAILQ_REMOVE(&c->lru, e, lru);
				TAILQ_INSERT_TAIL(&c->lru, e, lru);
				if (e->loading) {
					t->next = e->waiters;
					e->waiters = t;
					c->stats.coalesced++;
					c->stats.coalesced_bytes += length;
					result = READCACHE_WAIT;
				} else {
					t->success = true;
					atomic_store_explicit(&t->ready, true,
							      memory_order_release);
					c->stats.hits++;
					c->stats.hit_bytes += length;
					result = READCACHE_HIT;
				}
				pthread_mutex_unlock(&c->lock);
				return result;
			}
		}
	}
	e = allocate_entry(c, offset, length, &allocation_failed);
	if (e == NULL) {
		if (allocation_failed) {
			c->stats.bypass_alloc++;
		} else {
			c->stats.bypass_budget++;
		}
		goto bypass;
	}
	e->refs = 1;
	t->entry = e;
	c->stats.miss_leaders++;
	c->stats.miss_bytes += length;
	pthread_mutex_unlock(&c->lock);
	return READCACHE_MISS;
bypass:
	c->stats.bypasses++;
	c->stats.bypass_bytes += length;
	pthread_mutex_unlock(&c->lock);
	return READCACHE_BYPASS;
}

bool
readcache_token_ready(struct readcache_token *t)
{
	return atomic_load_explicit(&t->ready, memory_order_acquire);
}

static bool
copy_iov(void *data, uint64_t length, const struct iovec *iov, int iovcnt, bool to_iov)
{
	uint64_t copied = 0;
	int i;

	for (i = 0; i < iovcnt && copied < length; i++) {
		size_t n = iov[i].iov_len < length - copied ? iov[i].iov_len : length - copied;

		if (n != 0 && iov[i].iov_base == NULL) {
			return false;
		}
		if (n != 0) {
			if (to_iov) {
				memcpy(iov[i].iov_base, (char *)data + copied, n);
			} else {
				memcpy((char *)data + copied, iov[i].iov_base, n);
			}
		}
		copied += n;
	}
	return copied == length;
}

bool
readcache_copy(struct readcache_token *t, struct iovec *iov, int iovcnt)
{
	return readcache_token_ready(t) && t->success &&
	       copy_iov((char *)t->entry->data + t->data_offset, t->length, iov, iovcnt, true);
}

void
readcache_fill(struct readcache *c, struct readcache_token *leader, const struct iovec *iov,
	       int iovcnt, bool success)
{
	struct readcache_entry *e = leader->entry;
	struct readcache_token *t, *next;

	/* Only the miss leader writes this storage. It is pinned and not visible to
	 * cache hits until publication; invalidation cannot free a pinned entry. */
	if (success) {
		success = copy_iov(e->data, e->length, iov, iovcnt, false);
	}
	pthread_mutex_lock(&c->lock);
	assert(e->loading);
	e->loading = false;
	if (success) {
		c->stats.read_fills++;
	} else {
		c->stats.fill_errors++;
		unindex(c, e);
	}
	t = e->waiters;
	e->waiters = NULL;
	for (; t != NULL; t = next) {
		next = t->next;
		t->next = NULL;
		t->success = success;
		atomic_store_explicit(&t->ready, true, memory_order_release);
	}
	leader->success = success;
	atomic_store_explicit(&leader->ready, true, memory_order_release);
	pthread_mutex_unlock(&c->lock);
}

void
readcache_release(struct readcache *c, struct readcache_token *t)
{
	struct readcache_entry *e = t->entry;

	if (e == NULL) {
		return;
	}
	pthread_mutex_lock(&c->lock);
	assert(e->refs != 0 && readcache_token_ready(t));
	e->refs--;
	t->entry = NULL;
	if (e->refs == 0 && !e->indexed) {
		free_entry(c, e);
	}
	pthread_mutex_unlock(&c->lock);
}

static void
invalidate(struct readcache *c, uint64_t offset, uint64_t length)
{
	struct readcache_entry *e, *next;
	uint64_t first, last, span;
	unsigned int b, i;

	if (length == 0) {
		return;
	}
	first = offset / c->stats.max_io_bytes;
	if (first != 0) {
		first--; /* A cached request may start in the preceding bucket. */
	}
	last = (length - 1 > UINT64_MAX - offset ? UINT64_MAX : offset + length - 1) /
	       c->stats.max_io_bytes;
	span = last - first + 1;
	if (span > READCACHE_BUCKETS) {
		span = READCACHE_BUCKETS;
	}
	for (i = 0; i < span; i++) {
		b = (first + i) % READCACHE_BUCKETS;
		for (e = c->buckets[b]; e != NULL; e = next) {
			next = e->hash_next;
			if (overlap(offset, length, e->offset, e->length)) {
				c->stats.invalidations++;
				unindex(c, e);
			}
		}
	}
}

void
readcache_begin_mutation(struct readcache *c, uint64_t offset, uint64_t length,
			 struct readcache_mutation *m)
{
	struct readcache_mutation *other;

	m->offset = offset;
	m->length = length;
	m->conflicted = false;
	pthread_mutex_lock(&c->lock);
	m->generation = c->stats.generation;
	invalidate(c, offset, length);
	TAILQ_FOREACH (other, &c->mutations, link) {
		if (overlap(offset, length, other->offset, other->length)) {
			other->conflicted = m->conflicted = true;
		}
	}
	TAILQ_INSERT_TAIL(&c->mutations, m, link);
	c->stats.active_mutations++;
	pthread_mutex_unlock(&c->lock);
}

void
readcache_end_mutation(struct readcache *c, struct readcache_mutation *m, const struct iovec *iov,
		       int iovcnt, bool warm)
{
	struct readcache_entry *e;
	bool allocation_failed;

	pthread_mutex_lock(&c->lock);
	TAILQ_REMOVE(&c->mutations, m, link);
	c->stats.active_mutations--;
	/* Overlapping writes may complete out of media order. Never infer the
	 * winning value from completion order, even after the other write finished. */
	if (warm && c->stats.enabled && m->generation == c->stats.generation && !m->conflicted &&
	    m->length != 0 && m->length <= c->stats.max_io_bytes) {
		e = allocate_entry(c, m->offset, m->length, &allocation_failed);
		if (e != NULL) {
			if (copy_iov(e->data, e->length, iov, iovcnt, false)) {
				e->loading = false;
				c->stats.write_fills++;
			} else {
				unindex(c, e);
			}
		}
	}
	pthread_mutex_unlock(&c->lock);
}

void
readcache_get_stats(struct readcache *c, struct readcache_stats *stats)
{
	struct readcache_entry *e;

	pthread_mutex_lock(&c->lock);
	*stats = c->stats;
	TAILQ_FOREACH (e, &c->all, all) {
		if (e->refs != 0) {
			stats->pinned_bytes += sizeof(*e) + e->length;
			stats->pinned_entries++;
		}
		stats->loading_entries += e->loading;
		stats->ready_entries += e->indexed && !e->loading;
	}
	pthread_mutex_unlock(&c->lock);
}

int
readcache_configure(struct readcache *c, const struct readcache_options *options)
{
	struct readcache_entry *e;
	uint64_t evictable = 0;

	pthread_mutex_lock(&c->lock);
	if (options->has_capacity) {
		if (options->capacity_bytes < sizeof(*c) + sizeof(*e) + 1) {
			pthread_mutex_unlock(&c->lock);
			return -EINVAL;
		}
		TAILQ_FOREACH (e, &c->all, all) {
			if (e->refs == 0) {
				evictable += sizeof(*e) + e->length;
			}
		}
		if (c->stats.allocated_bytes - evictable > options->capacity_bytes) {
			pthread_mutex_unlock(&c->lock);
			return -EBUSY;
		}
	}
	if (options->clear || (options->has_enabled && !options->enabled)) {
		invalidate(c, 0, UINT64_MAX);
	}
	if (options->clear || (options->has_enabled && options->enabled != c->stats.enabled)) {
		c->stats.generation++;
	}
	if (options->has_enabled) {
		c->stats.enabled = options->enabled;
	}
	if (options->has_capacity) {
		while (c->stats.allocated_bytes > options->capacity_bytes) {
			TAILQ_FOREACH (e, &c->lru, lru) {
				if (e->refs == 0) {
					break;
				}
			}
			assert(e != NULL);
			c->stats.evictions++;
			unindex(c, e);
		}
		c->stats.capacity_bytes = options->capacity_bytes;
	}
	if (options->reset_stats) {
		struct readcache_stats old = c->stats;

		memset(&c->stats, 0, sizeof(c->stats));
		c->stats.allocated_bytes = c->stats.peak_bytes = old.allocated_bytes;
		c->stats.entries = old.entries;
		c->stats.active_mutations = old.active_mutations;
		c->stats.capacity_bytes = old.capacity_bytes;
		c->stats.max_io_bytes = old.max_io_bytes;
		c->stats.enabled = old.enabled;
		c->stats.generation = old.generation;
		c->stats.stats_epoch = old.stats_epoch + 1;
	}
	pthread_mutex_unlock(&c->lock);
	return 0;
}

void
readcache_record_external_bypass(struct readcache *c, uint64_t length)
{
	pthread_mutex_lock(&c->lock);
	c->stats.read_requests++;
	c->stats.read_bytes += length;
	c->stats.bypasses++;
	c->stats.bypass_bytes += length;
	c->stats.bypass_external++;
	pthread_mutex_unlock(&c->lock);
}

void
readcache_record_io_error(struct readcache *c)
{
	pthread_mutex_lock(&c->lock);
	c->stats.io_errors++;
	pthread_mutex_unlock(&c->lock);
}
