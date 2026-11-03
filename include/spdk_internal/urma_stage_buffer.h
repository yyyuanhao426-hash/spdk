/* SPDX-License-Identifier: BSD-3-Clause
 * Experiment-only deferred stage records. Included once per transport TU.
 * No allocation, formatting, locks, or file writes in append().
 */
#ifndef URMA_STAGE_SIDE
#error "Define URMA_STAGE_SIDE to 1 (reader) or 2 (target)"
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum urma_stage_sink { URMA_STAGE_LEGACY, URMA_STAGE_QUIET,
	URMA_STAGE_ECHO, URMA_STAGE_BUFFER };
struct urma_stage_record { uint64_t v[24]; };
struct urma_stage_file_header {
	char magic[8];
	uint64_t version, side, record_bytes, capacity, attempted, count, dropped, mode;
};
struct urma_stage_file_footer { char magic[8]; uint64_t count, dropped; };
_Static_assert(sizeof(struct urma_stage_record) == 192, "record ABI");
_Static_assert(sizeof(struct urma_stage_file_header) == 72, "header ABI");
_Static_assert(sizeof(struct urma_stage_file_footer) == 24, "footer ABI");
static struct {
	struct urma_stage_record *rows;
	uint64_t capacity, next, gate;
	int fd;
	enum urma_stage_sink mode;
} g_urma_stage_buffer = { .fd = -1 };
#define URMA_STAGE_CLOSED (UINT64_C(1) << 63)
#if URMA_STAGE_SIDE == 1
#define URMA_STAGE_ENV "SPDK_URMA_STAGE_READER_"
#elif URMA_STAGE_SIDE == 2
#define URMA_STAGE_ENV "SPDK_URMA_STAGE_TARGET_"
#else
#error "Invalid URMA_STAGE_SIDE"
#endif

static inline enum urma_stage_sink urma_stage_mode(void)
{
	return g_urma_stage_buffer.mode;
}

