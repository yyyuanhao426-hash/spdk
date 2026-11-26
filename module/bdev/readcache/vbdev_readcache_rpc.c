/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "vbdev_readcache.h"

struct rpc_readcache_create {
	char *base_bdev_name;
	char *name;
	uint64_t capacity_bytes;
	uint64_t max_io_bytes;
	bool populate_on_write;
	bool enabled;
	char *write_ack_policy;
	struct writeback_options writeback;
};

static const struct spdk_json_object_decoder create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_readcache_create, base_bdev_name),
	 spdk_json_decode_string},
	{"name", offsetof(struct rpc_readcache_create, name), spdk_json_decode_string},
	{"capacity_bytes", offsetof(struct rpc_readcache_create, capacity_bytes),
	 spdk_json_decode_uint64},
	{"max_io_bytes", offsetof(struct rpc_readcache_create, max_io_bytes),
	 spdk_json_decode_uint64, true},
	{"populate_on_write", offsetof(struct rpc_readcache_create, populate_on_write),
	 spdk_json_decode_bool, true},
	{"enabled", offsetof(struct rpc_readcache_create, enabled), spdk_json_decode_bool, true},
	{"write_ack_policy", offsetof(struct rpc_readcache_create, write_ack_policy),
	 spdk_json_decode_string, true},
	{"dirty_capacity_bytes", offsetof(struct rpc_readcache_create, writeback.capacity_bytes),
	 spdk_json_decode_uint64, true},
	{"max_pending_writes", offsetof(struct rpc_readcache_create, writeback.max_pending_writes),
	 spdk_json_decode_uint64, true},
	{"max_dirty_age_us", offsetof(struct rpc_readcache_create, writeback.max_dirty_age_us),
	 spdk_json_decode_uint64, true},
};

static void
rpc_readcache_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_readcache_create req = {
		.max_io_bytes = 256 * 1024, .enabled = true,
		.writeback = {.max_pending_writes = 1024, .max_dirty_age_us = 1000000},
	};
	struct spdk_json_write_ctx *w;
	bool early;
	int rc;

	if (spdk_json_decode_object(params, create_decoders, SPDK_COUNTOF(create_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}
	early = req.write_ack_policy &&
		strcmp(req.write_ack_policy, "volatile_early_ack_wait_read") == 0;
	if ((req.write_ack_policy && !early && strcmp(req.write_ack_policy, "backend_complete")) ||
	    (early && !req.writeback.capacity_bytes) || (!early && req.writeback.capacity_bytes)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid write policy or dirty budget");
		goto out;
	}
	rc = bdev_readcache_create_ext(req.base_bdev_name, req.name, req.capacity_bytes,
				       req.max_io_bytes, req.populate_on_write,
				       early ? &req.writeback : NULL);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}
	if (!req.enabled) {
		const struct readcache_options options = {.has_enabled = true, .enabled = false};

		bdev_readcache_set_options(req.name, &options);
	}
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);
out:
	free(req.base_bdev_name);
	free(req.name);
	free(req.write_ack_policy);
}
SPDK_RPC_REGISTER("bdev_readcache_create", rpc_readcache_create, SPDK_RPC_RUNTIME)

struct rpc_readcache_name {
	char *name;
};

static const struct spdk_json_object_decoder name_decoders[] = {
	{"name", offsetof(struct rpc_readcache_name, name), spdk_json_decode_string},
};

