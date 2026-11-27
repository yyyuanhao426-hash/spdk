#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise real URMA creation against externally configured iobuf slot sizes."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from handshake_build import extract_function, sdk_headers

ROOT = Path(__file__).resolve().parents[2]


def takeover_checks():
    source = (ROOT / 'target_nvme_takeover.sh').read_text()
    start = source.index('build_urma_transport_args() {')
    helper = source[start:source.index('\n}', start) + 2]
    begin = source.index('IB_SMALL_COUNT=0;')
    parse_pool = source[begin:source.index('\n# Slot 大小', begin)]
    for size, spec, cache, allowed in [
        ('8388608', '16384,8192,1024,262144', '32', True),
        ('8388608', '16384,8192,1024,524288', '32', True),
        ('8388608', '', '32', True), ('0', '', '32', True),
        ('0', '16384,8192,1024,262144', '16', True),
        ('262144', '16384,8192,1024,8388608', '0', True),
        ('7', '', '32', False), ('393216', '', '32', False),
        ('-1', '', '32', False), ('08', '', '32', False),
        ('4294967296', '', '32', False), ('8388608', '', '-1', False),
    ]:
        code = 'abort() { exit 1; }; info() { :; };\n' + parse_pool + '\n' + helper
        code += '\nbuild_urma_transport_args\nprintf "%s\\n" "${URMA_TRANSPORT_ARGS[@]}"\n'
        code += 'printf "%s\\n" "pool=$IB_LARGE_COUNT,$IB_LARGE_SIZE"\n'
        result = subprocess.run(['bash', '-eu', '-c', code], text=True, capture_output=True,
            env=dict(os.environ, MAX_IO_SIZE=size, IOBUF_SPEC=spec, IOBUF_PG_CACHE=cache,
                     CAPSULE_TRANSPORT='tcp', HUGE_PAGES='2048'))
        assert (result.returncode == 0) == allowed, (size, spec, cache, result.stderr)
        if not allowed:
            continue
        lines = result.stdout.splitlines()
        assert ('-i' in lines) == (size != '0')
        if size != '0':
            assert lines[lines.index('-i') + 1] == size
        assert ('--iobuf-large-cache-size' in lines) == bool(spec or size != '0')
        if spec:
            parts = spec.split(',')
            assert f'pool={parts[2]},{parts[3]}' in lines
    print('PASS takeover: independent slot/max-I/O, unchanged counts/sizes, '
          'explicit cache with -I alone and invalid arguments', flush=True)


def buffer_function(source, name):
    start = re.search(r'^(?:static [^\n]*|int|void)\n' + re.escape(name) + r'\(',
                      source, re.M).start()
    brace = source.index('{', start)
    level, end = 1, brace + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--umdk', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    source = (ROOT / 'lib/nvmf/urma.c').read_text()
    types = source[source.index('#define NVMF_URMA_REG_CACHE_SIZE'):
                   source.index('static inline struct nvmf_urma_qpair *')]
    names = ['nvmf_urma_register_iobuf_chunk', 'nvmf_urma_unregister_iobuf_pool',
             'nvmf_urma_create', 'nvmf_urma_dump_opts']
    if re.search(r'^nvmf_urma_iobuf_max_io_size\(', source, re.M):
        names.insert(0, 'nvmf_urma_iobuf_max_io_size')
    functions = ''.join(extract_function(source, name) for name in names)
    buffers = (ROOT / 'lib/nvmf/transport.c').read_text()
    functions += ''.join(buffer_function(buffers, name) for name in (
        'nvmf_transport_use_iobuf', 'spdk_nvmf_request_free_buffers',
        'nvmf_request_set_buffer', 'nvmf_request_get_buffers', 'nvmf_request_iobuf_get_cb',
        'spdk_nvmf_request_get_buffers'))
    fixture = (ROOT / 'test/urma/iobuf_limits.c').read_text().replace(
        '/* REAL_TYPES */', types).replace('/* REAL_FUNCTIONS */', functions)
    with tempfile.TemporaryDirectory(prefix='spdk-urma-iobuf-') as tmp:
        work = Path(tmp)
        (work / 'spdk').mkdir()
        (work / 'spdk/config.h').write_text('')
        (work / 'fixture.c').write_text(fixture)
        flags = shlex.split(os.environ.get('CC', 'cc')) + [
            '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
            '-Wno-format', '-Wno-missing-field-initializers', '-Wno-pointer-sign',
            '-ffunction-sections', '-fdata-sections', '-I' + str(work),
            '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'lib/nvme'),
            '-I' + str(ROOT / 'lib/nvmf'), '-I' + str(core), '-I' + str(bond),
            *shlex.split(os.environ.get('CFLAGS', ''))]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
        binary = work / 'test'
        subprocess.run(flags + [str(work / 'fixture.c'),
            str(ROOT / 'lib/nvme/nvme_urma_common.c'),
            *[str(p) for p in sorted((ROOT / 'lib/json').glob('*.c'))],
            str(ROOT / 'lib/util/math.c'), '-Wl,--gc-sections',
            '-Wl,--wrap=spdk_urma_device_open', '-Wl,--wrap=spdk_urma_device_close',
            '-pthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
    subprocess.run(['bash', '-n', str(ROOT / 'target_nvme_takeover.sh')], check=True)
    takeover_checks()


if __name__ == '__main__':
    main()
