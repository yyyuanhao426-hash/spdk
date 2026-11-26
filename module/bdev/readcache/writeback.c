/* SPDX-License-Identifier: BSD-3-Clause */
#include "writeback.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct writeback {
	pthread_mutex_t lock;
	TAILQ_HEAD(, writeback_node) nodes;
	struct writeback_stats stats;
	uint64_t slots;
};

static uint64_t
oldest_age(struct writeback *wb, uint64_t now_us)
{
	struct writeback_node *n;
	uint64_t age = 0;

	TAILQ_FOREACH (n, &wb->nodes, link) {
		if (n->charge && now_us >= n->admitted_us && now_us - n->admitted_us > age) {
			age = now_us - n->admitted_us;
		}
	}
	return age;
}

struct writeback *
writeback_create(const struct writeback_options *o)
{
	struct writeback *wb;

	if (!o || o->capacity_bytes < 4096 || !o->max_pending_writes || !o->max_dirty_age_us) {
		return NULL;
	}
	wb = calloc(1, sizeof(*wb));
	if (!wb) {
		return NULL;
	}
	if (pthread_mutex_init(&wb->lock, NULL)) {
		free(wb);
		return NULL;
	}
	TAILQ_INIT(&wb->nodes);
	wb->stats.capacity_bytes = o->capacity_bytes;
	wb->stats.max_pending_writes = o->max_pending_writes;
	wb->stats.max_dirty_age_us = o->max_dirty_age_us;
	return wb;
}

void
writeback_destroy(struct writeback *wb)
{
	assert(TAILQ_EMPTY(&wb->nodes) && !wb->slots && !wb->stats.allocated_bytes);
	pthread_mutex_destroy(&wb->lock);
	free(wb);
}

bool
writeback_reserve(struct writeback *wb, uint64_t charge, uint64_t now_us)
{
	struct writeback_stats *s = &wb->stats;
	bool ok = false;
	uint64_t high = s->capacity_bytes - s->capacity_bytes / 10;
	uint64_t low = s->capacity_bytes / 10 * 7;

	pthread_mutex_lock(&wb->lock);
	if (s->throttled && s->allocated_bytes <= low) {
		s->throttled = false;
	}
	if (s->allocated_bytes >= high || oldest_age(wb, now_us) >= s->max_dirty_age_us) {
		s->throttled = true;
	}
	if (!s->poisoned && !s->throttled && charge && charge <= s->capacity_bytes -
	    s->allocated_bytes && wb->slots < s->max_pending_writes) {
		s->allocated_bytes += charge;
		if (s->allocated_bytes > s->peak_bytes) {
			s->peak_bytes = s->allocated_bytes;
		}
		wb->slots++;
		ok = true;
	} else {
		s->backpressure++;
	}
	pthread_mutex_unlock(&wb->lock);
	return ok;
}

void
writeback_unreserve(struct writeback *wb, uint64_t charge)
{
	pthread_mutex_lock(&wb->lock);
	assert(wb->slots && wb->stats.allocated_bytes >= charge);
	wb->slots--;
	wb->stats.allocated_bytes -= charge;
	pthread_mutex_unlock(&wb->lock);
}

bool
writeback_enter(struct writeback *wb, struct writeback_node *n, uint64_t offset,
		uint64_t length, bool write, bool global, uint64_t charge, uint64_t now_us)
{
	*n = (struct writeback_node) {
		.offset = offset, .length = length, .write = write, .global = global,
		.charge = charge, .admitted_us = now_us,
	};
	pthread_mutex_lock(&wb->lock);
	if (charge && wb->stats.poisoned) {
		pthread_mutex_unlock(&wb->lock);
		return false;
	}
	n->sequence = ++wb->stats.last_sequence;
	wb->stats.active_operations++;
	if (wb->stats.active_operations > wb->stats.peak_operations) {
		wb->stats.peak_operations = wb->stats.active_operations;
	}
	TAILQ_INSERT_TAIL(&wb->nodes, n, link);
	if (charge) {
		wb->stats.accepted++;
		wb->stats.accepted_bytes += length;
		wb->stats.pending++;
		wb->stats.pending_bytes += length;
		if (wb->stats.pending > wb->stats.peak_pending) {
			wb->stats.peak_pending = wb->stats.pending;
		}
		if (wb->stats.pending_bytes > wb->stats.peak_pending_bytes) {
			wb->stats.peak_pending_bytes = wb->stats.pending_bytes;
		}
	}
	pthread_mutex_unlock(&wb->lock);
	return true;
}

int
writeback_ready(struct writeback *wb, struct writeback_node *n, uint64_t now_us)
{
	struct writeback_node *p;
	int rc = 1;

	pthread_mutex_lock(&wb->lock);
	if (wb->stats.poisoned) {
		rc = -EIO;
		goto out;
	}
	TAILQ_FOREACH (p, &wb->nodes, link) {
		if (p == n) {
			break;
		}
		if (p->global || n->global || ((p->write || n->write) && p->length && n->length &&
		    (p->offset <= n->offset ? n->offset - p->offset < p->length :
		     p->offset - n->offset < n->length))) {
			rc = 0;
			if (!n->write && !n->waited) {
				n->waited = true;
				wb->stats.read_waits++;
			}
			break;
		}
	}
	if (rc == 1 && !n->started) {
		n->started = true;
		if (n->waited && now_us >= n->admitted_us) {
			wb->stats.read_wait_us += now_us - n->admitted_us;
		}
	}
out:
	pthread_mutex_unlock(&wb->lock);
	return rc;
}

void
writeback_leave(struct writeback *wb, struct writeback_node *n, bool success, uint64_t now_us)
{
	struct writeback_stats *s = &wb->stats;
	uint64_t latency = now_us >= n->admitted_us ? now_us - n->admitted_us : 0;

	pthread_mutex_lock(&wb->lock);
	TAILQ_REMOVE(&wb->nodes, n, link);
	wb->stats.active_operations--;
	if (!success && n->write) {
		s->poisoned = true;
	}
	if (n->charge) {
		s->pending--;
		s->pending_bytes -= n->length;
		if (success) {
			s->committed++;
			s->committed_bytes += n->length;
		} else {
			s->failed++;
			s->failed_bytes += n->length;
		}
		s->commit_latency_us += latency;
		if (latency > s->max_commit_latency_us) {
			s->max_commit_latency_us = latency;
		}
		/* Caller releases the reservation after freeing payload AND node. */
	}
	pthread_mutex_unlock(&wb->lock);
}

void
writeback_get_stats(struct writeback *wb, struct writeback_stats *s, uint64_t now_us)
{
	pthread_mutex_lock(&wb->lock);
	*s = wb->stats;
	s->oldest_age_us = oldest_age(wb, now_us);
	pthread_mutex_unlock(&wb->lock);
}

void
writeback_poison(struct writeback *wb)
{
	pthread_mutex_lock(&wb->lock);
	wb->stats.poisoned = true;
	pthread_mutex_unlock(&wb->lock);
}
