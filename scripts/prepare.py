#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Unprivileged, pinned download/extraction and build. No package installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEB_SHA = "b48c93c3732f90aabbcc520e5538faeffbb87bb6847a01d03e14ea157f1d36c1"
LIB_SHA = "4ee33eb988a62413698d1761b9c9baf09faca6a645f82ac19db6f33d04c8cb1b"
REFERENCE_SHA = "a08ecd4e025c0d19272650960895cec91f9ce6b456771bf9a8e31314a76bc7ce"
SOURCES = ("reference/ctfdavis-focal_spi.c", "module/Makefile", "module/focal_medion_baseline.c",
           "tools/baseline-client.c", "scripts/prepare.py", "scripts/run-baseline.py")
URL = ("https://raw.githubusercontent.com/oneXfive/ubuntu_spi/"
       "d534b7a3759a1e338f8d1b866db222d0e1674025/"
       "libfprint-2-2_1.94.4%2Btod1-0ubuntu1~22.04.2_spi_20250112_amd64.deb")
CORE = ("focal_spi_power_off", "focal_spi_power_on", "focal_spi_reset",
        "focal_spi_hw_reset", "focal_spi_configure_spi", "focal_spi_get_gpio_config")

def sha(data):
    return hashlib.sha256(data).hexdigest()

def extract_core(source):
    result = []
    for name in CORE:
        match = re.search(r"static (?:void|int)\s+" + name + r"\([^)]*\)\s*\{", source)
        if not match:
            raise ValueError("Missing reference function: " + name)
        pos, depth = match.end(), 1
        while depth and pos < len(source):
            depth += (source[pos] == "{") - (source[pos] == "}")
            pos += 1
        if depth:
            raise ValueError("Unterminated reference function: " + name)
        result.append(source[match.start():pos])
    return "/* SPDX-License-Identifier: GPL-2.0-only */\n/* Generated from pinned reference/ */\n" + "\n\n".join(result) + "\n"

def checked(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)

def prepare_library(build, deb):
    if deb:
        data = deb.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=30) as response:
            if not response.url.startswith("https://"):
                raise RuntimeError("Non-HTTPS download refused")
            data = response.read(2 * 1024 * 1024)
    if len(data) != 806394 or sha(data) != DEB_SHA:
        raise RuntimeError("Reference package size/SHA256 mismatch")
    package = build / "reference.deb"
    package.write_bytes(data)
    archive = checked(["ar", "p", str(package), "data.tar.zst"], capture_output=True).stdout
    library = checked(["tar", "--zstd", "-xOf", "-", "./usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0"],
                      input=archive, capture_output=True).stdout
    if sha(library) != LIB_SHA:
        raise RuntimeError("Extracted library SHA256 mismatch")
    (build / "libfprint-legacy.so").write_bytes(library)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deb", type=Path, help="Use an already downloaded, hash-verified package")
    parser.add_argument("--compile-only", action="store_true", help="Build without downloading/extracting a vendor library; not runnable")
    parser.add_argument("--kernel-build", type=Path, default=Path("/lib/modules") / os.uname().release / "build")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Build as your regular user, without sudo")
    if os.uname().machine != "x86_64":
        parser.error("The reference library is x86_64 only")
    for tool in ("cc", "make", "ar", "tar", "zstd", "pkg-config", "modinfo"):
        if not shutil.which(tool):
            parser.error("Missing build dependency: " + tool)
    kernel = args.kernel_build.resolve(strict=True)
    build = ROOT / ".baseline" / "build"
    if any(c.isspace() for c in str(build) + str(kernel)):
        parser.error("Kernel build paths must not contain whitespace; use a new checkout without spaces")
    build.mkdir(parents=True, exist_ok=True)
    (build / "manifest.json").unlink(missing_ok=True)
    reference = (ROOT / "reference/ctfdavis-focal_spi.c").read_bytes()
    if sha(reference) != REFERENCE_SHA:
        raise RuntimeError("Reference source SHA256 mismatch")
    sources = {name: sha((ROOT / name).read_bytes()) for name in SOURCES}
    (build / "ctfdavis-core.inc").write_text(extract_core(reference.decode()))
    for filename in ("Makefile", "focal_medion_baseline.c"):
        shutil.copyfile(ROOT / "module" / filename, build / filename)
    flags = shlex.split(checked(["pkg-config", "--cflags", "--libs", "gio-2.0", "glib-2.0"],
                    capture_output=True, text=True).stdout)
    checked(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tools/baseline-client.c"), "-o", str(build / "baseline-client"), *flags, "-ldl"])
    checked(["make", "-C", str(kernel), "M=" + str(build), "modules"])
    module = build / "focal_medion_baseline.ko"
    vermagic = checked(["modinfo", "-F", "vermagic", str(module)], capture_output=True, text=True).stdout.strip()
    if args.compile_only:
        print("Compile-only passed; no vendor library downloaded or executed. No runnable manifest created.")
        return
    prepare_library(build, args.deb)
    if sources != {name: sha((ROOT / name).read_bytes()) for name in SOURCES}:
        raise RuntimeError("Sources changed during build; prepare again")
    paths = ("baseline-client", "libfprint-legacy.so", "focal_medion_baseline.ko")
    manifest = {"format": 1, "vermagic": vermagic, "library_sha256": LIB_SHA,
                "files": {name: sha((build / name).read_bytes()) for name in paths},
                "sources": sources}
    (build / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Prepared open/close baseline; no vendor code or module was executed.")
    print("Module kernel:", vermagic.split()[0])
    print("Next: sudo python3 scripts/run-baseline.py --run")

if __name__ == "__main__":
    main()
