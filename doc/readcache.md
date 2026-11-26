# Target host read cache

`readcache` is an optional bdev placed above a single NVMe bdev or a RAID bdev.
It shares cached data and in-flight reads across the bdev's channels. Nothing is
cached unless a readcache bdev is explicitly created and exported.

This first version uses host `malloc` memory and copies hits to the request's
existing I/O buffer. It does not change iobuf ownership, URMA registration or the
transport data-completion path. By default writes complete only after the base
device completes them. An explicit volatile early-ACK policy is described below.

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
This option alone does not enable early write acknowledgement.

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
memory. CLEAN data uses ordinary memory; optional W2 staging buffers use DMA
memory and need additional hugepage headroom.

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
order does not establish the final media value. In the default write policy,
FLUSH is forwarded unchanged. W2 adds ordering before this path.

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
are documented in `~/GDR/mooncake-logs/6.mooncake-vesegment/k8s_deploy/Bare_Nof_Without_Vsegment/READCACHE_V3_VALIDATION.md`.

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

## W2: volatile early ACK with read barriers

W2 targets the rank workload's one write followed by ten or more readers. It
copies a complete host WRITE payload into independent DMA storage, publishes a
range reservation, and completes the foreground write. A channel poller submits
the owned write independently. No background operation retains the original
request, iobuf, CID or qpair. After backend success the payload warms CLEAN cache
(even when `populate_on_write` is false), then DMA storage is released.

```bash
scripts/rpc.py bdev_readcache_create URMA_RAID0 NoFReadCache1 536870912 \
  --max-io-bytes 262144 \
  --write-ack-policy volatile_early_ack_wait_read \
  --dirty-capacity-bytes 67108864 --max-pending-writes 1024 \
  --max-dirty-age-us 1000000
```

These are starting budgets, not measured recommendations. For takeover, pass:

```bash
NOF_READ_CACHE_MIB=512
NOF_READ_CACHE_MAX_IO_KIB=256
NOF_WRITE_ACK_POLICY=volatile_early_ack_wait_read
NOF_DIRTY_MIB=64
NOF_MAX_PENDING_WRITES=1024
NOF_MAX_DIRTY_AGE_US=1000000
```

The write policy and dirty limits are immutable until device recreation. The
default is `backend_complete`, with no dirty allocation. Service v3 rank/recall
wiring is captured in [the deployment patch](readcache_service_v3_writeback.patch).
The Service variables, `start.sh` export/validation/envsubst, target Pod env, and
old-image check must all be updated together. The patch defaults to synchronous
writes. MP workload JSON, QPS, object size and SSD count do not change.

All reads enter a shared range queue before acquiring buffers or consulting
CLEAN cache. This includes disabled-cache, oversize, flagged and external-memory
reads. A read waits for every earlier overlapping mutation and reserves its
range until its data is copied/read into the original request buffer. Younger
overlapping writes wait for that read; disjoint operations may run concurrently.
Consequently a stream of later writes cannot extend a read's dependency set.
For cacheable ranges, concurrent cold readers still share one backend fill.

FUA and other flagged writes use synchronous completion, preserving flags and
backend status. In this fork, the NVMe bdev maps directive bits but does not map
the incoming CDW12 FUA bit into its hardware write flags; RAID may drop it too.
W2 therefore follows a successful FUA write with a real base Flush and holds the
mutation/range reservation until Flush succeeds, including resource retries.
Failed Flush fails that FUA request. Ordinary writes ignore CDW12's low 16 NLB
bits for admission, since NVMf passes the full command word for multi-block writes.
Flush and Reset reserve the entire namespace, including earlier
accepted writes still waiting in software; later operations wait until the
barrier completes. UNMAP and WRITE_ZEROES use range ordering. W2 advertises bdev
write cache even if the base does not: NVMf derives VWC from this property. This
fork's existing Set Features handler rejects WCE=0 as Feature Not Changeable.
Ordinary backend completion does not promise power-loss durability.

