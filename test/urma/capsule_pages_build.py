#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Replay Doc 65 page-grant overlap through production capsule pool functions."""
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
    parser.add_argument('--transport-ref', help='Replay pool functions from a git revision')
    parser.add_argument('--case', default='all', choices=[
        'all', 'target-rx', 'target-tx', 'initiator-rx', 'initiator-tx'])
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    def transport(path):
        if args.transport_ref:
            return subprocess.run(['git', 'show', args.transport_ref + ':' + path],
                                  cwd=ROOT, text=True, capture_output=True, check=True).stdout
        return (ROOT / path).read_text()

    target = transport('lib/nvmf/urma.c')
    initiator = transport('lib/nvme/nvme_urma.c')
    types = target[target.index('struct nvmf_urma_reg_entry {'):
                   target.index('/* Modified By Yida(v3): target-side staged')]
    types += target[target.index('enum nvmf_urma_req_state {'):
                    target.index('struct nvmf_urma_port {')]
    types += initiator[initiator.index('#define NVME_URMA_REG_CACHE_SIZE'):
                       initiator.index('struct nvme_urma_ctrlr {')]
    functions = ''.join(extract_function(target, name) for name in (
        'nvmf_urma_post_cmd_receive', 'nvmf_urma_rsp_tx_pool_init',
        'nvmf_urma_capsule_rx_post', 'nvmf_urma_capsule_resources_init',
        'nvmf_urma_capsule_resources_fini'))
    functions += ''.join(extract_function(initiator, name) for name in (
        'nvme_urma_post_rsp_receive', 'nvme_urma_capsule_resources_init',
        'nvme_urma_capsule_resources_fini', 'nvme_urma_tx_pool_init'))
    fixture = (ROOT / 'test/urma/capsule_pages.c').read_text().replace(
        '/* REAL_TYPES */', types).replace('/* REAL_FUNCTIONS */', functions)
    with tempfile.TemporaryDirectory(prefix='spdk-urma-capsule-pages-') as tmp:
        work = Path(tmp)
        (work / 'spdk').mkdir()
        (work / 'spdk/config.h').write_text('')
        (work / 'api.h').write_text('#include <urma_api.h>\n' + EXPORT_API)
        (work / 'fixture.c').write_text(fixture)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
            '-Wno-missing-field-initializers', '-ffunction-sections', '-fdata-sections',
            '-include', str(work / 'api.h'), '-I' + str(work),
            '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'lib/nvme'),
            '-I' + str(ROOT / 'lib/nvmf'), '-I' + str(core), '-I' + str(bond),
            *shlex.split(os.environ.get('CFLAGS', ''))]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
        binary = work / 'test'
        subprocess.run(command + [str(work / 'fixture.c'),
            str(ROOT / 'lib/nvme/nvme_urma_common.c'), '-Wl,--gc-sections',
            '-Wl,--wrap=posix_memalign', '-Wl,--wrap=sysconf',
            '-pthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary), args.case], check=True, timeout=30)


if __name__ == '__main__':
    main()