static void
rpc_readcache_delete_done(void *arg, int rc)
{
	struct spdk_jsonrpc_request *request = arg;

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

static void
rpc_readcache_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_readcache_name req = {};

	if (spdk_json_decode_object(params, name_decoders, SPDK_COUNTOF(name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
	} else {
		bdev_readcache_delete(req.name, rpc_readcache_delete_done, request);
	}
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_readcache_delete", rpc_readcache_delete, SPDK_RPC_RUNTIME)

static void
rpc_readcache_get_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_readcache_name req = {};
	struct readcache_stats s;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, name_decoders, SPDK_COUNTOF(name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}
	rc = bdev_readcache_get_stats(req.name, &s);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", req.name);
	spdk_json_write_named_uint64(w, "api_version", 2);
#define WRITE_STAT(field) spdk_json_write_named_uint64(w, #field, s.field)
	WRITE_STAT(read_requests);
	WRITE_STAT(hits);
	WRITE_STAT(miss_leaders);
	WRITE_STAT(coalesced);
	WRITE_STAT(bypasses);
	WRITE_STAT(read_fills);
	WRITE_STAT(write_fills);
	WRITE_STAT(fill_errors);
	WRITE_STAT(evictions);
	WRITE_STAT(invalidations);
	WRITE_STAT(allocated_bytes);
	WRITE_STAT(peak_bytes);
	WRITE_STAT(entries);
	WRITE_STAT(active_mutations);
	WRITE_STAT(capacity_bytes);
	WRITE_STAT(max_io_bytes);
	WRITE_STAT(read_bytes);
	WRITE_STAT(hit_bytes);
	WRITE_STAT(coalesced_bytes);
	WRITE_STAT(miss_bytes);
	WRITE_STAT(bypass_bytes);
	WRITE_STAT(bypass_disabled);
	WRITE_STAT(bypass_oversize);
	WRITE_STAT(bypass_mutation);
	WRITE_STAT(bypass_budget);
	WRITE_STAT(bypass_alloc);
	WRITE_STAT(bypass_invalid);
	WRITE_STAT(bypass_external);
	WRITE_STAT(pinned_bytes);
	WRITE_STAT(pinned_entries);
	WRITE_STAT(loading_entries);
	WRITE_STAT(ready_entries);
	WRITE_STAT(generation);
	WRITE_STAT(stats_epoch);
	WRITE_STAT(io_errors);
#undef WRITE_STAT
	spdk_json_write_named_bool(w, "enabled", s.enabled);
	spdk_json_write_named_bool(w, "populate_on_write", s.populate_on_write);
	spdk_json_write_named_string(w, "write_ack_policy", s.writeback.capacity_bytes ?
				    "volatile_early_ack_wait_read" : "backend_complete");
	spdk_json_write_named_string(w, "read_commit_level", "backend_complete");
	spdk_json_write_named_object_begin(w, "writeback");
#define WRITE_WB(field) spdk_json_write_named_uint64(w, #field, s.writeback.field)
	WRITE_WB(capacity_bytes);
	WRITE_WB(max_pending_writes);
	WRITE_WB(max_dirty_age_us);
	WRITE_WB(allocated_bytes);
	WRITE_WB(peak_bytes);
	WRITE_WB(pending);
	WRITE_WB(pending_bytes);
	WRITE_WB(peak_pending);
	WRITE_WB(peak_pending_bytes);
	WRITE_WB(max_commit_latency_us);
	WRITE_WB(active_operations);
	WRITE_WB(peak_operations);
	WRITE_WB(accepted);
	WRITE_WB(committed);
	WRITE_WB(failed);
	WRITE_WB(accepted_bytes);
	WRITE_WB(committed_bytes);
	WRITE_WB(failed_bytes);
	WRITE_WB(backpressure);
	WRITE_WB(read_waits);
	WRITE_WB(read_wait_us);
	WRITE_WB(commit_latency_us);
	WRITE_WB(oldest_age_us);
	WRITE_WB(last_sequence);
#undef WRITE_WB
	spdk_json_write_named_bool(w, "poisoned", s.writeback.poisoned);
	spdk_json_write_named_bool(w, "throttled", s.writeback.throttled);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
out:
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_readcache_get_stats", rpc_readcache_get_stats, SPDK_RPC_RUNTIME)

struct optional_bool {
	bool value, present;
};

struct optional_uint64 {
	uint64_t value;
	bool present;
};

static int
decode_optional_bool(const struct spdk_json_val *val, void *out)
{
	struct optional_bool *opt = out;

	opt->present = true;
	return spdk_json_decode_bool(val, &opt->value);
}

static int
decode_optional_uint64(const struct spdk_json_val *val, void *out)
{
	struct optional_uint64 *opt = out;

	opt->present = true;
	return spdk_json_decode_uint64(val, &opt->value);
}

struct rpc_readcache_options {
	char *name;
	struct optional_bool enabled, populate_on_write;
	struct optional_uint64 capacity;
	bool clear, reset_stats;
};

static const struct spdk_json_object_decoder option_decoders[] = {
	{"name", offsetof(struct rpc_readcache_options, name), spdk_json_decode_string},
	{"enabled", offsetof(struct rpc_readcache_options, enabled), decode_optional_bool, true},
	{"populate_on_write", offsetof(struct rpc_readcache_options, populate_on_write),
	 decode_optional_bool, true},
	{"capacity_bytes", offsetof(struct rpc_readcache_options, capacity), decode_optional_uint64,
	 true},
	{"clear", offsetof(struct rpc_readcache_options, clear), spdk_json_decode_bool, true},
	{"reset_stats", offsetof(struct rpc_readcache_options, reset_stats), spdk_json_decode_bool,
	 true},
};

static void
rpc_readcache_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_readcache_options req = {};
	int rc;

	if (spdk_json_decode_object(params, option_decoders, SPDK_COUNTOF(option_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
	} else {
		const struct readcache_options options = {
			.has_enabled = req.enabled.present,
			.enabled = req.enabled.value,
			.has_capacity = req.capacity.present,
			.capacity_bytes = req.capacity.value,
			.has_populate_on_write = req.populate_on_write.present,
			.populate_on_write = req.populate_on_write.value,
			.clear = req.clear,
			.reset_stats = req.reset_stats,
		};

		rc = bdev_readcache_set_options(req.name, &options);
		if (rc == 0) {
			spdk_jsonrpc_send_bool_response(request, true);
		} else {
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		}
	}
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_readcache_set_options", rpc_readcache_set_options, SPDK_RPC_RUNTIME)
