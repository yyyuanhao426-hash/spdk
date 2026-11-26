/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SPDK_READCACHE_WRITEBACK_H
#define SPDK_READCACHE_WRITEBACK_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>

struct writeback;

struct writeback_options {
	uint64_t capacity_bytes, max_pending_writes, max_dirty_age_us;
};

struct writeback_stats {
	uint64_t capacity_bytes, max_pending_writes, max_dirty_age_us;
	uint64_t allocated_bytes, peak_bytes, pending, pending_bytes;
	uint64_t peak_pending, peak_pending_bytes, max_commit_latency_us;
	uint64_t active_operations, peak_operations;
	uint64_t accepted, committed, failed, accepted_bytes, committed_bytes, failed_bytes;
	uint64_t backpressure, read_waits, read_wait_us, commit_latency_us, oldest_age_us;
	uint64_t last_sequence;
	bool poisoned, throttled;
};

/* All foreground reads, mutations and namespace barriers participate. Nodes
 * stay alive until leave; only predecessors may delay a node. Keeping a read
 * reservation through its completion prevents younger writes starving it or
 * changing the media underneath its backend read. */
struct writeback_node {
	uint64_t offset, length, charge, sequence, admitted_us;
	bool write, global, waited, started;
	TAILQ_ENTRY(writeback_node) link;
};

struct writeback *writeback_create(const struct writeback_options *options);
void writeback_destroy(struct writeback *wb);
/* Reserve before allocating DMA/metadata. Capacity includes both; reservations
 * count against the slot limit even before they are published/ACKed. */
bool writeback_reserve(struct writeback *wb, uint64_t charge, uint64_t now_us);
void writeback_unreserve(struct writeback *wb, uint64_t charge);
bool writeback_enter(struct writeback *wb, struct writeback_node *node, uint64_t offset,
		     uint64_t length, bool write, bool global, uint64_t charge, uint64_t now_us);
/* 1 = runnable, 0 = wait, -EIO = fail closed. No callbacks under the lock. */
int writeback_ready(struct writeback *wb, struct writeback_node *node, uint64_t now_us);
void writeback_leave(struct writeback *wb, struct writeback_node *node, bool success,
		     uint64_t now_us);
/* A charged node's reservation must be unreserved after leave and deallocation. */
void writeback_get_stats(struct writeback *wb, struct writeback_stats *stats, uint64_t now_us);
void writeback_poison(struct writeback *wb);

#endif
