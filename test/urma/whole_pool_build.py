#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise production whole-pool registration with shared qpair contexts."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from handshake_build import EXPORT_API, extract_function, sdk_headers

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--umdk', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    source = (ROOT / 'lib/nvme/nvme_urma.c').read_text()
    types = source[source.index('struct nvme_urma_timing {'):
                   source.index('/* Modified By Yida(v6): per-I/O tx trace CSV')]
    types += source[source.index('#define NVME_URMA_REG_CACHE_SIZE'):
                    source.index('struct nvme_urma_ctrlr {')]
    # Keep the public entry point intact, including its call into common.c.
    start = source.index('int\nspdk_nvme_urma_register_memory_for_qpair(')
    brace = source.index('{', start)
    level, end = 1, brace + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    functions = extract_function(source, 'nvme_urma_qpair') + source[start:end]
    fixture = (ROOT / 'test/urma/whole_pool.c').read_text().replace(
        '/* REAL_TYPES */', types).replace('/* REAL_FUNCTIONS */', functions)
    with tempfile.TemporaryDirectory(prefix='spdk-urma-pool-') as tmp:
        work = Path(tmp)
        (work / 'spdk').mkdir()
        (work / 'api.h').write_text('#include <urma_api.h>\n' + EXPORT_API)
        (work / 'test.c').write_text(fixture)
        for seg_ctx in (False, True):
            (work / 'spdk/config.h').write_text(
                '#define SPDK_CONFIG_URMA_PEERMEM 1\n' +
                ('#define SPDK_CONFIG_URMA_SEG_CTX 1\n' if seg_ctx else ''))
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
                '-Wno-missing-field-initializers', '-ffunction-sections', '-fdata-sections',
                '-include', str(work / 'api.h'), '-I' + str(work),
                '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'lib/nvme'),
                '-I' + str(core), '-I' + str(bond),
                *shlex.split(os.environ.get('CFLAGS', ''))]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(command + [str(work / 'test.c'),
                str(ROOT / 'lib/nvme/nvme_urma_common.c'), '-Wl,--gc-sections',
                '-Wl,--wrap=calloc', '-pthread', '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True, timeout=30)
            print(f'PASS whole-pool ownership, provider segment context={seg_ctx}', flush=True)


if __name__ == '__main__':
    main()
