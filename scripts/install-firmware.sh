#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Install independently catalogued payloads; no vendor code is executed.
set -eu
if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: Python 3 is required by this firmware installer." >&2
  exit 1
fi
exec python3 - "$@" <<'FTE3600_INSTALLER_PYTHON'
import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import resource
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request


@dataclass(frozen=True)
class Firmware:
    filename: str
    size: int
    sha256: str
    offsets: tuple


# Payload facts match drivers/fte3600-sensor.c and the independently documented
# 2.0.3.99/.100/.102 inventory. Every offset candidate must pass size AND hash.
CATALOG = {
    "ft9338": (Firmware("ft9338.bin", 14184,
        "ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46", (465312, 412752)),),
    "ft9348": (Firmware("ft9348.bin", 10312,
        "48d658d588c297a5d749c1f4bd6a0f5bd3d6ede59040e1674f95fe9db08eede2", (479504, 426944)),),
    "ft9361": (Firmware("ft9361.bin", 10396,
        "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f", (489824, 437264, 141824)),),
    "ft9536": (Firmware("ft9536.bin", 11934,
        "4a62b5d9a8a7b4620bec7843763b030632a5a37a54c4cb7ed2cd96d5c7451e79", (533440, 480880)),),
    "ft9368": (
        Firmware("ft9368-app.bin", 27120,
            "9997aafac8eb9a1aecc7ba5b41212004fe1b325e7bdd1ea9ca08ae54b803e2e0", (500224, 447664)),
        Firmware("ft9368-pramboot.bin", 6096,
            "c93a807eaaa34d9e79fbab77b86e910a419fd633ea98b628b72c64b285ffd191", (527344, 474784)),
    ),
}
NO_FIRMWARE = {"ft9365", "ft9369", "ft9769"}
CAB_URL = ("https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2026/08/"
           "e684f740-91ac-4458-9097-09850eaedf9d_4f80a6cb0c9e453d4619667af7c92eadeb165e0f.cab")
MAX_DLL_SIZE = 16 * 1024 * 1024
MAX_CAB_SIZE = 64 * 1024 * 1024
EXTRACT_TIMEOUT = 60


def read_regular(path, limit):
    """Read one bounded regular-file snapshot; FIFO inputs must not block."""
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise ValueError(f"{path}: expected a regular file of at most {limit} bytes")
        parts, total = [], 0
        while total <= before.st_size:
            block = os.read(fd, min(65536, before.st_size + 1 - total))
            if not block:
                break
            parts.append(block)
            total += len(block)
        if total != before.st_size or os.fstat(fd).st_size != before.st_size:
            raise ValueError(f"{path}: file size changed while reading")
        return b"".join(parts)
    finally:
        os.close(fd)


def validate(payload, firmware):
    if len(payload) != firmware.size:
        raise ValueError(f"{firmware.filename}: expected {firmware.size} bytes, got {len(payload)}")
    if hashlib.sha256(payload).hexdigest() != firmware.sha256:
        raise ValueError(f"{firmware.filename}: SHA-256 does not match the selected chip")


def extract_dll(data, firmware):
    for offset in firmware.offsets:
        candidate = data[offset:offset + firmware.size]
        if len(candidate) == firmware.size and hashlib.sha256(candidate).hexdigest() == firmware.sha256:
            return candidate
    raise ValueError(f"{firmware.filename}: no verified payload at the documented DLL offsets; "
                     "supply the exact extracted binary instead")


def download_cab(path):
    # Validate payload hashes, not an unverified Windows package signature.
    with urllib.request.urlopen(CAB_URL, timeout=30) as response, path.open("xb") as output:
        if not response.geturl().startswith("https://"):
            raise ValueError("Driver package redirect must remain HTTPS")
        total = 0
        while True:
            block = response.read(min(65536, MAX_CAB_SIZE + 1 - total))
            if not block:
                break
            total += len(block)
            if total > MAX_CAB_SIZE:
                raise ValueError("Driver package exceeds the 64 MiB download limit")
            output.write(block)


def limit_extractor_output():
    # Enforce the bound in the child before exec, not after decompression has
    # already consumed disk space. Respect a stricter inherited hard limit.
    _, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
    limit = MAX_DLL_SIZE if hard == resource.RLIM_INFINITY else min(MAX_DLL_SIZE, hard)
    resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def extract_cab(path, directory):
    # Snapshot the archive before extraction. Select only the named DLL into a
    # private directory; it is never loaded or executed.
    archive = directory / "source.cab"
    archive.write_bytes(read_regular(path, MAX_CAB_SIZE))
    if shutil.which("cabextract"):
        command = ["cabextract", "-q", "-d", str(directory), "-F", "ftWbioUmdfDriverV2.dll", str(archive)]
    elif shutil.which("7z"):
        command = ["7z", "e", "-y", "-o" + str(directory), str(archive), "ftWbioUmdfDriverV2.dll"]
    else:
        raise ValueError("CAB input requires cabextract or 7z")
    # Neither diagnostic stream is retained in memory or written to a log.
    # subprocess.run kills and reaps the child if the timeout expires.
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=EXTRACT_TIMEOUT, preexec_fn=limit_extractor_output)
    return read_regular(directory / "ftWbioUmdfDriverV2.dll", MAX_DLL_SIZE)


