#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Huawei Technologies Co., Ltd.

"""Compile and exercise real URMA HELLO paths without URMA hardware.

The provider fixture exports opaque bytes which import requires intact. It tests
SPDK's transport contract, not the ABI or behavior of an installed ARM provider.
"""

import argparse
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
EXPORT_API = """
urma_status_t urma_get_rjetty(urma_jetty_t *, urma_rjetty_t **, uint32_t *);
void urma_put_rjetty(urma_rjetty_t *);
urma_status_t urma_get_seg_ctx(urma_target_seg_t *, urma_seg_t **, uint32_t *);
void urma_put_seg_ctx(urma_seg_t *);
"""


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, timeout=30, **kwargs)


def extract_function(source, name):
    start = re.search(r"^static [^\n]*\n" + re.escape(name) + r"\(", source, re.M).start()
    brace = source.index("{", start)
    level, end = 1, brace + 1
    while level:
        if source[end] == "{":
            level += 1
        elif source[end] == "}":
            level -= 1
        end += 1
    return source[start:end] + "\n"


def sdk_headers(prefix):
    candidates = [prefix / "include/ub/umdk/urma", prefix / "include",
                  prefix / "src/urma/lib/urma/core/include"]
    core = next((p for p in candidates if (p / "urma_api.h").is_file()), None)
    candidates.append(prefix / "src/urma/lib/urma/bond/include")
    bond = next((p for p in candidates if (p / "urma_ubagg.h").is_file()), None)
    if core is None or bond is None:
        raise RuntimeError("--umdk must contain urma_api.h and urma_ubagg.h")
    return core, bond


