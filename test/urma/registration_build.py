#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Huawei Technologies Co., Ltd.

"""Build real URMA registration code against SDK variants, without GPU hardware.

Pass --umdk pointing to a UMDK source/install prefix containing the GPU extensions.
The baseline fixture removes both extensions to catch accidental dependencies.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def sdk_headers(prefix):
    candidates = (prefix / "include/ub/umdk/urma", prefix / "include",
                  prefix / "src/urma/lib/urma/core/include")
    core = next((p for p in candidates if (p / "urma_api.h").is_file()), None)
    bond = next((p for p in (*candidates, prefix / "src/urma/lib/urma/bond/include")
                 if (p / "urma_ubagg.h").is_file()), None)
    if core is None or bond is None:
        raise RuntimeError("--umdk must contain urma_api.h and urma_ubagg.h")
    return core, bond


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--umdk", type=Path, required=True)
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    compiler = shlex.split(os.environ.get("CC", "cc"))
    extra_flags = shlex.split(os.environ.get("CFLAGS", ""))
    configure = (ROOT / "configure").read_text()
    probe_start = configure.index('if [ "${CONFIG[URMA]}" = "y" ]; then')
    probe_end = configure.index('\nif [[ "${CONFIG[FC]}"', probe_start)
    probes = configure[probe_start:probe_end]

    for option in ("--with-urma-peermem", "--with-urma-dmabuf"):
        result = subprocess.run(["bash", str(ROOT / "configure"), "--without-urma", option],
                                text=True, capture_output=True, cwd=ROOT)
        assert result.returncode != 0 and "require --with-urma" in result.stdout

    with tempfile.TemporaryDirectory(prefix="spdk-urma-build-") as tmp:
        work = Path(tmp)
        for peermem, dmabuf in ((False, False), (True, False), (False, True), (True, True)):
            name = f"peermem={int(peermem)},dmabuf={int(dmabuf)}"
            fixture = work / name
            headers = fixture / "include"
            headers.mkdir(parents=True)
            for directory in (core, bond):
                for header in directory.glob("*.h"):
                    shutil.copy2(header, headers / header.name)
            if not peermem:
                types = headers / "urma_types.h"
                text, count = re.subn(r"^\s*uint32_t\s+is_gpu_seg\s*;[^\n]*\n", "",
                                      types.read_text(), flags=re.M)
                if count != 1:
                    raise RuntimeError("Test requires an SDK with uint32_t is_gpu_seg")
                types.write_text(text)
            if not dmabuf:
                api = headers / "urma_api.h"
                text, count = re.subn(r"urma_target_seg_t\s*\*\s*urma_register_seg_dmabuf\s*"
                                      r"\([^;]*\);", "", api.read_text())
                if count != 1:
                    raise RuntimeError("Test requires an SDK with DMA-BUF declaration")
                api.write_text(text)
            config_dir = headers / "spdk"
            config_dir.mkdir()
            config = ""
            if peermem:
                config += "#define SPDK_CONFIG_URMA_PEERMEM 1\n"
            if dmabuf:
                config += "#define SPDK_CONFIG_URMA_DMABUF 1\n"
            (config_dir / "config.h").write_text(config)
            cflags = ["-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                      "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                      "-ffunction-sections", "-fdata-sections", f"-I{headers}",
                      f"-I{ROOT / 'include'}", f"-I{ROOT / 'lib/nvme'}", *extra_flags]
            obj = fixture / "registration.o"
            run([*compiler, *cflags, "-c", str(ROOT / "lib/nvme/nvme_urma_common.c"),
                 "-o", str(obj)])
            symbols = run(["nm", "-u", str(obj)], capture_output=True).stdout
            assert ("urma_register_seg_dmabuf" in symbols) == dmabuf, name
            binary = fixture / "registration"
            run([*compiler, *cflags, str(ROOT / "test/urma/memory_registration.c"),
                 str(obj), "-Wl,--gc-sections", "-pthread", "-o", str(binary)])
            run([str(binary)])
            run([*compiler, *cflags, "-c", str(ROOT / "examples/nvme/urma_perf/urma_perf.c"),
                 "-o", str(fixture / "perf.o")])

            # Exercise the real configure probes with a matching static SDK.
            libdir = fixture / "lib"
            libdir.mkdir()
            stub = fixture / "sdk.c"
            source = '#include "urma_api.h"\n'
            source += "urma_status_t urma_init(urma_init_attr_t *attr) { return URMA_SUCCESS; }\n"
            if dmabuf:
                source += ("urma_target_seg_t *urma_register_seg_dmabuf(urma_context_t *ctx, "
                           "urma_seg_cfg_t *cfg, int fd, uint64_t off) { return NULL; }\n")
            stub.write_text(source)
            stub_obj = fixture / "sdk.o"
            run([*compiler, f"-I{headers}", "-c", str(stub), "-o", str(stub_obj)])
            run(["ar", "rcs", str(libdir / "liburma.a"), str(stub_obj)])
            for requested_peer, requested_dma in ((False, False), (True, False),
                                                 (False, True), (True, True)):
                script = "set -e\ndeclare -A CONFIG\n"
                script += "CONFIG[URMA]=y\n"
                script += f"CONFIG[URMA_DIR]={shlex.quote(str(fixture))}\n"
                script += f"CONFIG[URMA_PEERMEM]={'y' if requested_peer else 'n'}\n"
                script += f"CONFIG[URMA_DMABUF]={'y' if requested_dma else 'n'}\n"
                script += "BUILD_CMD=(" + shlex.join([*compiler, "-o", "/dev/null", "-x", "c"]) + ")\n"
                result = subprocess.run(["bash", "-c", script + probes], text=True,
                                        capture_output=True)
                expected = ((not requested_peer or peermem) and
                            (not requested_dma or dmabuf))
                assert (result.returncode == 0) == expected, (name, result.stdout, result.stderr)

            if dmabuf:
                # Keep the declaration but omit the symbol: configure must reject it at link time.
                no_symbol = fixture / "no-dmabuf-symbol"
                (no_symbol / "lib").mkdir(parents=True)
                (no_symbol / "include").symlink_to(headers, target_is_directory=True)
                stub.write_text('#include "urma_api.h"\n'
                                'urma_status_t urma_init(urma_init_attr_t *attr) { return URMA_SUCCESS; }\n')
                run([*compiler, f"-I{headers}", "-c", str(stub), "-o", str(stub_obj)])
                run(["ar", "rcs", str(no_symbol / "lib/liburma.a"), str(stub_obj)])
                script = "set -e\ndeclare -A CONFIG\nCONFIG[URMA]=y\n"
                script += f"CONFIG[URMA_DIR]={shlex.quote(str(no_symbol))}\n"
                script += "CONFIG[URMA_PEERMEM]=n\nCONFIG[URMA_DMABUF]=y\n"
                script += "BUILD_CMD=(" + shlex.join([*compiler, "-o", "/dev/null", "-x", "c"]) + ")\n"
                result = subprocess.run(["bash", "-c", script + probes], text=True,
                                        capture_output=True)
                assert result.returncode != 0 and "linkable urma_register_seg_dmabuf" in result.stdout
            print(f"PASS {name}: registration, cleanup, symbols, perf compilation, configure probes")


if __name__ == "__main__":
    main()