def collect_payloads(chip, input_path, pramboot, input_format, directory):
    firmwares = CATALOG[chip]
    kind = input_format
    if kind == "auto":
        kind = {".dll": "dll", ".cab": "cab"}.get(input_path.suffix.lower(), "binary")
    if pramboot and (chip != "ft9368" or kind != "binary"):
        raise ValueError("--pramboot requires --chip ft9368 and binary application input")
    if kind in ("cab", "dll"):
        data = extract_cab(input_path, directory) if kind == "cab" else read_regular(input_path, MAX_DLL_SIZE)
        payloads = [(firmware, extract_dll(data, firmware)) for firmware in firmwares]
    else:
        if chip == "ft9368" and not pramboot:
            raise ValueError("FT9368 requires both --input APP.bin and --pramboot PRAMBOOT.bin")
        paths = [input_path, Path(pramboot)] if pramboot else [input_path]
        payloads = [(firmware, read_regular(path, firmware.size)) for firmware, path in zip(firmwares, paths)]
    for firmware, payload in payloads:
        validate(payload, firmware)
    return payloads


def install_payloads(payloads, destination):
    """Validate the whole set, stage it, then atomically replace each file.

    The two FT9368 renames are not a multi-file transaction. Its driver checks
    both fixed hashes again before it can program hardware.
    """
    if not payloads:
        raise ValueError("No firmware selected")
    names = set()
    for firmware, payload in payloads:
        if Path(firmware.filename).name != firmware.filename or firmware.filename in names:
            raise ValueError("Invalid or duplicate firmware destination name")
        names.add(firmware.filename)
        validate(payload, firmware)
    destination.mkdir(mode=0o755, parents=True, exist_ok=True)
    directory_fd = os.open(destination, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    staged = []
    try:
        for firmware, payload in payloads:
            try:
                existing = os.stat(firmware.filename, dir_fd=directory_fd, follow_symlinks=False)
            except FileNotFoundError:
                existing = None
            if existing is not None and not stat.S_ISREG(existing.st_mode):
                raise ValueError(f"{firmware.filename}: destination is not a regular file")
            temporary = ".fte3600-" + secrets.token_hex(12)
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                         0o600, dir_fd=directory_fd)
            staged.append((temporary, firmware.filename))
            with os.fdopen(fd, "wb") as output:
                output.write(payload)
                output.flush()
                os.fchmod(output.fileno(), 0o644)
                os.fsync(output.fileno())
        for temporary, filename in staged:
            os.replace(temporary, filename, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
        os.fsync(directory_fd)
    finally:
        for temporary, _ in staged:
            try:
                os.unlink(temporary, dir_fd=directory_fd)
            except FileNotFoundError:
                pass
        os.close(directory_fd)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(prog="install-firmware.sh", description=(
        "Validate and install FTE3600 firmware files; this does not flash hardware. "
        "Python 3 is required. No chip is inferred from the computer model."))
    parser.add_argument("legacy_input", nargs="?", help="legacy FT9361 input file")
    parser.add_argument("--chip", type=str.lower, choices=sorted(set(CATALOG) | NO_FIRMWARE))
    parser.add_argument("--input", type=Path, help="local DLL, CAB or raw application binary")
    parser.add_argument("--pramboot", help="FT9368 raw PRAM boot image, paired with binary --input")
    parser.add_argument("--format", choices=("auto", "dll", "cab", "binary"), default="auto")
    parser.add_argument("--download", action="store_true", help="download the pinned Microsoft Catalog package")
    parser.add_argument("--destdir", type=Path, default=Path("/usr/lib/firmware"),
                        help="firmware root; files go under its fte3600/ subdirectory")
    parser.add_argument("--verify-only", action="store_true", help="validate without installing")
    parser.add_argument("--list", action="store_true", help="print the payload catalog and exit")
    args = parser.parse_args(argv)
    if args.list:
        return args
    if args.legacy_input:
        if args.chip or args.input or args.download:
            parser.error("legacy positional input cannot be combined with --chip, --input or --download")
        args.chip, args.input = "ft9361", Path(args.legacy_input)
    elif not argv:
        # Preserve the existing FT9361 entry point only. Failed inputs never
        # retry with another chip, another source or an implicit download.
        args.chip, args.download = "ft9361", True
    if not args.chip:
        parser.error("--chip is required (only legacy positional/no-argument calls select FT9361)")
    if args.chip in NO_FIRMWARE:
        parser.error(f"{args.chip.upper()} uses host configuration and has no installable firmware payload")
    if bool(args.input) == args.download:
        parser.error("choose exactly one of --input or --download")
    if args.download and (args.pramboot or args.format not in ("auto", "cab")):
        parser.error("--download supplies a CAB, including both FT9368 images")
    return args


def main(argv=None):
    args = parse_arguments(sys.argv[1:] if argv is None else argv)
    if args.list:
        for chip, firmwares in CATALOG.items():
            for firmware in firmwares:
                print(f"{chip} fte3600/{firmware.filename} {firmware.size} {firmware.sha256}")
        return 0
    with tempfile.TemporaryDirectory(prefix="fte3600-firmware-") as temporary:
        directory = Path(temporary)
        if args.download:
            args.input = directory / "download.cab"
            download_cab(args.input)
            args.format = "cab"
        payloads = collect_payloads(args.chip, args.input, args.pramboot, args.format, directory)
        for firmware, _ in payloads:
            print(f"Verified {firmware.filename}: {firmware.size} bytes, SHA-256 {firmware.sha256}")
        if args.verify_only:
            return 0
        destination = args.destdir / "fte3600"
        install_payloads(payloads, destination)
        for firmware, _ in payloads:
            print(f"Installed {destination / firmware.filename}")
        if args.chip == "ft9368":
            print("Files installed only; persistent FT9368 update still requires explicit driver opt-in and positive identity.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        if isinstance(error, PermissionError):
            print("Use --destdir for writable staging, or rerun the explicit command with sudo for system installation.", file=sys.stderr)
        sys.exit(1)
FTE3600_INSTALLER_PYTHON
