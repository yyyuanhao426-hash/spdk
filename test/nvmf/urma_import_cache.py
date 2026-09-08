#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile the production cache helpers against a mock URMA provider.

Run with Python 3 and a C compiler (CC, or cc) on Linux. No URMA hardware needed.
The helpers are extracted so the test exercises the implementation in urma.c.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[2] / "lib/nvmf/urma.c"


def main():
    source = SOURCE.read_text(encoding="utf-8")
    entry = source[source.index("struct nvmf_urma_import_entry {"):
                   source.index("struct nvmf_urma_reg_entry {")]
    helpers = source[source.index("static int\nnvmf_urma_import_remote_seg("):
                     source.index("static struct spdk_nvmf_transport_poll_group *\n"
                                  "nvmf_urma_get_optimal_poll_group(")]
    helpers += source[source.index("static void\nnvmf_urma_clear_import_cache("):
                      source.index("static int\nnvmf_urma_post_data(")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#define NVMF_URMA_IMPORT_BUCKETS 128
#define SPDK_URMA_DEFAULT_TOKEN 123
#define URMA_NON_CACHEABLE 0
#define URMA_ACCESS_READ 2
#define URMA_ACCESS_WRITE 4
#define URMA_SEG_NOMAP 0
typedef struct { unsigned char raw[16]; } eid_t;
typedef struct {
    struct { eid_t eid; uint32_t uasid; uint64_t va; } ubva;
    uint64_t len;
    union { uint32_t value; } attr;
    uint32_t token_id;
} urma_seg_t;
typedef struct { urma_seg_t seg; } urma_target_seg_t;
typedef struct { uint32_t token; } urma_token_t;
typedef struct { struct { unsigned cacheable, access, mapping; } bs; }
    urma_import_seg_flag_t;
static unsigned imports, unimports;
static bool fail_import, fail_alloc;
static void *test_calloc(size_t n, size_t size) {
    if (fail_alloc) { fail_alloc = false; return NULL; }
    return calloc(n, size);
}
static urma_target_seg_t *urma_import_seg(void *ctx, urma_seg_t *seg,
        urma_token_t *token, uint64_t addr, urma_import_seg_flag_t flag) {
    assert(ctx && token->token == SPDK_URMA_DEFAULT_TOKEN && addr == 0);
    assert(flag.bs.access == (URMA_ACCESS_READ | URMA_ACCESS_WRITE));
    if (fail_import) { fail_import = false; return NULL; }
    urma_target_seg_t *t = malloc(sizeof(*t));
    assert(t);
    t->seg = *seg;
    imports++;
    return t;
}
static int urma_unimport_seg(urma_target_seg_t *t) {
    assert(t); unimports++; free(t); return 0;
}
'''
    harness += entry
    harness += r'''
struct device { void *context; };
struct nvmf_urma_worker {
    struct device *device;
};
struct nvmf_urma_transport {
    pthread_rwlock_t import_lock;
    struct nvmf_urma_import_entry *import_cache[NVMF_URMA_IMPORT_BUCKETS];
};
struct nvmf_urma_qpair {
    struct device *device;
    struct nvmf_urma_worker *worker;
    struct nvmf_urma_transport *transport;
};
#define calloc test_calloc
'''
    harness += helpers
    harness += r'''
#undef calloc
int main(void) {
    struct device d = {.context = &d};
    struct nvmf_urma_worker worker = {.device = &d}, other_worker = {.device = &d};
    struct nvmf_urma_transport transport, other_transport;
    memset(&transport, 0, sizeof(transport));
    memset(&other_transport, 0, sizeof(other_transport));
    assert(pthread_rwlock_init(&transport.import_lock, NULL) == 0);
    assert(pthread_rwlock_init(&other_transport.import_lock, NULL) == 0);
    struct nvmf_urma_qpair q = {.device = &d, .worker = &worker, .transport = &transport};
    struct nvmf_urma_qpair same_worker = {.device = &d, .worker = &worker, .transport = &transport};
    struct nvmf_urma_qpair other = {.device = &d, .worker = &other_worker,
                                    .transport = &other_transport};
    urma_seg_t seg = {0}, same;
    urma_target_seg_t *a, *b;
    seg.ubva.va = 4096; seg.len = 8192; seg.token_id = 7;
    assert(nvmf_urma_import_remote_seg(&q, &seg, &a) == 0);
    assert(nvmf_urma_import_remote_seg(&q, &seg, &b) == 0);
    assert(a == b && imports == 1);
    assert(nvmf_urma_import_remote_seg(&same_worker, &seg, &b) == 0);
    assert(a == b && imports == 1);
    /* Semantically equal descriptors with different padding must hit. */
    memset(&same, 0xa5, sizeof(same));
    same.ubva.eid = seg.ubva.eid; same.ubva.uasid = seg.ubva.uasid;
    same.ubva.va = seg.ubva.va; same.len = seg.len;
    same.attr.value = seg.attr.value; same.token_id = seg.token_id;
    assert(nvmf_urma_import_remote_seg(&q, &same, &b) == 0 && a == b);
    /* Distinct registration identities, including token changes, must miss. */
#define CHECK_FIELD(field) do { \
    same = seg; same.field++; \
    unsigned before = imports; \
    assert(nvmf_urma_import_remote_seg(&q, &same, &b) == 0); \
    assert(b != a && imports == before + 1); \
} while (0)
    CHECK_FIELD(ubva.va); CHECK_FIELD(ubva.uasid); CHECK_FIELD(ubva.eid.raw[0]);
    CHECK_FIELD(len); CHECK_FIELD(attr.value); CHECK_FIELD(token_id);
    /* Another worker/context never borrows this worker's entries. */
    assert(nvmf_urma_import_remote_seg(&other, &seg, &b) == 0 && b != a);
    same = seg; same.token_id = 100;
    fail_alloc = true;
    assert(nvmf_urma_import_remote_seg(&q, &same, &b) == -ENOMEM);
    fail_import = true;
    assert(nvmf_urma_import_remote_seg(&q, &same, &b) == -EIO);
    assert(nvmf_urma_import_remote_seg(&q, &same, &b) == 0);
    /* More keys than buckets force collisions; every key remains reusable. */
    for (unsigned i = 1000; i < 1400; i++) {
        same.token_id = i;
        assert(nvmf_urma_import_remote_seg(&q, &same, &b) == 0);
    }
    unsigned before = imports;
    for (unsigned i = 1000; i < 1400; i++) {
        same.token_id = i;
        assert(nvmf_urma_import_remote_seg(&q, &same, &b) == 0);
    }
    assert(imports == before && unimports == 0);
    nvmf_urma_clear_import_cache(&transport);
    nvmf_urma_clear_import_cache(&other_transport);
    assert(unimports == imports);
    nvmf_urma_clear_import_cache(&transport);
    assert(unimports == imports);
    assert(nvmf_urma_import_remote_seg(&q, &seg, &b) == 0);
    nvmf_urma_clear_import_cache(&transport);
    assert(unimports == imports);
    pthread_rwlock_destroy(&transport.import_lock);
    pthread_rwlock_destroy(&other_transport.import_lock);
    puts("URMA import cache tests passed");
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        c_file = Path(tmp) / "cache_test.c"
        binary = Path(tmp) / "cache_test"
        c_file.write_text(harness)
        command = shlex.split(os.environ.get("CC", "cc"))
        try:
            subprocess.run(command + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
                                      str(c_file), "-o", str(binary)], check=True)
        except FileNotFoundError as error:
            raise SystemExit(f"C compiler not found: {command[0]}") from error
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
