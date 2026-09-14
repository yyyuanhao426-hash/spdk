# SPDK / Mooncake URMA implementation comparison

This comparison describes the local SPDK `perf_boost` branch and the local
Mooncake checkout, not upstream releases. Source paths below are relative to
the workspace containing both repositories.

## Remote segment import alignment

SPDK target now imports a remote segment on the first use and caches the result
for subsequent reads and writes. Requests borrow the cached pointer; request
completion or failure no longer unimports it. Qpair teardown clears the cache;
the worker-owned URMA context remains alive until transport teardown.

The worker-context cache uses 128 hash buckets with collision chains, not a
128-entry capacity limit. Keys contain EID, UASID, registered base address,
segment length, attribute bits and token ID. Padding is normalized before
hashing/comparison. A different I/O offset within the same segment reuses the
same entry; a changed segment identity gets a separate entry. Failed allocation
or import does not insert an entry.

Mooncake's `UrmaContext::retrieveRemoteSeg()` similarly caches imports, using
`std::unordered_map<string, urma_target_seg_t*>` keyed by the serialized segment
description. SPDK scopes its cache to the worker URMA context. It retains the
existing qpair polling ownership and protects the shared context cache with a
reader/writer lock, like Mooncake. Reconnection within the transport can reuse
imports; transport teardown clears them.

## Data-plane alignment

The target now creates one long-lived URMA device/context shared by its configured workers.
SPDK memory notifications register existing and future host hugepage regions once
in that context. Data requests first perform a page-map translation and reuse the
covering registration; buffers outside SPDK registered hugepages retain the old
per-qpair registration-cache fallback.

Ready data operations are queued per qpair. A poll pass receives up to
`batch_size` capsules and links ready embedded WRs into one provider post. The
batch is bounded by the qpair jetty depth, shared JFC depth and configured batch
size. Partial-post failures preserve the successfully posted prefix and complete
only the rejected suffix with an NVMe transport error.

Qpairs are round-robin assigned to workers and to the corresponding
SPDK transport poll groups, so worker execution uses SPDK reactor threads.
Each worker owns one JFC and its JFR. Each JFC has a shared
atomic outstanding counter, while each qpair also tracks its jetty outstanding
count. A JFR is created for every JFC so each jetty's send and shared receive
queues use the same completion queue.

Configuration is available through transport-specific JSON fields
`worker_count` and `batch_size`, or `SPDK_URMA_WORKERS_PER_CTX` and
`SPDK_URMA_BATCH_SIZE`. Compatibility aliases `MC_WORKERS_PER_CTX`,
`MC_NUM_CQ_PER_CTX` and `MC_MAX_WR` are accepted. Native names
`SPDK_URMA_JFC_COUNT` and `SPDK_URMA_JETTY_DEPTH` take precedence.

This is application-level object caching. `URMA_NON_CACHEABLE` remains the
provider flag; it does not disable the application's hash table. SPDK retains
READ/WRITE access flags; Mooncake also requests ATOMIC access.

Neither this change nor Mooncake's cache introduces a remote invalidation
protocol. The peer must keep a registration valid while it is being accessed.
Identical descriptor reuse after deregistration cannot be distinguished by
these keys. Distinct descriptors accumulate until cache teardown, as in the
Mooncake implementation; workloads with continually changing registrations
should account for this memory growth.

Sources: `spdk/lib/nvmf/urma.c`,
`Mooncake/mooncake-transfer-engine/src/transport/kunpeng_transport/urma/urma_endpoint.cpp`,
`Mooncake/mooncake-transfer-engine/include/transport/kunpeng_transport/urma/urma_endpoint.h`.

## Remaining differences

| Area | SPDK | Mooncake |
| --- | --- | --- |
| Data flow | NVMe write: target READs initiator memory, then executes bdev write. NVMe read: target executes bdev read, then WRITEs to initiator memory. | Transfer Engine caller chooses READ or WRITE. Ordinary Store memory-replica Put submits WRITE. |
| Control messages | TCP carries endpoint handshake, per-I/O capsules and completions. Segment descriptor travels as binary capsule data. | Memory registration publishes metadata; segment bytes are hex encoded. TE submits one-sided transfers using those descriptors, without an NVMe capsule per transfer. |
| Connection setup | Import jetty during qpair handshake; bind in RC mode. | Worker can establish an unconnected endpoint during transfer processing. |
| Local registration | Target preregisters SPDK registered host hugepages once in the shared context and uses a memory-map lookup; non-hugepage buffers fall back to the old request cache. Initiator still uses its request cache. | Explicit registerLocalMemory registers regions and publishes descriptors for subsequent transfers. |
| Target buffers | Obtain SPDK iobuf buffers on capsule receipt; current post_data requires one iovec. | Transfers operate on registered local/remote regions and are split into slices. |
| Submission | Ready operations are chained into a configurable batch. Each qpair has one jetty; qpairs are spread over workers and JFCs in the shared context. | Endpoint has multiple jettys; submitPostSend builds WR chains subject to jetty/JFC depth limits. |
| Completion/failure | JFC completion drives PULLING/EXECUTING/PUSHING states. Flow counters reopen submission capacity. Data errors return NVMe internal errors without retry. | Worker tracks slices and retries failed transfers subject to retry limits. |
| Buffer lifetime | Transport buffers serve a bdev I/O lifecycle, then return to the pool. | TE provides memory transfers; storage policy belongs to higher layers. |

Source locations for the remaining differences:

- `spdk/lib/nvmf/urma.c`: create_jetty, handshake, post_data, receive_capsule,
  poll_group_poll, release_req and req_complete.
- `spdk/lib/nvme/nvme_urma.c`: request registration cache and capsule submission.
- `spdk/lib/nvmf/ctrlr_bdev.c`: spdk_bdev_writev_blocks_ext.
- `Mooncake/mooncake-transfer-engine/src/transport/kunpeng_transport/ub_transport.cpp`:
  registerLocalMemory and submitTransferTask.
- `Mooncake/mooncake-transfer-engine/src/transport/kunpeng_transport/ub_context.cpp`:
  endpoint connection, submission and slice retry processing.
- `Mooncake/mooncake-transfer-engine/src/transport/kunpeng_transport/urma/urma_endpoint.cpp`:
  retrieveRemoteSeg and submitPostSend.
- `Mooncake/mooncake-store/src/client_service.cpp`: Put memory-replica WRITE submission.

## Validation

Run the mock-provider regression on Linux with Python 3 and a C compiler:

```sh
python3 test/nvmf/urma_import_cache.py
```

The test compiles the production import-cache helpers and checks reuse, padding
normalization, all identity fields, connection isolation, collisions, allocation
and import failures, cleanup and reuse after cleanup. It does not replace a
full SPDK build or URMA hardware tests. The implementation was edited on Windows
without a C compiler or WSL distribution; compilation and hardware validation
remain outstanding. `git diff --check` passed.

Hardware validation should include repeated reads/writes of a registered buffer,
multiple offsets in one segment, multiple qpairs, disconnect/reconnect and error
cleanup. Measure import counts and CPU cost before drawing performance claims:
the local UDMA provider's import is user-space allocation/initialization, not a
network handshake.
