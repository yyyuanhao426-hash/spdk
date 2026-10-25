/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk_internal/urma_lifetime.h"

static void
test_close_blocks_new_references(void)
{
	struct spdk_urma_lifetime lifetime;

	spdk_urma_lifetime_init(&lifetime);
	CU_ASSERT(spdk_urma_lifetime_try_get(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_try_get(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_close(&lifetime) == 2);
	CU_ASSERT(spdk_urma_lifetime_is_closing(&lifetime));
	CU_ASSERT(!spdk_urma_lifetime_try_get(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_put(&lifetime) == 1);
	CU_ASSERT(spdk_urma_lifetime_put(&lifetime) == 0);
}

static void
test_reinit_only_after_drain(void)
{
	struct spdk_urma_lifetime lifetime;

	spdk_urma_lifetime_init(&lifetime);
	CU_ASSERT(spdk_urma_lifetime_try_get(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_close(&lifetime) == 1);
	CU_ASSERT(spdk_urma_lifetime_refs(&lifetime) == 1);
	CU_ASSERT(!spdk_urma_lifetime_try_reopen(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_put(&lifetime) == 0);

	CU_ASSERT(spdk_urma_lifetime_try_reopen(&lifetime));
	CU_ASSERT(!spdk_urma_lifetime_is_closing(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_try_get(&lifetime));
	CU_ASSERT(spdk_urma_lifetime_put(&lifetime) == 0);
}

static void
test_retire_posted_receives_after_close(void)
{
	struct spdk_urma_lifetime lifetime;
	struct spdk_urma_rx_ref rx[128] = {};
	const size_t rx_count = sizeof(rx) / sizeof(rx[0]);

	spdk_urma_lifetime_init(&lifetime);
	for (size_t i = 0; i < rx_count; i++) {
		CU_ASSERT(spdk_urma_rx_ref_post_begin(&rx[i], &lifetime));
	}
	CU_ASSERT(spdk_urma_lifetime_close(&lifetime) == rx_count);

	/* A receive queue destroy cancels receives which never produced a CR. */
	for (size_t i = 0; i < rx_count; i++) {
		CU_ASSERT(spdk_urma_rx_ref_retire(&rx[i], &lifetime));
	}
	CU_ASSERT(spdk_urma_lifetime_refs(&lifetime) == 0);
}

static void
test_claimed_receive_survives_queue_destroy(void)
{
	struct spdk_urma_lifetime lifetime;
	struct spdk_urma_rx_ref rx[2] = {};

	spdk_urma_lifetime_init(&lifetime);
	CU_ASSERT(spdk_urma_rx_ref_post_begin(&rx[0], &lifetime));
	CU_ASSERT(spdk_urma_rx_ref_post_begin(&rx[1], &lifetime));
	/* Polling transfers rx[0]'s posted reference to its completion handler. */
	CU_ASSERT(spdk_urma_rx_ref_claim(&rx[0]));
	CU_ASSERT(spdk_urma_lifetime_close(&lifetime) == 2);

	CU_ASSERT(!spdk_urma_rx_ref_retire(&rx[0], &lifetime));
	CU_ASSERT(spdk_urma_rx_ref_retire(&rx[1], &lifetime));
	CU_ASSERT(spdk_urma_lifetime_refs(&lifetime) == 1);
	CU_ASSERT(spdk_urma_lifetime_put(&lifetime) == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("urma_lifetime", NULL, NULL);
	CU_ADD_TEST(suite, test_close_blocks_new_references);
	CU_ADD_TEST(suite, test_reinit_only_after_drain);
	CU_ADD_TEST(suite, test_retire_posted_receives_after_close);
	CU_ADD_TEST(suite, test_claimed_receive_survives_queue_destroy);
	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
