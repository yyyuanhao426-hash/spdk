/* SPDX-License-Identifier: BSD-3-Clause */
/* Real production RPC handlers and SPDK JSON parser/decoder/writer; only the
 * local bdev framework and JSON-RPC response envelope are mocked. */
#define READCACHE_REAL_JSON
#define main bdev_harness_main
#include "bdev.c"
#undef main
#include "../../module/bdev/readcache/vbdev_readcache_rpc.c"

struct spdk_jsonrpc_request {
	char output[16384];
	size_t length;
	int error;
	bool responded;
};

static struct {
	const char *name;
	spdk_rpc_method_handler handler;
} methods[8];
static unsigned int method_count;

void
spdk_rpc_register_method(const char *name, spdk_rpc_method_handler handler, uint32_t states)
{
	assert(method_count < SPDK_COUNTOF(methods));
	methods[method_count].name = name;
	methods[method_count++].handler = handler;
}

void
spdk_log_register_flag(const char *name, struct spdk_log_flag *flag)
{
}

void
spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	 const char *format, ...)
{
}

const char *
spdk_strerror(int errnum)
{
	return strerror(errnum);
}

static int
output(void *arg, const void *data, size_t size)
{
	struct spdk_jsonrpc_request *request = arg;

	assert(request->length + size < sizeof(request->output));
	memcpy(request->output + request->length, data, size);
	request->length += size;
	request->output[request->length] = 0;
	return 0;
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	return spdk_json_write_begin(output, request, 0);
}

void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
	assert(spdk_json_write_end(w) == 0);
	request->responded = true;
}

void
spdk_jsonrpc_send_bool_response(struct spdk_jsonrpc_request *request, bool value)
{
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, value);
	spdk_jsonrpc_end_result(request, w);
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request, int code,
				 const char *message)
{
	request->error = code;
	request->responded = true;
}

static struct spdk_jsonrpc_request
invoke(const char *method, const char *params)
{
	struct spdk_jsonrpc_request request = {};
	struct spdk_json_val values[128];
	char *text = strdup(params);
	void *end;
	ssize_t n;
	unsigned int i;

	n = spdk_json_parse(text, strlen(text), values, SPDK_COUNTOF(values), &end,
			    SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	assert(n > 0);
	for (i = 0; i < method_count; i++) {
		if (!strcmp(method, methods[i].name)) {
			methods[i].handler(&request, values);
			framework_tick_cleanup();
			break;
		}
	}
	assert(i != method_count && request.responded);
	free(text);
	return request;
}

int
main(void)
{
	struct spdk_jsonrpc_request result;
	struct readcache_stats stats;

	framework_base = (struct spdk_bdev){.blocklen = BLOCK, .blockcnt = 64, .name = "base"};
	result = invoke("bdev_readcache_create", "{\"base_bdev_name\":\"base\",\"name\":\"cache\","
						 "\"capacity_bytes\":131072,\"enabled\":false}");
	assert(!result.error && !strcmp(result.output, "\"cache\""));
	assert(bdev_readcache_get_stats("cache", &stats) == 0 && !stats.enabled);
	result = invoke("bdev_readcache_set_options",
			"{\"name\":\"cache\",\"enabled\":true,\"populate_on_write\":true,"
			"\"capacity_bytes\":65536,\"clear\":true,\"reset_stats\":true}");
	assert(!result.error && !strcmp(result.output, "true"));
	assert(bdev_readcache_get_stats("cache", &stats) == 0 && stats.enabled &&
	       stats.populate_on_write);
	assert(stats.capacity_bytes == 65536 && stats.stats_epoch == 2);
	result = invoke("bdev_readcache_get_stats", "{\"name\":\"cache\"}");
	assert(!result.error && strstr(result.output, "\"enabled\":true"));
	assert(strstr(result.output, "\"capacity_bytes\":65536"));
	assert(strstr(result.output, "\"stats_epoch\":2"));
	assert(strstr(result.output, "\"bypass_external\":0"));
	result = invoke("bdev_readcache_set_options",
			"{\"name\":\"cache\",\"capacity_bytes\":1,\"enabled\":false}");
	assert(result.error == -EINVAL);
	assert(bdev_readcache_get_stats("cache", &stats) == 0 && stats.enabled);
	result = invoke("bdev_readcache_set_options", "{\"name\":\"cache\",\"enabled\":\"false\"}");
	assert(result.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
	result = invoke("bdev_readcache_set_options", "{\"name\":\"cache\",\"unknown\":true}");
	assert(result.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
	result = invoke("bdev_readcache_create",
			"{\"name\":\"missing_required\",\"capacity_bytes\":131072}");
	assert(result.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
	result = invoke("bdev_readcache_delete", "{\"name\":\"cache\"}");
	assert(!result.error && !strcmp(result.output, "true"));
	result = invoke("bdev_readcache_get_stats", "{\"name\":\"cache\"}");
	assert(result.error == -ENODEV);
	assert(!opens && !claims && !io_devices && !framework_registered);
	puts("PASS production RPC + real SPDK JSON: create/configure/stats/delete, invalid types, "
	     "unknown fields and atomic error handling");
	return 0;
}
