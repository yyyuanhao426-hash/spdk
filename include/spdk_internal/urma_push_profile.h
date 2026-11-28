/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SPDK_INTERNAL_URMA_PUSH_PROFILE_H
#define SPDK_INTERNAL_URMA_PUSH_PROFILE_H

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/util.h"

/* Process-cumulative diagnostics, independent of the legacy connection resets.
 * Only the last 64 slow records are retained. Never print in a completion. */
#define URMA_PUSH_PROFILE_RECORDS 64

struct urma_push_record {
	uint64_t seq, trace_seq, lba;
	uint64_t post_tick, poll_tick, owner_tick, done_tick;
	uint64_t poll_start_tick, previous_poll_end_tick;
	uint64_t poll_thread, owner_thread;
	uintptr_t qpair, device;
	uint32_t poll_core, owner_core, poll_pg, owner_pg, jfc_index, batch_count, length;
	uint16_t qid, cid;
	uint8_t opcode;
	bool forwarded, split_valid;
	char peer[64];
};

struct urma_push_poll_record {
	uint64_t start, end, previous, thread;
	uintptr_t device;
	uint32_t core, pg, jfc;
	int count;
};

/* Separate hot poll counters so independent JFC pollers do not continually
 * write the same cache line. Collisions affect cost, never counting accuracy. */
struct urma_push_poll_counters {
	uint64_t poll_attempts, poll_calls, poll_empty, poll_busy, poll_errors;
	uint64_t poll_call_ticks, poll_call_max, poll_gaps, poll_gap_ticks, poll_gap_max;
} __attribute__((aligned(128)));

struct urma_push_profile {
	pthread_mutex_t lock;
	uint64_t total, valid, untracked, early_poll, invalid;
	uint64_t post_to_poll, poll_to_owner, owner_to_done;
	uint64_t saved, printed;
	struct urma_push_record records[URMA_PUSH_PROFILE_RECORDS];
	uint64_t poll_saved, poll_printed;
	struct urma_push_poll_record polls[URMA_PUSH_PROFILE_RECORDS];
	/* Poll updates stay lock-free, including the overwhelmingly empty polls. */
	struct urma_push_poll_counters counters[64];
};

static struct urma_push_profile g_urma_push_profile = {.lock = PTHREAD_MUTEX_INITIALIZER};
static pthread_once_t g_urma_push_profile_once = PTHREAD_ONCE_INIT;
static bool g_urma_push_profile_enabled = true;
static uint64_t g_urma_push_threshold_ticks;
static uint64_t g_urma_push_poll_threshold_ticks;

static void
urma_push_profile_init(void)
{
	const char *enabled = getenv("SPDK_URMA_TARGET_PUSH_PROFILE");
	const char *threshold = getenv("SPDK_URMA_TARGET_PUSH_TRACE_US");
	uint64_t us = 1000;
	char *end;

	g_urma_push_profile_enabled = enabled == NULL || strcmp(enabled, "0") != 0;
	if (threshold != NULL && threshold[0] >= '0' && threshold[0] <= '9') {
		errno = 0;
		unsigned long long value = strtoull(threshold, &end, 10);

		if (errno == 0 && *end == '\0' && value <= 3600000000ULL) {
			us = value;
		}
	}
	g_urma_push_threshold_ticks = (uint64_t)((__uint128_t)us * spdk_get_ticks_hz() / 1000000);
	g_urma_push_poll_threshold_ticks = spdk_max(UINT64_C(1), spdk_get_ticks_hz() / 1000);
}

static bool
urma_push_profile_enabled(void)
{
	pthread_once(&g_urma_push_profile_once, urma_push_profile_init);
	return g_urma_push_profile_enabled;
}

