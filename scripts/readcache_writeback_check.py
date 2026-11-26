#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Validate W2 drain/accounting using MP v2 readcache_experiment snapshots.

This command only reads local JSON files. It does not connect to a cluster.
"""
import argparse
import json
from pathlib import Path


POLICY = 'volatile_early_ack_wait_read'
COUNTERS = ('accepted', 'committed', 'failed', 'accepted_bytes', 'committed_bytes',
            'failed_bytes', 'backpressure', 'read_waits', 'read_wait_us', 'commit_latency_us')


def integer(value):
    if type(value) is not int or value < 0:
        raise ValueError('expected a nonnegative integer')
    return value


def check(before, after):
    report = {'valid': False, 'errors': [], 'caches': {}, 'write_unit': 'bdev_request'}
    try:
        if before['schema'] != 1 or after['schema'] != 1:
            raise ValueError('unsupported MP snapshot schema')
        if before['errors'] or after['errors']:
            raise ValueError('snapshot contains collection errors')
        if not before['targets'] or set(before['targets']) != set(after['targets']):
            raise ValueError('empty or changed target set')
        for key, old in before['targets'].items():
            new = after['targets'][key]
            if old['uid'] != new['uid'] or old['containers'] != new['containers']:
                raise ValueError(f'{key}: target restarted or image changed')
            if not old['caches'] or set(old['caches']) != set(new['caches']):
                raise ValueError(f'{key}: empty or changed cache set')
            for name, a in old['caches'].items():
                label = f'{key}/{name}'
                b = new['caches'][name]
                if a['uuid'] != b['uuid'] or a['base_bdev'] != b['base_bdev']:
                    raise ValueError(f'{label}: bdev identity changed')
                s0, s1 = a['stats'], b['stats']
                for field in ('stats_epoch', 'generation', 'enabled', 'populate_on_write',
                              'capacity_bytes', 'max_io_bytes', 'write_ack_policy', 'read_commit_level'):
                    if s0[field] != s1[field]:
                        raise ValueError(f'{label}: {field} changed')
                if s1['write_ack_policy'] != POLICY or s1['read_commit_level'] != 'backend_complete':
                    raise ValueError(f'{label}: W2 is not enabled')
                w0, w1 = s0['writeback'], s1['writeback']
                for field in ('capacity_bytes', 'max_pending_writes', 'max_dirty_age_us'):
                    if integer(w0[field]) != integer(w1[field]) or not w1[field]:
                        raise ValueError(f'{label}: invalid or changed dirty configuration')
                for s in (s0, s1):
                    w = s['writeback']
                    for suffix in ('', '_bytes'):
                        if integer(w['accepted' + suffix]) != sum(integer(w[f + suffix]) for f in
                                                                 ('committed', 'failed', 'pending')):
                            raise ValueError(f'{label}: write accounting mismatch')
                    if w['poisoned'] is not False or integer(w['failed']):
                        raise ValueError(f'{label}: asynchronous write failure or poisoned device')
                    if (integer(w['pending']) or integer(w['pending_bytes']) or
                            integer(w['allocated_bytes']) or integer(w['active_operations'])):
                        raise ValueError(f'{label}: measurement endpoint is not drained')
                    if integer(w['peak_bytes']) > w['capacity_bytes']:
                        raise ValueError(f'{label}: dirty budget exceeded')
                    if integer(s['allocated_bytes']) > s['capacity_bytes']:
                        raise ValueError(f'{label}: CLEAN budget exceeded')
                delta = {field: integer(w1[field]) - integer(w0[field]) for field in COUNTERS}
                if min(delta.values()) < 0 or not delta['accepted']:
                    raise ValueError(f'{label}: decreased counters or no early-ACK activity')
                for field in ('io_errors', 'fill_errors'):
                    if integer(s1[field]) != integer(s0[field]):
                        raise ValueError(f'{label}: I/O errors during measurement')
                report['caches'][label] = {'delta': delta, 'dirty_peak_bytes': w1['peak_bytes'],
                                           'max_commit_latency_us': w1['max_commit_latency_us']}
    except (KeyError, TypeError, ValueError) as exc:
        report['errors'].append(str(exc))
    report['valid'] = bool(report['caches']) and not report['errors']
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    result = check(json.loads(args.before.read_text()), json.loads(args.after.read_text()))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['valid'] else 1)


if __name__ == '__main__':
    main()
