#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Replay Identify submit and registration failures through production code."""
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
    types = source[source.index('struct nvme_urma_timing {'):source.index('/* Modified By Yida(v6): per-I/O tx trace CSV')]
    types += source[source.index('#define NVME_URMA_REG_CACHE_SIZE'):source.index('struct nvme_urma_ctrlr {')]
    names = ('nvme_urma_qpair', 'nvme_urma_cid_alloc', 'nvme_urma_cid_free',
             'nvme_urma_reg_cache_lookup', 'nvme_urma_reg_cache_insert',
             'nvme_urma_reg_cache_release', 'nvme_urma_write_full',
             'nvme_urma_req_memory_type', 'nvme_urma_qpair_submit_request')
    functions = ''.join(extract_function(source, name) for name in names)
    fixture = (ROOT / 'test/urma/admin_submit.c').read_text().replace('/* REAL_TYPES */', types).replace('/* REAL_FUNCTIONS */', functions)
    with tempfile.TemporaryDirectory(prefix='spdk-urma-admin-') as tmp:
        work = Path(tmp)
        (work / 'spdk').mkdir()
        (work / 'spdk/config.h').write_text('#define SPDK_CONFIG_URMA_SEG_CTX 1\n')
        (work / 'api.h').write_text('#include <urma_api.h>\n' + EXPORT_API)
        (work / 'test.c').write_text(fixture)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-function',
            '-Wno-missing-field-initializers', '-ffunction-sections', '-fdata-sections',
            '-include', str(work / 'api.h'), '-I' + str(work), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'lib/nvme'), '-I' + str(core), '-I' + str(bond),
            *shlex.split(os.environ.get('CFLAGS', ''))]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command + [str(work / 'test.c'), str(ROOT / 'lib/nvme/nvme_urma_common.c'),
            '-Wl,--gc-sections', '-Wl,--wrap=spdk_urma_memory_chip_id', '-pthread', '-o', str(work / 'test')], check=True)
        subprocess.run([str(work / 'test')], check=True)

if __name__ == '__main__':
    main()