static void
urma_push_profile_max(uint64_t *field, uint64_t value)
{
	uint64_t old = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (old < value && !__atomic_compare_exchange_n(field, &old, value, false,
			__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static bool
urma_push_profile_poll(uint64_t start, uint64_t end, uint64_t previous, bool busy, int count,
		uint32_t shard)
{
	struct urma_push_poll_counters *p = &g_urma_push_profile.counters[shard % 64];

#define PUSH_POLL_ADD(field, value) __atomic_fetch_add(&p->field, (value), __ATOMIC_RELAXED)
	PUSH_POLL_ADD(poll_attempts, 1);
	if (busy) {
		PUSH_POLL_ADD(poll_busy, 1);
		return false;
	}
	if (end == 0 || end < start) {
		return false;
	}
	PUSH_POLL_ADD(poll_calls, 1);
	PUSH_POLL_ADD(poll_empty, count == 0);
	PUSH_POLL_ADD(poll_errors, count < 0);
	PUSH_POLL_ADD(poll_call_ticks, end - start);
	urma_push_profile_max(&p->poll_call_max, end - start);
	if (previous != 0 && start >= previous) {
		PUSH_POLL_ADD(poll_gaps, 1);
		PUSH_POLL_ADD(poll_gap_ticks, start - previous);
		urma_push_profile_max(&p->poll_gap_max, start - previous);
	}
#undef PUSH_POLL_ADD
	return end - start >= g_urma_push_poll_threshold_ticks ||
	       (previous != 0 && start >= previous &&
		start - previous >= g_urma_push_poll_threshold_ticks);
}

static void
urma_push_profile_poll_record(const struct urma_push_poll_record *record)
{
	struct urma_push_profile *p = &g_urma_push_profile;

	pthread_mutex_lock(&p->lock);
	p->polls[p->poll_saved++ % URMA_PUSH_PROFILE_RECORDS] = *record;
	pthread_mutex_unlock(&p->lock);
}

static void
urma_push_profile_record(struct urma_push_record *record)
{
	struct urma_push_profile *p = &g_urma_push_profile;

	if (!urma_push_profile_enabled()) {
		return;
	}
	record->split_valid = record->poll_tick != 0 &&
			      record->poll_start_tick <= record->poll_tick &&
			      record->post_tick <= record->poll_tick &&
			      record->poll_tick <= record->owner_tick &&
			      record->owner_tick <= record->done_tick;
	pthread_mutex_lock(&p->lock);
	record->seq = ++p->total;
	if (record->split_valid) {
		p->valid++;
		p->post_to_poll += record->poll_tick - record->post_tick;
		p->poll_to_owner += record->owner_tick - record->poll_tick;
		p->owner_to_done += record->done_tick - record->owner_tick;
	} else if (record->poll_tick == 0) {
		p->untracked++;
	} else if (record->poll_tick < record->post_tick) {
		p->early_poll++;
	} else {
		p->invalid++;
	}
	if (!record->split_valid || record->done_tick - record->post_tick >=
	    g_urma_push_threshold_ticks) {
		p->records[p->saved++ % URMA_PUSH_PROFILE_RECORDS] = *record;
	}
	pthread_mutex_unlock(&p->lock);
}

static uint64_t
urma_push_ticks_to_ns(uint64_t ticks)
{
	uint64_t hz = spdk_get_ticks_hz();

	return hz == 0 ? 0 : (uint64_t)((__uint128_t)ticks * 1000000000 / hz);
}

static void
urma_push_profile_dump(FILE *out)
{
	struct urma_push_profile *p = &g_urma_push_profile;
	struct urma_push_record records[URMA_PUSH_PROFILE_RECORDS];
	struct urma_push_poll_record polls[URMA_PUSH_PROFILE_RECORDS];
	uint64_t count, total, valid, untracked, early, invalid, a, b, c, overwritten;
	uint64_t poll_count, poll_overwritten;
	struct urma_push_poll_counters sums = {};

	if (!urma_push_profile_enabled()) {
		return;
	}
	pthread_mutex_lock(&p->lock);
	total = p->total;
	valid = p->valid;
	untracked = p->untracked;
	early = p->early_poll;
	invalid = p->invalid;
	a = p->post_to_poll;
	b = p->poll_to_owner;
	c = p->owner_to_done;
	count = spdk_min(p->saved - p->printed, (uint64_t)URMA_PUSH_PROFILE_RECORDS);
	overwritten = p->saved - p->printed - count;
	for (uint64_t i = 0; i < count; i++) {
		records[i] = p->records[(p->saved - count + i) % URMA_PUSH_PROFILE_RECORDS];
	}
	p->printed = p->saved;
	poll_count = spdk_min(p->poll_saved - p->poll_printed, (uint64_t)URMA_PUSH_PROFILE_RECORDS);
	poll_overwritten = p->poll_saved - p->poll_printed - poll_count;
	for (uint64_t i = 0; i < poll_count; i++) {
		polls[i] = p->polls[(p->poll_saved - poll_count + i) % URMA_PUSH_PROFILE_RECORDS];
	}
	p->poll_printed = p->poll_saved;
	pthread_mutex_unlock(&p->lock);
	fprintf(out, "URMA_PUSH_PROFILE cumulative=1 hz=%" PRIu64 " threshold_us=%" PRIu64
		" total=%" PRIu64
		" valid=%" PRIu64 " untracked=%" PRIu64 " early_poll=%" PRIu64
		" invalid=%" PRIu64 " post_to_poll_avg_ns=%" PRIu64
		" poll_to_owner_avg_ns=%" PRIu64 " owner_to_done_avg_ns=%" PRIu64
		" details=%" PRIu64 " overwritten=%" PRIu64 "\n",
		spdk_get_ticks_hz(), urma_push_ticks_to_ns(g_urma_push_threshold_ticks) / 1000,
		total, valid, untracked, early, invalid,
		valid ? urma_push_ticks_to_ns(a) / valid : 0,
		valid ? urma_push_ticks_to_ns(b) / valid : 0,
		valid ? urma_push_ticks_to_ns(c) / valid : 0, count, overwritten);
	for (unsigned i = 0; i < SPDK_COUNTOF(p->counters); i++) {
		const struct urma_push_poll_counters *s = &p->counters[i];
#define SUM_POLL(field) sums.field += __atomic_load_n(&s->field, __ATOMIC_RELAXED)
		SUM_POLL(poll_attempts);
		SUM_POLL(poll_calls);
		SUM_POLL(poll_empty);
		SUM_POLL(poll_busy);
		SUM_POLL(poll_errors);
		SUM_POLL(poll_call_ticks);
		SUM_POLL(poll_gaps);
		SUM_POLL(poll_gap_ticks);
		sums.poll_call_max = spdk_max(sums.poll_call_max,
				__atomic_load_n(&s->poll_call_max, __ATOMIC_RELAXED));
		sums.poll_gap_max = spdk_max(sums.poll_gap_max,
				__atomic_load_n(&s->poll_gap_max, __ATOMIC_RELAXED));
#undef SUM_POLL
	}
#define PUSH_POLL_GET(field) (sums.field)
	uint64_t calls = PUSH_POLL_GET(poll_calls), gaps = PUSH_POLL_GET(poll_gaps);
	fprintf(out, "URMA_JFC_POLL cumulative=1 attempts=%" PRIu64 " calls=%" PRIu64
		" empty=%" PRIu64 " busy=%" PRIu64 " errors=%" PRIu64
		" call_avg_ns=%" PRIu64 " call_max_ns=%" PRIu64
		" gap_avg_ns=%" PRIu64 " gap_max_ns=%" PRIu64
		" details=%" PRIu64 " overwritten=%" PRIu64 "\n",
		PUSH_POLL_GET(poll_attempts), calls, PUSH_POLL_GET(poll_empty),
		PUSH_POLL_GET(poll_busy), PUSH_POLL_GET(poll_errors),
		calls ? urma_push_ticks_to_ns(PUSH_POLL_GET(poll_call_ticks)) / calls : 0,
		urma_push_ticks_to_ns(PUSH_POLL_GET(poll_call_max)),
		gaps ? urma_push_ticks_to_ns(PUSH_POLL_GET(poll_gap_ticks)) / gaps : 0,
		urma_push_ticks_to_ns(PUSH_POLL_GET(poll_gap_max)), poll_count, poll_overwritten);
#undef PUSH_POLL_GET
	for (uint64_t i = 0; i < poll_count; i++) {
		const struct urma_push_poll_record *r = &polls[i];
		bool known = r->previous != 0 && r->start >= r->previous;

		fprintf(out, "URMA_JFC_POLL_DETAIL device=0x%" PRIxPTR " jfc=%u pg=%u"
			" thread=%" PRIu64 " core=%u previous_end=%" PRIu64
			" poll_start=%" PRIu64 " poll_end=%" PRIu64 " completions=%d"
			" call_ns=%" PRIu64 " gap_known=%u gap_ns=%" PRIu64 "\n",
			r->device, r->jfc, r->pg, r->thread, r->core, r->previous, r->start,
			r->end, r->count, urma_push_ticks_to_ns(r->end - r->start), known,
			known ? urma_push_ticks_to_ns(r->start - r->previous) : 0);
	}
	for (uint64_t i = 0; i < count; i++) {
		const struct urma_push_record *r = &records[i];
		uint64_t x = 0, y = 0, z = 0;
		bool gap_known = r->previous_poll_end_tick != 0 &&
				 r->poll_start_tick >= r->previous_poll_end_tick;

		if (r->split_valid) {
			x = urma_push_ticks_to_ns(r->poll_tick - r->post_tick) / 1000;
			y = urma_push_ticks_to_ns(r->owner_tick - r->poll_tick) / 1000;
			z = urma_push_ticks_to_ns(r->done_tick - r->owner_tick) / 1000;
		}
		fprintf(out, "URMA_PUSH_DETAIL seq=%" PRIu64 " peer=%s qpair=0x%" PRIxPTR
			" device=0x%" PRIxPTR " qid=%u cid=%u opcode=%u lba=%" PRIu64
			" bytes=%u trace_seq=%" PRIu64 " jfc=%u batch=%u forwarded=%u"
			" poll_pg=%u owner_pg=%u poll_thread=%" PRIu64 " owner_thread=%" PRIu64
			" poll_core=%u owner_core=%u split_valid=%u"
			" t0=%" PRIu64 " tpoll=%" PRIu64 " towner=%" PRIu64 " t1=%" PRIu64
			" post_to_poll_us=%" PRIu64 " poll_to_owner_us=%" PRIu64
			" owner_to_done_us=%" PRIu64 " poll_call_ns=%" PRIu64
			" poll_gap_known=%u poll_gap_ns=%" PRIu64 "\n",
			r->seq, r->peer, r->qpair, r->device, r->qid, r->cid, r->opcode, r->lba,
			r->length, r->trace_seq, r->jfc_index, r->batch_count, r->forwarded,
			r->poll_pg, r->owner_pg, r->poll_thread, r->owner_thread,
			r->poll_core, r->owner_core, r->split_valid,
			r->post_tick, r->poll_tick, r->owner_tick, r->done_tick, x, y, z,
			r->poll_tick >= r->poll_start_tick ?
			urma_push_ticks_to_ns(r->poll_tick - r->poll_start_tick) : 0,
			gap_known, gap_known ? urma_push_ticks_to_ns(
				r->poll_start_tick - r->previous_poll_end_tick) : 0);
	}
}
#endif
