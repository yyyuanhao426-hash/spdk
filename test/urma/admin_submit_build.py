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
    parser.add_argument('--transport-ref', help='Replay this git revision of the initiator')
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    source = (ROOT / 'lib/nvme/nvme_urma.c').read_text()
    if args.transport_ref:
        source = subprocess.run(
            ['git', 'show', f'{args.transport_ref}:lib/nvme/nvme_urma.c'],
            cwd=ROOT, check=True, capture_output=True, text=True).stdout
    types = source[source.index('struct nvme_urma_timing {'):source.index('/* Modified By Yida(v6): per-I/O tx trace CSV')]
    types += source[source.index('#define NVME_URMA_REG_CACHE_SIZE'):source.index('struct nvme_urma_ctrlr {')]
    names = ('nvme_urma_qpair', 'nvme_urma_cid_alloc', 'nvme_urma_cid_free',
             'nvme_urma_reg_cache_lookup', 'nvme_urma_reg_cache_insert',
             'nvme_urma_reg_cache_release', 'nvme_urma_request_has_transient_payload',
             'nvme_urma_admin_region_diag',
             'nvme_urma_write_full',
             'nvme_urma_req_memory_type', 'nvme_urma_acquire_request_memory',
             'nvme_urma_release_request_memory', 'nvme_urma_qpair_submit_request',
             'nvme_urma_ticks_to_us', 'nvme_urma_monotonic_ns',
             'nvme_urma_complete_response', 'nvme_urma_qpair_abort_reqs')
    functions = ''.join(extract_function(source, name) for name in names
                        if f'\n{name}(' in source)
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
            '-Wno-discarded-qualifiers',
            '-Wno-missing-field-initializers', '-ffunction-sections', '-fdata-sections',
            '-include', str(work / 'api.h'), '-I' + str(work), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'lib/nvme'), '-I' + str(core), '-I' + str(bond),
            *shlex.split(os.environ.get('CFLAGS', ''))]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command + [str(work / 'test.c'), str(ROOT / 'lib/nvme/nvme_urma_common.c'),
            '-Wl,--gc-sections', '-Wl,--wrap=spdk_urma_memory_chip_id',
            '-Wl,--wrap=posix_memalign', '-pthread', '-o', str(work / 'test')], check=True)
        subprocess.run([str(work / 'test')], check=True)

if __name__ == '__main__':
    main()
