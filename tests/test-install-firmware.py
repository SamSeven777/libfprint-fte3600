#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Local mathematical payloads only; no network, vendor files or system writes."""
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import replace
import hashlib
import io
import json
import os
from pathlib import Path
import re
import resource
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/install-firmware.sh"
source = SCRIPT.read_text().split("<<'FTE3600_INSTALLER_PYTHON'\n", 1)[1].rsplit("\nFTE3600_INSTALLER_PYTHON", 1)[0]
installer = types.ModuleType("fte3600_installer_under_test")
sys.modules[installer.__name__] = installer
exec(compile(source, str(SCRIPT), "exec"), installer.__dict__)


class FirmwareInstallerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="fte3600-installer-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def synthetic(self, firmware, seed=17):
        payload = bytes((index * 29 + seed) & 255 for index in range(firmware.size))
        return replace(firmware, sha256=hashlib.sha256(payload).hexdigest()), payload

    def test_catalog_matches_runtime_and_audited_offsets(self):
        runtime = (ROOT / "libfprint/drivers/fte3600-sensor.c").read_text()
        expected = {name: (int(size), digest) for name, size, digest in re.findall(
            r'"fte3600/([^"\n]+)",\s*(\d+),\s*"([0-9a-f]{64})"', runtime)}
        installed = {f.filename: (f.size, f.sha256) for group in installer.CATALOG.values() for f in group}
        self.assertEqual(installed, expected)
        self.assertEqual(len(installed), 6)
        inventory = json.loads((ROOT / "docs/fte3600/windows-hardware-inventory.json").read_text())
        firmware_by_name = {f.filename: f for group in installer.CATALOG.values() for f in group}
        for version in inventory["versions"]:
            for region in version["firmware_exact_matches"]:
                firmware = firmware_by_name[region["label"].lower() + ".bin"]
                self.assertEqual(firmware.size, region["size"])
                self.assertIn(region["file_offset"], firmware.offsets)

    def test_cli_selection_and_legacy_compatibility(self):
        legacy = installer.parse_arguments([])
        self.assertEqual(legacy.chip, "ft9361")
        self.assertTrue(legacy.download)
        legacy = installer.parse_arguments(["local.bin", "--verify-only"])
        self.assertEqual(legacy.chip, "ft9361")
        self.assertEqual(legacy.input, Path("local.bin"))
        explicit = installer.parse_arguments(["--chip", "FT9368", "--input", "app.bin", "--pramboot", "pram.bin"])
        self.assertEqual(explicit.chip, "ft9368")
        for argv in (
            ["--input", "local.bin"], ["--chip", "unknown", "--input", "local.bin"],
            ["--chip", "ft9365", "--input", "local.bin"],
            ["--chip", "ft9369", "--download"], ["--chip", "ft9769", "--download"],
            ["--chip", "ft9361"], ["--chip", "ft9361", "--input", "local.bin", "--download"],
            ["--chip", "ft9368", "--download", "--pramboot", "pram.bin"],
            ["--chip", "ft9338", "legacy.bin"],
        ):
            with self.subTest(argv=argv), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                installer.parse_arguments(argv)

    def test_every_raw_payload_and_pair_install(self):
        for chip, group in list(installer.CATALOG.items()):
            with self.subTest(chip=chip):
                generated = [self.synthetic(firmware, i + 31) for i, firmware in enumerate(group)]
                inputs = []
                for firmware, payload in generated:
                    path = self.root / (chip + "-" + firmware.filename)
                    path.write_bytes(payload)
                    inputs.append(path)
                destination = self.root / chip
                argv = ["--chip", chip, "--input", str(inputs[0]), "--destdir", str(destination)]
                if len(inputs) == 2:
                    argv += ["--pramboot", str(inputs[1])]
                with mock.patch.dict(installer.CATALOG, {chip: tuple(f for f, _ in generated)}), redirect_stdout(io.StringIO()):
                    self.assertEqual(installer.main(argv), 0)
                for firmware, payload in generated:
                    path = destination / "fte3600" / firmware.filename
                    self.assertEqual(path.read_bytes(), payload)
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o644)
                self.assertEqual(sorted(p.name for p in (destination / "fte3600").iterdir()),
                                 sorted(f.filename for f, _ in generated))

    def test_size_hash_pair_fail_before_creating_destination(self):
        app, app_data = self.synthetic(installer.CATALOG["ft9368"][0])
        pram, pram_data = self.synthetic(installer.CATALOG["ft9368"][1], 23)
        for bad in (pram_data[:-1], pram_data + b"x", bytes([pram_data[0] ^ 1]) + pram_data[1:]):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                installer.install_payloads([(app, app_data), (pram, bad)], self.root / "absent")
            self.assertFalse((self.root / "absent").exists())
        with self.assertRaises(ValueError):
            installer.collect_payloads("ft9368", self.root / "absent.bin", None, "binary", self.root)

    def test_dll_offsets_and_format_override(self):
        for chip, group in list(installer.CATALOG.items()):
            generated = [self.synthetic(firmware, i + 44) for i, firmware in enumerate(group)]
            for index in range(min(len(f.offsets) for f, _ in generated)):
                data = bytearray(max(f.offsets[index] + f.size for f, _ in generated))
                for firmware, payload in generated:
                    offset = firmware.offsets[index]
                    data[offset:offset + len(payload)] = payload
                path = self.root / "driver with spaces.DLL"
                path.write_bytes(data)
                with self.subTest(chip=chip, index=index), mock.patch.dict(installer.CATALOG, {chip: tuple(f for f, _ in generated)}):
                    self.assertEqual(installer.collect_payloads(chip, path, None, "auto", self.root), generated)
                    self.assertEqual(installer.collect_payloads(chip, path, None, "dll", self.root), generated)
        firmware, payload = self.synthetic(installer.CATALOG["ft9361"][0])
        with self.assertRaises(ValueError):
            installer.extract_dll(b"prefix" + payload, firmware)
        with self.assertRaises(ValueError):
            installer.extract_dll(bytes(firmware.offsets[0] + firmware.size), firmware)

    def test_regular_files_bounds_and_input_types(self):
        path = self.root / "input.bin"
        path.write_bytes(b"fixture")
        self.assertEqual(installer.read_regular(path, 7), b"fixture")
        with self.assertRaises(ValueError):
            installer.read_regular(path, 6)
        with self.assertRaises(ValueError):
            installer.read_regular(self.root, 1024)
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            installer.read_regular(fifo, 1024)
        link = self.root / "link"
        link.symlink_to(path)
        with self.assertRaises(OSError):
            installer.read_regular(link, 1024)
        with self.assertRaises(FileNotFoundError):
            installer.read_regular(self.root / "missing", 1024)

    def test_input_growth_and_shrink(self):
        path = self.root / "racing.bin"
        original_read = os.read
        for replacement in (b"abcdefghijklmnop", b"abc"):
            path.write_bytes(b"abcdefgh")
            changed = False

            def racing_read(fd, size):
                nonlocal changed
                if not changed:
                    path.write_bytes(replacement)
                    changed = True
                return original_read(fd, size)

            with mock.patch.object(installer.os, "read", side_effect=racing_read), self.assertRaises(ValueError):
                installer.read_regular(path, 32)

    def test_atomic_staging_and_cleanup(self):
        generated = [self.synthetic(f, i + 1) for i, f in enumerate(installer.CATALOG["ft9368"])]
        destination = self.root / "firmware"
        destination.mkdir()
        for firmware, _ in generated:
            (destination / firmware.filename).write_bytes(b"old")
        original_replace = os.replace
        seen = []

        def checked_replace(source, target, **kwargs):
            # All bytes are staged before the first visible replacement.
            staged = list(destination.glob(".fte3600-*"))
            self.assertEqual(len(staged), len(generated) - len(seen))
            self.assertEqual((destination / target).read_bytes(), b"old")
            self.assertIn((destination / source).read_bytes(), [data for _, data in generated])
            seen.append(target)
            return original_replace(source, target, **kwargs)

        with mock.patch.object(installer.os, "replace", side_effect=checked_replace):
            installer.install_payloads(generated, destination)
        self.assertEqual(len(seen), 2)
        self.assertFalse(list(destination.glob(".fte3600-*")))
        for firmware, payload in generated:
            self.assertEqual((destination / firmware.filename).read_bytes(), payload)
        with mock.patch.object(installer.os, "replace", side_effect=OSError("synthetic rename failure")), self.assertRaises(OSError):
            installer.install_payloads(generated, destination)
        self.assertFalse(list(destination.glob(".fte3600-*")))
        for firmware, payload in generated:
            self.assertEqual((destination / firmware.filename).read_bytes(), payload)

    def test_destination_aliases_are_not_followed(self):
        firmware, payload = self.synthetic(installer.CATALOG["ft9361"][0])
        target = self.root / "unrelated"
        target.write_bytes(b"keep")
        destination = self.root / "firmware"
        destination.mkdir()
        (destination / firmware.filename).symlink_to(target)
        with self.assertRaises(ValueError):
            installer.install_payloads([(firmware, payload)], destination)
        self.assertEqual(target.read_bytes(), b"keep")
        self.assertFalse(list(destination.glob(".fte3600-*")))
        directory_link = self.root / "directory-link"
        directory_link.symlink_to(destination, target_is_directory=True)
        with self.assertRaises(OSError):
            installer.install_payloads([(firmware, payload)], directory_link)

    def test_pair_rename_failure_leaves_only_complete_files(self):
        generated = [self.synthetic(f, i + 1) for i, f in enumerate(installer.CATALOG["ft9368"])]
        destination = self.root / "firmware"
        destination.mkdir()
        for firmware, _ in generated:
            (destination / firmware.filename).write_bytes(b"old")
        original_replace = os.replace
        calls = 0

        def fail_second(source, target, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("synthetic second-file failure")
            return original_replace(source, target, **kwargs)

        with mock.patch.object(installer.os, "replace", side_effect=fail_second), self.assertRaises(OSError):
            installer.install_payloads(generated, destination)
        self.assertEqual((destination / generated[0][0].filename).read_bytes(), generated[0][1])
        self.assertEqual((destination / generated[1][0].filename).read_bytes(), b"old")
        self.assertFalse(list(destination.glob(".fte3600-*")))
        with self.assertRaises(ValueError):
            installer.validate((destination / generated[1][0].filename).read_bytes(), generated[1][0])

    def test_staging_failure_does_not_replace_old_file(self):
        firmware, payload = self.synthetic(installer.CATALOG["ft9361"][0])
        destination = self.root / "firmware"
        destination.mkdir()
        (destination / firmware.filename).write_bytes(b"old")
        with mock.patch.object(installer.os, "fsync", side_effect=OSError("synthetic disk failure")), self.assertRaises(OSError):
            installer.install_payloads([(firmware, payload)], destination)
        self.assertEqual((destination / firmware.filename).read_bytes(), b"old")
        self.assertFalse(list(destination.glob(".fte3600-*")))

    def test_verify_only_missing_input_and_no_network_fallback(self):
        firmware, payload = self.synthetic(installer.CATALOG["ft9361"][0])
        path = self.root / "input.bin"
        path.write_bytes(payload)
        destination = self.root / "not-created"
        with mock.patch.dict(installer.CATALOG, {"ft9361": (firmware,)}), \
             mock.patch.object(installer, "download_cab", side_effect=AssertionError("unexpected network")), \
             redirect_stdout(io.StringIO()):
            self.assertEqual(installer.main([str(path), "--verify-only", "--destdir", str(destination)]), 0)
            self.assertFalse(destination.exists())
            with self.assertRaises(FileNotFoundError):
                installer.main([str(self.root / "missing")])
            path.write_bytes(bytes(firmware.size))
            with self.assertRaises(ValueError):
                installer.main([str(path)])

    def test_cab_selects_only_named_dll(self):
        archive = self.root / "input.cab"
        archive.write_bytes(b"synthetic archive")
        output = self.root / "extract"
        output.mkdir()
        commands = []

        def extract(command, **kwargs):
            commands.append(command)
            self.assertEqual(kwargs["timeout"], 60)
            self.assertEqual(kwargs["stdout"], subprocess.DEVNULL)
            self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
            self.assertIs(kwargs["preexec_fn"], installer.limit_extractor_output)
            (output / "ftWbioUmdfDriverV2.dll").write_bytes(b"synthetic dll")

        with mock.patch.object(installer.shutil, "which", return_value="cabextract"), \
             mock.patch.object(installer.subprocess, "run", side_effect=extract):
            self.assertEqual(installer.extract_cab(archive, output), b"synthetic dll")
        self.assertIn("-F", commands[0])
        self.assertIn("ftWbioUmdfDriverV2.dll", commands[0])
        self.assertEqual((output / "source.cab").read_bytes(), b"synthetic archive")
        with mock.patch.object(installer.shutil, "which", return_value=None), self.assertRaises(ValueError):
            installer.extract_cab(archive, output)

    def extractor_fixture(self, name, body):
        executable = self.root / name
        executable.write_text(f"#!{sys.executable}\nimport os, sys, time\nfrom pathlib import Path\n" + body)
        executable.chmod(0o755)
        return executable

    def test_archive_expansion_is_bounded_before_writing(self):
        archive = self.root / "input.cab"
        archive.write_bytes(b"synthetic compressed input")
        inherited_limit = resource.getrlimit(resource.RLIMIT_FSIZE)
        for name in ("cabextract", "7z"):
            output = self.root / (name + "-output")
            output.mkdir()
            executable = self.extractor_fixture(name,
                "directory = Path(sys.argv[sys.argv.index('-d') + 1]) if '-d' in sys.argv "
                "else Path(next(arg[2:] for arg in sys.argv if arg.startswith('-o')))\n"
                "os.write(1, b'o' * (2 * 1024 * 1024))\n"
                "os.write(2, b'e' * (2 * 1024 * 1024))\n"
                "with (directory / 'ftWbioUmdfDriverV2.dll').open('wb') as output:\n"
                "    output.write(b'x' * 1025)\n")
            with self.subTest(extractor=name), \
                 mock.patch.dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ["PATH"]), \
                 mock.patch.object(installer.shutil, "which", side_effect=lambda candidate: str(executable) if candidate == name else None), \
                 mock.patch.object(installer, "MAX_DLL_SIZE", 1024), \
                 self.assertRaises(subprocess.CalledProcessError):
                installer.extract_cab(archive, output)
            self.assertLessEqual((output / "ftWbioUmdfDriverV2.dll").stat().st_size, 1024)
            self.assertEqual(resource.getrlimit(resource.RLIMIT_FSIZE), inherited_limit)
            self.assertEqual(sorted(path.name for path in output.iterdir()),
                             ["ftWbioUmdfDriverV2.dll", "source.cab"])

    def test_archive_timeout_kills_and_reaps_extractor(self):
        archive = self.root / "input.cab"
        archive.write_bytes(b"synthetic compressed input")
        output = self.root / "output"
        output.mkdir()
        executable = self.extractor_fixture("cabextract",
            "directory = Path(sys.argv[sys.argv.index('-d') + 1])\n"
            "(directory / 'child.pid').write_text(str(os.getpid()))\n"
            "while True:\n"
            "    os.write(2, b'e' * 65536)\n"
            "    time.sleep(0.01)\n")
        with mock.patch.dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ["PATH"]), \
             mock.patch.object(installer.shutil, "which", return_value=str(executable)), \
             mock.patch.object(installer, "EXTRACT_TIMEOUT", 2.0), \
             self.assertRaises(subprocess.TimeoutExpired):
            installer.extract_cab(archive, output)
        pid = int((output / "child.pid").read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        with self.assertRaises(ChildProcessError):
            os.waitpid(pid, os.WNOHANG)

    def test_shell_entrypoint(self):
        result = subprocess.run(["/bin/sh", str(SCRIPT), "--list"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 6)
        result = subprocess.run(["/bin/sh", str(SCRIPT), "--chip", "unknown"], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        result = subprocess.run(["/bin/sh", str(SCRIPT), "--list"], capture_output=True, text=True, timeout=10,
                                env=dict(os.environ, PATH=str(self.root)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Python 3", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