Dirty quota includes requested aligned DMA sizes and per-operation metadata;
reservations are charged before allocation, and held until both are freed. DMA
allocator/hugepage overhead and the fixed scheduler allocation are additional.
Reserve dirty hugepage capacity on top of the existing iobuf/backend budget; do
not reduce the iobuf reserve. The Service patch does not change Pod memory or
hugepage resources automatically. At the hard byte/slot limit, after reaching
the 90% watermark (resuming at 70%), or while an accepted write exceeds the age
limit, new eligible writes fall back to ordered synchronous completion. Allocation
failure has the same fallback. Age pressure never frees or cancels an ACKed write.
If the backend stops making progress, operations remain pending and visible;
the module does not free active DMA or invent a successful timeout.

Any background write failure latches **the entire wrapper** as poisoned. Queued
ACKed operations fail and release their payloads with explicit failed counters;
inflight operations retain their DMA until the backend callback. Waiting and new
reads fail instead of reading old SSD contents; subsequent writes and Flush also
fail. Strict mutation failures, base removal and resize poison W2 too. CLEAN
clear/disable/resize and stats reset cannot erase this latch or the writeback
counters. Recovery requires stopping the batch, invalidating its upper-layer
segment/locations, and recreating the wrapper for a new batch. Automatic segment
recovery after a target crash is not implemented. Never re-advertise the old SSD
contents as the previously ACKed data after a crash.

`bdev_readcache_get_stats` adds `write_ack_policy`, `read_commit_level` and a
`writeback` object. Its counters last for the bdev lifetime, independently of
CLEAN `stats_epoch`; their unit is one accepted bdev request, not one 8 MiB object:

```text
accepted = committed + failed + pending
accepted_bytes = committed_bytes + failed_bytes + pending_bytes
```

`allocated_bytes`, `peak_bytes`, pending byte/count peaks, `oldest_age_us`,
`backpressure`, `read_waits`, summed `read_wait_us`, summed `commit_latency_us` and
`max_commit_latency_us` expose pressure and delays. Commit latency is measured
from range admission through background completion processing, including queue
and cache-publication costs; it is not a transport ACK timestamp or a P99.
Both successful and failed background outcomes contribute to latency counters.
Synchronous fallback writes are absent from accepted/committed counters and remain
visible through normal bdev iostat and `io_errors`. A delete retains the base
claim, descriptor and independent channel references until background work drains.
`active_operations` includes foreground reads, synchronous fallback writes, FUA
and namespace barriers as well as background writes; a drained endpoint has zero.

Use MP v3's `readcache_experiment.py snapshot` before starting writers
and after stopping writers/readers and draining the target. Validate those local
snapshot files with:

```bash
python3 scripts/readcache_writeback_check.py before.json after.json
```

The checker rejects undrained endpoints, asynchronous errors, counter mismatch,
budget violations, target/bdev replacement and windows without early-ACK activity.
MP v3 comparison measures CLEAN read avoidance; add `--writeback` to also
validate W2 drain and accounting using its bundled checker. Neither checker
proves object contents, per-object commit, actual network ACK timing, or end-to-end
performance. Compare R0/R1/W2 using version-varying payloads and full verification,
then measure writer-to-all-selected-readers latency. Keep the original official
JSON as a baseline: `verify_level=size` cannot detect stale contents.

Local W2 regression covers ten and 64 readers, request/buffer destruction after
ACK, multi-range and partial-overlap ordering, ten OS threads/10,000 reservations,
strict FUA, queued Flush, UNMAP/zeroes/Reset, all read bypasses, resource retries,
DMA/slot/age backpressure, fail-closed errors and asynchronous teardown/removal.
It uses a simulated backend. Real NVMe DMA, URMA disconnects, target termination,
crash generation isolation, object verification and performance remain deployment
acceptance work. Stage 5 cache-to-URMA zero-copy is separate and unchanged.
