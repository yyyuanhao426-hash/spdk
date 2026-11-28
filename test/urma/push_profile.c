/* SPDX-License-Identifier: BSD-3-Clause */
#include "nvme_urma_internal.h"
#include "spdk_internal/urma_push_profile.h"

static uint64_t tick;
static pthread_mutex_t poll_lock = PTHREAD_MUTEX_INITIALIZER;
static bool check_return_lock;
static int poll_result;

uint64_t spdk_get_ticks_hz(void) { return 1000000; }
uint64_t
spdk_get_ticks(void)
{
	if (check_return_lock) {
		assert(pthread_mutex_trylock(&poll_lock) != 0);
		check_return_lock = false;
	}
	return tick;
}

int
urma_poll_jfc(urma_jfc_t *jfc, int count, urma_cr_t *cr)
{
	assert(pthread_mutex_trylock(&poll_lock) != 0);
	tick += 20;
	check_return_lock = true;
	return poll_result;
}

static unsigned
occurrences(const char *text, const char *needle)
{
	unsigned count = 0;

	while ((text = strstr(text, needle)) != NULL) {
		count++;
		text += strlen(needle);
	}
	return count;
}

static char *
dump(void)
{
	char *text = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&text, &size);

	assert(out);
	urma_push_profile_dump(out);
	assert(fclose(out) == 0);
	return text;
}

int
main(int argc, char **argv)
{
	if (argc > 1) {
		assert(!urma_push_profile_enabled());
		struct urma_push_record r = {};
		urma_push_profile_record(&r);
		char *text = dump();
		assert(text[0] == '\0');
		free(text);
		puts("PASS push profiling disabled: no records or output");
		return 0;
	}
	assert(urma_push_profile_enabled());
	urma_jfc_t *jfcs[1] = {NULL};
	struct spdk_urma_jfc_poll_history previous[1] = {};
	struct spdk_urma_device device = {
		.send_jfc_count = 1, .send_jfcs = jfcs,
		.send_jfc_poll_locks = &poll_lock, .send_jfc_poll_history = previous,
	};
	struct spdk_urma_jfc_poll_timing timing;
	urma_cr_t cr;
	tick = 100;
	poll_result = 1;
	assert(spdk_urma_device_poll_send_jfc_timed(&device, 0, 1, &cr, &timing) == 1);
	assert(timing.start_tick == 100 && timing.end_tick == 120 && !timing.previous_end_tick);
	assert(!urma_push_profile_poll(100, 120, 0, false, 1, 0));
	tick = 220;
	poll_result = 0;
	assert(spdk_urma_device_poll_send_jfc_timed(&device, 0, 1, &cr, &timing) == 0);
	assert(timing.start_tick == 220 && timing.end_tick == 240 &&
	       timing.previous_end_tick == 120);
	assert(!urma_push_profile_poll(220, 240, 120, false, 0, 1));
	assert(pthread_mutex_lock(&poll_lock) == 0);
	assert(spdk_urma_device_poll_send_jfc_timed(&device, 0, 1, &cr, &timing) == 0);
	assert(timing.busy && timing.end_tick == 0 && previous[0].end_tick == 240);
	assert(!urma_push_profile_poll(0, 0, 0, true, 0, 2));
	assert(pthread_mutex_unlock(&poll_lock) == 0);
	tick = 2000;
	poll_result = 0;
	assert(spdk_urma_device_poll_send_jfc_timed(&device, 0, 1, &cr, &timing) == 0);
	assert(urma_push_profile_poll(timing.start_tick, timing.end_tick,
				     timing.previous_end_tick, timing.busy, 0, 3));
	struct urma_push_poll_record poll = {
		.start = timing.start_tick, .end = timing.end_tick,
		.previous = timing.previous_end_tick, .jfc = 1, .pg = 3, .count = 0,
	};
	urma_push_profile_poll_record(&poll);
	struct urma_push_record r = {
		.post_tick = 100, .poll_tick = 2100, .owner_tick = 4100, .done_tick = 4300,
		.poll_start_tick = 2080, .previous_poll_end_tick = 2070,
		.forwarded = true, .qid = 2, .cid = 7, .lba = 123, .length = 262144,
		.poll_thread = 1, .owner_thread = 4, .poll_core = 1, .owner_core = 4,
	};
	strcpy(r.peer, "192.168.0.1");
	urma_push_profile_record(&r);
	assert(r.split_valid);
	assert(g_urma_push_profile.post_to_poll + g_urma_push_profile.poll_to_owner +
	       g_urma_push_profile.owner_to_done == 4200);
	r.poll_tick = 99; /* Completion can be polled before post returns. */
	urma_push_profile_record(&r);
	r.poll_tick = 0;
	urma_push_profile_record(&r);
	r.poll_tick = 4200; /* Invalid owner/poll ordering must not wrap. */
	urma_push_profile_record(&r);
	char *text = dump();
	assert(strstr(text, "valid=1 untracked=1 early_poll=1 invalid=1"));
	assert(strstr(text, "post_to_poll_us=2000 poll_to_owner_us=2000 owner_to_done_us=200"));
	assert(strstr(text, "URMA_JFC_POLL_DETAIL"));
	assert(strstr(text, "completions=0 call_ns=20000 gap_known=1 gap_ns=1760000"));
	assert(occurrences(text, "URMA_PUSH_DETAIL") == 4);
	free(text);
	r.poll_tick = 2100;
	for (unsigned i = 0; i < 70; i++) {
		urma_push_profile_record(&r);
	}
	text = dump();
	assert(strstr(text, "details=64 overwritten=6"));
	assert(occurrences(text, "URMA_PUSH_DETAIL") == 64);
	free(text);
	text = dump();
	assert(occurrences(text, "URMA_PUSH_DETAIL") == 0);
	free(text);
	puts("PASS real timed JFC poll: return tick under lock, "
	     "previous physical poll and contention");
	puts("PASS push profile: 4.2ms decomposition, invalid/missing/early stamps, "
	     "slow empty polls, bounded output and no duplicate details");
	return 0;
}