static inline void urma_stage_append(const struct urma_stage_record *record)
{
	uint64_t gate, index;
	if (g_urma_stage_buffer.mode != URMA_STAGE_BUFFER) { return; }
	gate = __atomic_load_n(&g_urma_stage_buffer.gate, __ATOMIC_ACQUIRE);
	for (;;) {
		if (gate & URMA_STAGE_CLOSED) { return; }
		if (__atomic_compare_exchange_n(&g_urma_stage_buffer.gate, &gate, gate + 1,
			false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { break; }
	}
	index = __atomic_fetch_add(&g_urma_stage_buffer.next, 1, __ATOMIC_RELAXED);
	if (index < g_urma_stage_buffer.capacity) { g_urma_stage_buffer.rows[index] = *record; }
	__atomic_fetch_sub(&g_urma_stage_buffer.gate, 1, __ATOMIC_RELEASE);
}

static int urma_stage_write_all(int fd, const void *data, size_t bytes)
{
	const unsigned char *p = data;
	while (bytes) {
		ssize_t n = write(fd, p, bytes);
		if (n < 0 && errno == EINTR) { continue; }
		if (n <= 0) { return -1; }
		p += n; bytes -= (size_t)n;
	}
	return 0;
}

static void urma_stage_dump(void)
{
	uint64_t attempted, count, dropped;
	int ok;
	__atomic_fetch_or(&g_urma_stage_buffer.gate, URMA_STAGE_CLOSED, __ATOMIC_ACQ_REL);
	while (__atomic_load_n(&g_urma_stage_buffer.gate, __ATOMIC_ACQUIRE) != URMA_STAGE_CLOSED) {
		sched_yield();
	}
	if (g_urma_stage_buffer.fd < 0) { return; }
	attempted = __atomic_load_n(&g_urma_stage_buffer.next, __ATOMIC_RELAXED);
	count = attempted < g_urma_stage_buffer.capacity ? attempted : g_urma_stage_buffer.capacity;
	dropped = attempted - count;
	struct urma_stage_file_header h = { .magic = {'U','R','M','A','3','B','0','1'}, .version = 1,
		.side = URMA_STAGE_SIDE, .record_bytes = sizeof(struct urma_stage_record),
		.capacity = g_urma_stage_buffer.capacity, .attempted = attempted,
		.count = count, .dropped = dropped, .mode = g_urma_stage_buffer.mode };
	struct urma_stage_file_footer f = { .magic = {'U','R','M','A','3','E','N','D'}, .count = count, .dropped = dropped };
	ok = urma_stage_write_all(g_urma_stage_buffer.fd, &h, sizeof(h));
	if (!ok) { ok = urma_stage_write_all(g_urma_stage_buffer.fd, g_urma_stage_buffer.rows,
			(size_t)count * sizeof(struct urma_stage_record)); }
	if (!ok) { ok = urma_stage_write_all(g_urma_stage_buffer.fd, &f, sizeof(f)); }
	if (close(g_urma_stage_buffer.fd)) { ok = -1; }
	g_urma_stage_buffer.fd = -1;
	fprintf(stderr, "URMA_STAGE_BUFFER_V3 side=%u count=%" PRIu64
		" dropped=%" PRIu64 " complete=%u\n", URMA_STAGE_SIDE, count, dropped, ok == 0);
	free(g_urma_stage_buffer.rows);
	g_urma_stage_buffer.rows = NULL;
}

__attribute__((constructor)) static void urma_stage_buffer_init(void)
{
	const char *sink = getenv(URMA_STAGE_ENV "SINK");
	const char *path = getenv(URMA_STAGE_ENV "PATH");
	const char *raw = getenv(URMA_STAGE_ENV "CAPACITY");
	char *end;
	uint64_t capacity = UINT64_C(16777216);
	uint16_t endian = 1;
	if (sink) {
		if (!strcmp(sink, "legacy")) { g_urma_stage_buffer.mode = URMA_STAGE_LEGACY; }
		else if (!strcmp(sink, "quiet")) { g_urma_stage_buffer.mode = URMA_STAGE_QUIET; }
		else if (!strcmp(sink, "echo")) { g_urma_stage_buffer.mode = URMA_STAGE_ECHO; }
		else if (!strcmp(sink, "buffer")) { g_urma_stage_buffer.mode = URMA_STAGE_BUFFER; }
		else { fprintf(stderr, "invalid " URMA_STAGE_ENV "SINK\n"); abort(); }
	}
	if (!path || !*path) {
		if (g_urma_stage_buffer.mode == URMA_STAGE_BUFFER) {
			fprintf(stderr, "buffer sink requires " URMA_STAGE_ENV "PATH\n"); abort();
		}
		return;
	}
	if (*(unsigned char *)&endian != 1) { abort(); }
	if (raw) {
		errno = 0; capacity = strtoull(raw, &end, 10);
		if (errno || !*raw || *raw == '-' || *end || capacity < 1 || capacity > UINT64_C(33554432)) {
			fprintf(stderr, "invalid " URMA_STAGE_ENV "CAPACITY\n"); abort();
		}
	}
	g_urma_stage_buffer.capacity = capacity;
	g_urma_stage_buffer.rows = calloc((size_t)capacity, sizeof(struct urma_stage_record));
	if (!g_urma_stage_buffer.rows) { perror("stage buffer allocation"); abort(); }
	/* Touch every page before main, including quiet/echo control groups. */
	volatile unsigned char *p = (volatile unsigned char *)g_urma_stage_buffer.rows;
	for (size_t i = 0; i < capacity * sizeof(struct urma_stage_record); i += 4096) { p[i] = 0; }
	g_urma_stage_buffer.fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (g_urma_stage_buffer.fd < 0 || atexit(urma_stage_dump)) {
		perror("stage buffer setup"); abort();
	}
}
