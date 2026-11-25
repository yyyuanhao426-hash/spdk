#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile production cache code and run without SSDs, DPDK or remote access."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index(name + '() {')
    # Target helper functions deliberately have no nested shell function defs.
    end = source.index('\n}', start) + 2
    return source[start:end]


def target_checks():
    source = (ROOT / 'target_nvme_takeover.sh').read_text()
    validate = function(source, 'validate_readcache_config')
    namespace = function(source, 'add_nof_namespace')
    setup = 'abort() { printf "%s\\n" "$*" >&2; exit 1; }; info() { :; };\n'
    for mib, size, warm, null, ok in [
        ('0', '256', '0', '0', True), ('512', '128', '1', '0', True),
        ('-1', '256', '0', '0', False), ('foo', '256', '0', '0', False),
        ('08', '256', '0', '0', False), ('1', '3', '0', '0', False),
        ('1', '65537', '0', '0', False), ('1', '256', '2', '0', False),
        ('1', '256', '0', '1', False),
    ]:
        script = setup + validate + '\nvalidate_readcache_config\n'
        result = subprocess.run(['bash', '-eu', '-c', script], text=True, capture_output=True,
            env=dict(os.environ, READ_CACHE_MIB=mib, READ_CACHE_MAX_IO_KIB=size,
                     READ_CACHE_POPULATE_ON_WRITE=warm, READ_CACHE_ENABLED='1', NULL_BDEV_MODE=null))
        assert (result.returncode == 0) == ok, (mib, size, warm, null, result.stderr)
    for mib, warm in [('0', '0'), ('512', '0'), ('512', '1')]:
        for enabled in ('0', '1'):
            for base in ('Nvme0n1', 'URMA_RAID0'):
                script = setup + namespace + '\nrun_rpc() { printf "%s\\n" "$*"; };\n'
                script += 'add_nof_namespace "$BASE" 1\n'
                result = subprocess.run(['bash', '-eu', '-c', script], text=True, capture_output=True,
                    env=dict(os.environ, READ_CACHE_MIB=mib, READ_CACHE_MAX_IO_KIB='256',
                             READ_CACHE_POPULATE_ON_WRITE=warm, READ_CACHE_ENABLED=enabled, NQN='test-nqn', BASE=base), check=True)
                lines = result.stdout.splitlines()
                if mib == '0':
                    assert lines == [f'nvmf_subsystem_add_ns test-nqn {base} -n 1'], lines
                else:
                    expected = f'bdev_readcache_create {base} NoFReadCache1 536870912 --max-io-bytes 262144'
                    if warm == '1':
                        expected += ' --populate-on-write'
                    if enabled == '0':
                        expected += ' --disabled'
                    assert lines == [expected, 'nvmf_subsystem_add_ns test-nqn NoFReadCache1 -n 1'], lines
    print('PASS takeover: validation before root/bind; off path unchanged; cache exported above single SSD/RAID', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--thread-sanitize', action='store_true')
    args = parser.parse_args()
    assert not (args.sanitize and args.thread_sanitize)
    flags = shlex.split(os.environ.get('CC', 'cc')) + [
        '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-missing-field-initializers',
        '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'include'),
        '-I' + str(ROOT / 'module/bdev/readcache')]
    flags += shlex.split(os.environ.get('CFLAGS', '')) + ['-UNDEBUG']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    if args.thread_sanitize:
        flags += ['-fsanitize=thread', '-fno-omit-frame-pointer', '-no-pie']
    with tempfile.TemporaryDirectory(prefix='spdk-readcache-test-') as tmp:
        for name in ('core', 'bdev', 'rpc'):
            binary = str(Path(tmp) / name)
            subprocess.run(flags + (['-Wno-pointer-sign'] if name == 'rpc' else []) + [str(ROOT / 'module/bdev/readcache/readcache.c'),
                str(ROOT / f'test/readcache/{name}.c'),
                *([str(p) for p in sorted((ROOT / 'lib/json').glob('*.c'))] if name == 'rpc' else []),
                '-Wl,--gc-sections',
                *(['-Wl,--wrap=malloc'] if name == 'core' else []),
                '-pthread', '-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=60)
        subprocess.run(flags + ['-fsyntax-only',
            *[str(p) for p in sorted((ROOT / 'module/bdev/readcache').glob('*.c'))]], check=True)
    subprocess.run(['bash', '-n', str(ROOT / 'target_nvme_takeover.sh')], check=True)
    target_checks()
    # --dry-run here belongs only to SPDK's RPC serializer: it does not connect
    # to a target. No benchmark runner or SSH helper is invoked.
    result = subprocess.run(['python3', str(ROOT / 'scripts/rpc.py'), '--dry-run',
        'bdev_readcache_create', 'URMA_RAID0', 'Cache0', '536870912', '--populate-on-write'],
        cwd=ROOT, text=True, capture_output=True, check=True)
    assert 'bdev_readcache_create' in result.stdout and '536870912' in result.stdout
    assert '"populate_on_write": true' in result.stdout
    result = subprocess.run(['python3', str(ROOT / 'scripts/rpc.py'), '--dry-run',
        'bdev_readcache_set_options', 'Cache0', '--no-enabled', '--clear', '--reset-stats'],
        cwd=ROOT, text=True, capture_output=True, check=True)
    assert '"enabled": false' in result.stdout and '"clear": true' in result.stdout
    print('PASS production module/RPC syntax, takeover syntax and CLI request serialization', flush=True)


if __name__ == '__main__':
    main()
