/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SPDK_VBDEV_READCACHE_H
#define SPDK_VBDEV_READCACHE_H

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "readcache.h"

int bdev_readcache_create(const char *base_name, const char *name, uint64_t capacity,
			  uint64_t max_io, bool populate_on_write);
void bdev_readcache_delete(const char *name, spdk_bdev_unregister_cb cb, void *arg);
int bdev_readcache_get_stats(const char *name, struct readcache_stats *stats);
int bdev_readcache_set_options(const char *name, const struct readcache_options *options);

#endif
