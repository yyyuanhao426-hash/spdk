# Target host read cache

`readcache` is an optional bdev placed above a single NVMe bdev or a RAID bdev.
It shares cached data and in-flight reads across the bdev's channels. Nothing is
cached unless a readcache bdev is explicitly created and exported.

This first version uses host `malloc` memory and copies hits to the request's
existing I/O buffer. It does not change iobuf ownership, URMA registration or the
transport data-completion path. All writes still reach the base device, and their
completion status is propagated before optional cache warming becomes visible.

## Create and inspect

After creating a base bdev, before exporting it as an NVMe-oF namespace:

```bash
scripts/rpc.py -s /var/tmp/spdk.sock bdev_readcache_create \
  URMA_RAID0 NoFReadCache1 536870912 --max-io-bytes 262144
scripts/rpc.py -s /var/tmp/spdk.sock nvmf_subsystem_add_ns \
  nqn.2026-01.io.spdk:urma-gpu-test NoFReadCache1 -n 1
scripts/rpc.py -s /var/tmp/spdk.sock bdev_readcache_get_stats NoFReadCache1
```

This allocates entries lazily within a 512 MiB budget. Requests larger than
256 KiB bypass the cache; match `max_io_bytes` to observed NVMe requests, not the
full Mooncake object size. A ready or loading entry may also satisfy a fully contained subrange. The module does not stitch several entries into one hit or expand a backend read.
Add `--populate-on-write` only when measuring write-warmed caching separately.
There is no write-back mode.

The base bdev is claimed exclusively. Create the wrapper before exporting the
namespace; do not retain a separately writable export of the underlying device.
To delete, first detach its consumers/remove its namespace, then use
`bdev_readcache_delete NoFReadCache1`. Deletion does not switch an existing
namespace back to the base device. Restart without the cache to restore baseline.

## NoF takeover script

The existing `target_nvme_takeover.sh` honors:

| Variable | Default | Meaning |
|---|---:|---|
| `NOF_READ_CACHE_MIB` | `0` | Off; positive value is one target's cache budget in MiB |
| `NOF_READ_CACHE_MAX_IO_KIB` | `256` | Maximum cacheable request size in KiB |
| `NOF_READ_CACHE_ENABLED` | `1` | `0` creates an explicitly bypassed wrapper when MIB is positive |
| `NOF_READ_CACHE_POPULATE_ON_WRITE` | `0` | `1`: warm after successful, non-conflicting backend WRITE |

Use these as additional environment variables with your existing, reviewed
takeover invocation. The script wraps the base as `NoFReadCache1` and exports the
wrapper. It accepts caching only for a single namespace: one real SSD or a RAID
aggregate. Null mode and multiple independent namespaces are rejected when
caching is enabled. With the default `0`, RPC export commands remain unchanged.

Rebuild SPDK and the runtime image to include the new bdev library, RPC handler,
CLI and takeover script. Kubernetes configuration must pass these variables to
the target container; setting them only in an MP client does not enable caching.
No workload JSON or Mooncake C++ change is required. Keep the normal-memory
limit large enough for the cache, iobuf-related allocations and other runtime
memory; this cache does not consume the iobuf hugepage budget itself.

## Counters and limits

- `read_requests` and `read_bytes`: all reads received by the wrapper after buffer acquisition, including external/accel/special-flag bypasses. Buffer acquisition failures are separately reflected in `io_errors`.
- `hits`: ready entries; `coalesced`: followers of a loading entry;
  `miss_leaders`: reads elected to issue a backend request.
- `bypasses`: oversize requests, active overlapping mutations, allocation failure
  or a full pinned budget. Bypass is a normal backend request, not an I/O failure.
- `read_fills`, `write_fills`, `fill_errors`, `invalidations`, `evictions` expose
  admission and invalidation behavior.
- `allocated_bytes` includes the fixed cache/index, entries and retained data,
  including invalidated entries pinned by readers. `peak_bytes` is the high-water mark since the last stats reset and may exceed the current capacity after a successful shrink. Live `allocated_bytes` stays within `capacity_bytes`. These are requested live allocation sizes, not total process
  RSS: libc allocator metadata, fragmentation and retained arenas are additional.

Use `stats_epoch` and `generation` to detect resets and configuration/clear boundaries. Compare counter deltas around each run, and use the base NVMe/RAID iostat for
physical read reduction. A wrapper's read I/O count still includes cache hits;
it must not be interpreted as SSD read IOPS.

WRITE, UNMAP, WRITE_ZEROES, RESET and resize invalidate overlapping data before
new reads can hit it. A loading read invalidated by a write cannot later
republish its old data. New reads during overlapping mutations bypass both ready
and loading entries. Overlapping writes are never warmed, because completion
order does not establish the final media value. FLUSH is forwarded unchanged.

Only host buffers, metadata-free conventional bdevs and READ/WRITE/UNMAP/
WRITE_ZEROES/FLUSH/RESET are supported. COPY, fused compare/write, ZCOPY, ABORT
and NVMe passthrough are not advertised. A failed merged read fails its
followers; errors are not cached. Completion occurs on each reader's own SPDK
thread. In-flight teardown relies on SPDK channel/I/O draining; a simulated
framework regression is included, but real transport disconnect testing remains
part of deployment validation.

## Online control and measurement

```bash
scripts/rpc.py bdev_readcache_set_options NoFReadCache1 --no-enabled --clear
scripts/rpc.py bdev_readcache_set_options NoFReadCache1 --enabled --clear --reset-stats
scripts/rpc.py bdev_readcache_set_options NoFReadCache1 --populate-on-write
scripts/rpc.py bdev_readcache_set_options NoFReadCache1 --capacity-bytes 268435456
```

All options in one request are validated before applying changes. Shrinking below
pinned allocations returns EBUSY, with the old options preserved. Clearing and
disabling unpublish old entries; existing readers keep their references, and
pre-clear writes cannot warm the new generation. Disable does not detach the
wrapper or bypass SSD writes. Reset statistics after stopping a test and take a
new before snapshot; in-flight completions can otherwise cross the reset epoch.

The module reports byte counters, per-reason bypass counters, pinned/ready/loading
gauges and logical `io_errors`, including failed merged readers. A small 1 ms
owner-thread cleanup poller is reserved at create time so asynchronous destruction
can close the descriptor on its owner without allocating a message during
teardown. The bdev delete completion is delivered only after channels drain.

Kubernetes Service parameters, MP v2 snapshots and the read-only experiment tool
are documented in `~/GDR/mooncake-logs/6.mooncake-vesegment/k8s_deploy/Bare_Nof_Without_Vsegment/READCACHE_VALIDATION.md`.

## Local regression

```bash
python3 test/readcache/run.py
python3 test/readcache/run.py --sanitize
make -C module/bdev/readcache -j2
```

Tests use the production core and bdev adapter with a controlled local backend;
they do not access disks or remote nodes. RPC handlers additionally use the actual SPDK JSON parser, decoder and writer, with only the framework and response envelope mocked. They cover ten-way fanout, actual byte
comparisons, late stale completions, overlapping writes, budget exhaustion,
allocation failure, failed reads, resource-wait retry, mutations, lifecycle and
20,000 seeded read/overwrite operations. Full target/image compilation and real
SSD/URMA P99 measurements are separate validation steps.
