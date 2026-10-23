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

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("urma_lifetime", NULL, NULL);
	CU_ADD_TEST(suite, test_close_blocks_new_references);
	CU_ADD_TEST(suite, test_reinit_only_after_drain);
	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
