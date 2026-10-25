/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#ifndef SPDK_INTERNAL_URMA_LIFETIME_H
#define SPDK_INTERNAL_URMA_LIFETIME_H

#include "spdk/stdinc.h"

/*
 * A posted URMA WR owns one reference from before the post call until its
 * completion handler has finished.  Closing and the reference count share one
 * atomic word so that no reference can be acquired after close observes zero.
 */
#define SPDK_URMA_LIFETIME_CLOSING (UINT64_C(1) << 63)
#define SPDK_URMA_LIFETIME_REFS_MASK (~SPDK_URMA_LIFETIME_CLOSING)

struct spdk_urma_lifetime {
	uint64_t state;
};

/*
 * Receive WRs are different from ordinary completion-enabled WRs: a qpair
 * keeps one receive posted for every capsule slot for its whole lifetime.
 * Some providers (including udma) only return send WRs from flush_jetty(), so
 * these references must be retired after the receive queue has been destroyed.
 *
 * A true posted bit means the lifetime reference still belongs to the receive
 * queue.  Polling atomically claims that bit before the CR leaves the JFC lock;
 * the same reference then belongs to the completion handler.  Consequently a
 * queue teardown can retire only unclaimed receives without stealing a
 * reference from a handler which is already using the qpair.
 */
struct spdk_urma_rx_ref {
	bool posted;
};

static inline void
spdk_urma_lifetime_init(struct spdk_urma_lifetime *lifetime)
{
	__atomic_store_n(&lifetime->state, 0, __ATOMIC_RELEASE);
}

static inline bool
spdk_urma_lifetime_try_reopen(struct spdk_urma_lifetime *lifetime)
{
	uint64_t expected = SPDK_URMA_LIFETIME_CLOSING;

	return __atomic_compare_exchange_n(&lifetime->state, &expected, 0, false,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline bool
spdk_urma_lifetime_try_get(struct spdk_urma_lifetime *lifetime)
{
	uint64_t old = __atomic_load_n(&lifetime->state, __ATOMIC_ACQUIRE);

	for (;;) {
		if (old & SPDK_URMA_LIFETIME_CLOSING) {
			return false;
		}
		assert((old & SPDK_URMA_LIFETIME_REFS_MASK) != SPDK_URMA_LIFETIME_REFS_MASK);
		if (__atomic_compare_exchange_n(&lifetime->state, &old, old + 1, true,
						__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

static inline uint64_t
spdk_urma_lifetime_put(struct spdk_urma_lifetime *lifetime)
{
	uint64_t old = __atomic_fetch_sub(&lifetime->state, 1, __ATOMIC_ACQ_REL);

	assert((old & SPDK_URMA_LIFETIME_REFS_MASK) != 0);
	return (old - 1) & SPDK_URMA_LIFETIME_REFS_MASK;
}

static inline uint64_t
spdk_urma_lifetime_close(struct spdk_urma_lifetime *lifetime)
{
	uint64_t old = __atomic_fetch_or(&lifetime->state, SPDK_URMA_LIFETIME_CLOSING,
					 __ATOMIC_ACQ_REL);

	return old & SPDK_URMA_LIFETIME_REFS_MASK;
}

static inline bool
spdk_urma_lifetime_is_closing(const struct spdk_urma_lifetime *lifetime)
{
	return (__atomic_load_n(&lifetime->state, __ATOMIC_ACQUIRE) &
		SPDK_URMA_LIFETIME_CLOSING) != 0;
}

static inline uint64_t
spdk_urma_lifetime_refs(const struct spdk_urma_lifetime *lifetime)
{
	return __atomic_load_n(&lifetime->state, __ATOMIC_ACQUIRE) &
	       SPDK_URMA_LIFETIME_REFS_MASK;
}

static inline bool
spdk_urma_rx_ref_post_begin(struct spdk_urma_rx_ref *rx_ref,
			    struct spdk_urma_lifetime *lifetime)
{
	if (!spdk_urma_lifetime_try_get(lifetime)) {
		return false;
	}
	assert(!__atomic_load_n(&rx_ref->posted, __ATOMIC_ACQUIRE));
	__atomic_store_n(&rx_ref->posted, true, __ATOMIC_RELEASE);
	return true;
}

static inline bool
spdk_urma_rx_ref_claim(struct spdk_urma_rx_ref *rx_ref)
{
	return __atomic_exchange_n(&rx_ref->posted, false, __ATOMIC_ACQ_REL);
}

static inline void
spdk_urma_rx_ref_post_failed(struct spdk_urma_rx_ref *rx_ref,
			     struct spdk_urma_lifetime *lifetime)
{
	bool posted = spdk_urma_rx_ref_claim(rx_ref);

	assert(posted);
	spdk_urma_lifetime_put(lifetime);
}

static inline bool
spdk_urma_rx_ref_is_posted(const struct spdk_urma_rx_ref *rx_ref)
{
	return __atomic_load_n(&rx_ref->posted, __ATOMIC_ACQUIRE);
}

static inline bool
spdk_urma_rx_ref_retire(struct spdk_urma_rx_ref *rx_ref,
			struct spdk_urma_lifetime *lifetime)
{
	if (!spdk_urma_rx_ref_claim(rx_ref)) {
		return false;
	}
	spdk_urma_lifetime_put(lifetime);
	return true;
}

#endif /* SPDK_INTERNAL_URMA_LIFETIME_H */
