#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Ensure W2 cannot be declared drained after failures/restart/no activity."""
import copy
import importlib.util
from pathlib import Path

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('checker', root / 'scripts/readcache_writeback_check.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

stats = dict(stats_epoch=1, generation=1, enabled=True, populate_on_write=False,
             capacity_bytes=65536, max_io_bytes=4096, allocated_bytes=10000,
             io_errors=0, fill_errors=0, write_ack_policy=checker.POLICY,
             read_commit_level='backend_complete', writeback=dict.fromkeys(checker.COUNTERS, 0))
stats['writeback'].update(capacity_bytes=65536, max_pending_writes=32, max_dirty_age_us=1000,
                          pending=0, pending_bytes=0, allocated_bytes=0, peak_bytes=0,
                          max_commit_latency_us=0, poisoned=False, active_operations=0)
before = dict(schema=1, errors=[], targets={'rank/store': dict(uid='pod1', containers=[['target', 0]],
              caches={'wb': dict(uuid='bdev1', base_bdev='raid', stats=stats)})})
after = copy.deepcopy(before)
after['targets']['rank/store']['caches']['wb']['stats']['writeback'].update(
    accepted=10, committed=10, accepted_bytes=40960, committed_bytes=40960, peak_bytes=8192)
assert checker.check(before, after)['valid']
assert not checker.check(before, before)['valid']
for field, value in [('pending', 1), ('failed', 1), ('poisoned', True),
                     ('allocated_bytes', 4096), ('peak_bytes', 65537),
                     ('max_pending_writes', 33), ('accepted_bytes', 40959), ('active_operations', 1)]:
    bad = copy.deepcopy(after)
    bad['targets']['rank/store']['caches']['wb']['stats']['writeback'][field] = value
    assert not checker.check(before, bad)['valid'], field
for field, value in [('uuid', 'recreated'), ('base_bdev', 'different')]:
    bad = copy.deepcopy(after)
    bad['targets']['rank/store']['caches']['wb'][field] = value
    assert not checker.check(before, bad)['valid'], field
for field, value in [('io_errors', 1), ('stats_epoch', 2), ('write_ack_policy', 'backend_complete')]:
    bad = copy.deepcopy(after)
    bad['targets']['rank/store']['caches']['wb']['stats'][field] = value
    assert not checker.check(before, bad)['valid'], field
bad = copy.deepcopy(after)
bad['targets']['rank/store']['containers'] = [['target', 1]]
assert not checker.check(before, bad)['valid']
assert not checker.check({}, {})['valid']
print('PASS W2 snapshot accounting: drain, failures, pending bytes, dirty limits, identity, '
      'restart, policy/config changes and no-activity rejection')
