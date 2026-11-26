/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SPDK_READCACHE_CORE_H
#define SPDK_READCACHE_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include "writeback.h"

struct readcache;
struct readcache_entry;

/* Tokens belong to the caller until release. A WAIT token may only be consumed
 * after ready is observed with acquire ordering. No callbacks run under a lock. */
struct readcache_token {
	struct readcache_entry *entry;
	struct readcache_token *next;
	uint64_t data_offset, length;
	atomic_bool ready;
	bool success;
};

struct readcache_mutation {
	uint64_t offset;
	uint64_t length;
	bool conflicted;
	uint64_t generation;
	TAILQ_ENTRY(readcache_mutation) link;
};

enum readcache_result {
	READCACHE_BYPASS,
	READCACHE_HIT,
	READCACHE_MISS,
	READCACHE_WAIT,
};

struct readcache_stats {
	struct writeback_stats writeback;
	uint64_t read_requests, hits, miss_leaders, coalesced, bypasses;
	uint64_t read_fills, write_fills, fill_errors, evictions, invalidations;
	uint64_t allocated_bytes, peak_bytes, entries, active_mutations;
	uint64_t capacity_bytes, max_io_bytes;
	uint64_t read_bytes, hit_bytes, coalesced_bytes, miss_bytes, bypass_bytes;
	uint64_t bypass_disabled, bypass_oversize, bypass_mutation, bypass_budget;
	uint64_t bypass_alloc, bypass_invalid, bypass_external;
	uint64_t pinned_bytes, pinned_entries, loading_entries, ready_entries;
	uint64_t generation, stats_epoch;
	uint64_t io_errors;
	bool enabled;
	bool populate_on_write;
};

struct readcache_options {
	bool has_enabled, enabled;
	bool has_capacity;
	uint64_t capacity_bytes;
	bool has_populate_on_write, populate_on_write;
	bool clear, reset_stats;
};

/* Capacity includes allocated entry metadata and retained data, including
 * invalidated entries still pinned by readers. The fixed index is also charged.
 * Allocation failure degrades to bypass; it never fails the storage request. */
struct readcache *readcache_create(uint64_t capacity_bytes, uint64_t max_io_bytes);
int readcache_destroy(struct readcache *cache);
enum readcache_result readcache_acquire(struct readcache *cache, uint64_t offset, uint64_t length,
					struct readcache_token *token);
bool readcache_token_ready(struct readcache_token *token);
bool readcache_copy(struct readcache_token *token, struct iovec *iov, int iovcnt);
void readcache_fill(struct readcache *cache, struct readcache_token *leader,
		    const struct iovec *iov, int iovcnt, bool success);
void readcache_release(struct readcache *cache, struct readcache_token *token);
void readcache_begin_mutation(struct readcache *cache, uint64_t offset, uint64_t length,
			      struct readcache_mutation *mutation);
/* All writes go to the backend. Optional warming is allowed only after backend
 * success and if no overlapping mutation existed throughout this operation. */
void readcache_end_mutation(struct readcache *cache, struct readcache_mutation *mutation,
			    const struct iovec *iov, int iovcnt, bool warm);
void readcache_get_stats(struct readcache *cache, struct readcache_stats *stats);
int readcache_configure(struct readcache *cache, const struct readcache_options *options);
void readcache_record_external_bypass(struct readcache *cache, uint64_t length);
void readcache_record_io_error(struct readcache *cache);

#endif