def configure_probe(work, core, bond, modern, compiler):
    headers, libdir = work / "sdk/include", work / "sdk/lib"
    headers.mkdir(parents=True, exist_ok=True)
    libdir.mkdir(parents=True, exist_ok=True)
    for directory in (core, bond):
        for header in directory.glob("*.h"):
            shutil.copy2(header, headers / header.name)
    if modern:
        with (headers / "urma_api.h").open("a") as out:
            out.write(EXPORT_API)
    stub = work / "sdk.c"
    init = ("#include <urma_api.h>\n"
            "urma_status_t urma_init(urma_init_attr_t *a) {return URMA_SUCCESS;}\n")
    exports = ("urma_status_t urma_get_rjetty(urma_jetty_t *j, urma_rjetty_t **r, "
               "uint32_t *n) {return URMA_SUCCESS;}\n"
               "void urma_put_rjetty(urma_rjetty_t *r) {}\n"
               "urma_status_t urma_get_seg_ctx(urma_target_seg_t *t, urma_seg_t **s, uint32_t *n) {return URMA_SUCCESS;}\n"
               "void urma_put_seg_ctx(urma_seg_t *s) {}\n")
    config = (ROOT / "configure").read_text()
    begin = config.index('if [ "${CONFIG[URMA]}" = "y" ]; then')
    end = config.index('\nif [[ "${CONFIG[FC]}"', begin)
    script = ("set -e\ndeclare -A CONFIG\nCONFIG[URMA]=y\nCONFIG[URMA_DIR]="
              + shlex.quote(str(work / "sdk"))
              + "\nCONFIG[URMA_PEERMEM]=n\nCONFIG[URMA_DMABUF]=n\nBUILD_CMD=("
              + shlex.join([*compiler, "-o", "/dev/null", "-x", "c"])
              + ")\n" + config[begin:end]
              + '\nprintf "Rjetty=%s SEG_CTX=%s\\n" "${CONFIG[URMA_RJETTY]}" "${CONFIG[URMA_SEG_CTX]}"\n')
    # Declarations without linkable symbols must not enable the feature.
    for link_exports in ([True, False] if modern else [False]):
        stub.write_text(init + (exports if link_exports else ""))
        run([*compiler, f"-I{headers}", "-c", str(stub), "-o", str(work / "sdk.o")])
        archive = libdir / "liburma.a"
        if archive.exists():
            archive.unlink()
        run(["ar", "rcs", str(archive), str(work / "sdk.o")])
        result = run(["bash", "-c", script], capture_output=True)
        expected = "y" if modern and link_exports else "n"
        assert f"Rjetty={expected} SEG_CTX={expected}" in result.stdout, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--umdk", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    core, bond = sdk_headers(args.umdk.resolve())
    compiler = shlex.split(os.environ.get("CC", "cc"))
    source = (ROOT / "lib/nvme/nvme_urma.c").read_text()
    target = (ROOT / "lib/nvmf/urma.c").read_text()
    functions = "".join(extract_function(source, name) for name in (
        "nvme_urma_write_full", "nvme_urma_read_full", "nvme_urma_exchange_hello"))
    functions += "".join(extract_function(target, name) for name in (
        "nvmf_urma_hs_fill", "nvmf_urma_hs_flush", "nvmf_urma_handshake_apply",
        "nvmf_urma_handshake_progress"))
    test = (ROOT / "test/urma/jetty_handshake.c").read_text().replace(
        "/* REAL_FUNCTIONS */", functions)
    with tempfile.TemporaryDirectory(prefix="spdk-urma-hello-") as tmp:
        work = Path(tmp)
        (work / "spdk").mkdir()
        (work / "test.c").write_text(test)
        api = work / "export_api.h"
        api.write_text('#include <urma_api.h>\n' + EXPORT_API)
        for modern in (True, False):
            (work / "spdk/config.h").write_text(
                "#define SPDK_CONFIG_URMA_RJETTY 1\n" if modern else "")
            flags = ["-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                     "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
                     "-include", str(api), f"-I{work}", f"-I{ROOT / 'include'}",
                     f"-I{ROOT / 'lib/nvme'}", f"-I{ROOT / 'lib/nvmf'}",
                     f"-I{core}", f"-I{bond}",
                     *shlex.split(os.environ.get("CFLAGS", ""))]
            if args.sanitize:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            for path in ("lib/nvme/nvme_urma_common.c", "lib/nvme/nvme_urma.c",
                         "lib/nvmf/urma.c"):
                # The full pre-existing transports have unrelated printf and
                # const diagnostics; keep the common code and harness strict.
                legacy_flags = [] if path.endswith("common.c") else [
                    "-Wno-format", "-Wno-discarded-qualifiers"]
                run([*compiler, *flags, *legacy_flags, "-c", str(ROOT / path),
                     "-o", str(work / (Path(path).stem + ".o"))])
            binary = work / "test"
            run([*compiler, *flags, str(work / "test.c"),
                 str(work / "nvme_urma_common.o"), "-Wl,--gc-sections",
                 "-pthread", "-o", str(binary)])
            run([str(binary)])
            configure_probe(work, core, bond, modern, compiler)
            print(f"PASS full transport compilation and configure API/link detection, modern={modern}")
            if modern:
                (work / "spdk/config.h").write_text(
                    "#define SPDK_CONFIG_URMA_RJETTY 1\n#define SPDK_CONFIG_URMA_SEG_CTX 1\n")
                for path in ("lib/nvme/nvme_urma_common.c", "lib/nvme/nvme_urma.c", "lib/nvmf/urma.c"):
                    extra = [] if path.endswith("common.c") else ["-Wno-format", "-Wno-discarded-qualifiers"]
                    run([*compiler, *flags, *extra, "-c", str(ROOT / path),
                         "-o", str(work / (Path(path).stem + ".o"))])
                data = (ROOT / "test/urma/bonding_data.c").read_text()
                target_functions = "".join(extract_function(target, name) for name in (
                    "nvmf_urma_next_jetty", "nvmf_urma_seg_hash", "nvmf_urma_import_cache_find",
                    "nvmf_urma_import_cache_fini", "nvmf_urma_post_data", "nvmf_urma_read_full",
                    "nvmf_urma_receive_capsule", "nvmf_urma_process_cmd_receive"))
                send_functions = "".join(extract_function(source, name) for name in (
                    "nvme_urma_next_jetty", "nvme_urma_tx_pool_init", "nvme_urma_send_cmd_capsule",
                    "nvme_urma_cr_owner_qpair", "nvme_urma_qpair_defer_error",
                    "nvme_urma_process_capsule_cr_inner", "nvme_urma_process_capsule_cr"))
                data = data.replace("/* REAL_TARGET_DATA_FUNCTIONS */", target_functions)
                data = data.replace("/* REAL_INITIATOR_SEND_FUNCTIONS */", send_functions)
                (work / "data.c").write_text(data)
                run([*compiler, *flags, str(work / "data.c"), str(work / "nvme_urma_common.o"),
                     "-Wl,--gc-sections", "-Wl,--wrap=spdk_urma_memory_chip_id", "-pthread",
                     "-o", str(work / "data")])
                run([str(work / "data")])



if __name__ == "__main__":
    main()
