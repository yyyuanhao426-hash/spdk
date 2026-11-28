#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check target push diagnostics and real JFC timing without URMA hardware."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from handshake_build import sdk_headers

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--umdk', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    with tempfile.TemporaryDirectory(prefix='spdk-urma-push-profile-') as tmp:
        work = Path(tmp)
        (work / 'spdk').mkdir()
        (work / 'spdk/config.h').write_text('')
        flags = shlex.split(os.environ.get('CC', 'cc')) + [
            '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-unused-function', '-ffunction-sections',
            '-fdata-sections', '-I' + str(work), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'lib/nvme'), '-I' + str(core), '-I' + str(bond)]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
        binary = work / 'test'
        subprocess.run(flags + [str(ROOT / 'test/urma/push_profile.c'),
            str(ROOT / 'lib/nvme/nvme_urma_common.c'), '-Wl,--gc-sections',
            '-pthread', '-o', str(binary)], check=True)
        env = dict(os.environ, SPDK_URMA_TARGET_PUSH_PROFILE='1',
                   SPDK_URMA_TARGET_PUSH_TRACE_US='1000')
        subprocess.run([str(binary)], env=env, check=True, timeout=30)
        env['SPDK_URMA_TARGET_PUSH_PROFILE'] = '0'
        subprocess.run([str(binary), '--disabled'], env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
