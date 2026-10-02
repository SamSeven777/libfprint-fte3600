#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""No hardware, no root, no downloaded/vendor code execution."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]

def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

prepare = load("prepare", "prepare.py")
runner = load("runner", "run-baseline.py")

def build(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result

def function(source, name):
    match = re.search(r"static ssize_t " + name + r"\([^)]*\)\s*\{", source)
    if not match: raise AssertionError(name)
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]

class FakeHost:
    def __init__(self, fail=(), interrupt=False):
        self.fail, self.interrupt, self.events = fail, interrupt, []

    def action(self, name):
        self.events.append(name)
        if name in self.fail:
            raise (KeyboardInterrupt if self.interrupt else RuntimeError)(name)

    def pause(self): self.action("pause")
    def power_on(self): self.action("power")
    def bind(self): self.action("bind")
    def open_library(self): self.action("open")
    def restore_binding(self): self.action("unbind")
    def restore_power(self): self.action("restore-power")
    def resume(self): self.action("resume")

class SessionTests(unittest.TestCase):
    def test_success(self):
        host = FakeHost()
        runner.Session(host).run()
        self.assertEqual(host.events, ["pause", "power", "bind", "open", "unbind", "restore-power", "resume"])

    def test_every_partial_failure_restores_in_reverse_order(self):
        expected = {
            "pause": ["pause", "resume"],
            "power": ["pause", "power", "restore-power", "resume"],
            "bind": ["pause", "power", "bind", "unbind", "restore-power", "resume"],
            "open": ["pause", "power", "bind", "open", "unbind", "restore-power", "resume"],
        }
        for step, events in expected.items():
            for interruption in (False, True):
                with self.subTest(step=step, interruption=interruption):
                    host = FakeHost((step,), interruption)
                    with self.assertRaises(KeyboardInterrupt if interruption else RuntimeError):
                        runner.Session(host).run()
                    self.assertEqual(host.events, events)

    def test_restoration_failure_keeps_authentication_paused(self):
        for step in ("unbind", "restore-power"):
            with self.subTest(step=step):
                host = FakeHost((step,))
                with self.assertRaisesRegex(RuntimeError, "RESTORE INCOMPLETE"):
                    runner.Session(host).run()
                self.assertIn("restore-power", host.events)
                self.assertNotIn("resume", host.events)

    def test_failed_resume_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, "RESTORE INCOMPLETE"):
            runner.Session(FakeHost(("resume",))).run()

    def test_cleanup_masks_signals_then_restores_handlers(self):
        handlers = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
        with runner.protect_cleanup():
            for s in handlers: self.assertEqual(signal.getsignal(s), signal.SIG_IGN)
        for s, handler in handlers.items(): self.assertEqual(signal.getsignal(s), handler)

class HostTests(unittest.TestCase):
    def test_existing_runtime_mask_is_not_removed(self):
        host = runner.Host(Path("/unused"), 10)
        host.service = {"LoadState": "masked", "ActiveState": "inactive", "UnitFileState": "masked-runtime"}
        host.mask = Path("/unused/runtime-mask")
        with patch.object(runner, "command") as command:
            command.return_value.stdout = "inactive\n"
            host.pause()
            host.resume()
        self.assertFalse(host.created_mask)
        self.assertNotIn("mask", [str(c) for c in command.call_args_list])
        self.assertFalse(any("unmask" in c.args[0] for c in command.call_args_list))

    def test_owned_mask_and_active_service_are_restored(self):
        with tempfile.TemporaryDirectory() as directory:
            host = runner.Host(Path(directory), 10)
            host.service = {"ActiveState": "active"}
            host.created_mask = True
            host.mask = Path(directory) / "fprintd.service"
            host.mask.symlink_to("/dev/null")
            with patch.object(runner, "command") as command:
                host.resume()
            self.assertEqual(command.call_args_list[0].args[0], ["systemctl", "unmask", "--runtime", "fprintd.service"])
            self.assertEqual(command.call_args_list[1].args[0], ["systemctl", "start", "fprintd.service"])

    def test_replaced_mask_is_not_removed_or_service_started(self):
        with tempfile.TemporaryDirectory() as directory:
            host = runner.Host(Path(directory), 10)
            host.service = {"ActiveState": "active"}
            host.created_mask = True
            host.mask = Path(directory) / "fprintd.service"
            host.mask.symlink_to("/nonexistent/other-owner")
            with patch.object(runner, "command") as command:
                with self.assertRaisesRegex(RuntimeError, "changed externally"): host.resume()
            command.assert_not_called()

    def test_power_rollback_attempts_all_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = runner.Host(root, 10)
            valid = root / "valid"
            valid.write_text("on\n")
            host.controls = [(valid, "auto"), (root / "missing/control", "auto")]
            with self.assertRaises(RuntimeError): host.restore_power()
            self.assertEqual(valid.read_text().strip(), "auto")

class BuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="medion-tests-")
        cls.out = Path(cls.temp.name)
        cls.flags = shlex.split(build(["pkg-config", "--cflags", "--libs", "gio-2.0"]).stdout)
        build(["cc", "-Wall", "-Wextra", "-Werror", str(ROOT / "tools/baseline-client.c"),
               "-o", str(cls.out / "client"), *cls.flags, "-ldl"])
        for extra, name in (([], "mock.so"), (["-DMISSING_CLOSE"], "missing.so")):
            build(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", *extra,
                   str(ROOT / "tests/mock-library.c"), "-o", str(cls.out / name), *cls.flags])

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def client(self, mode="ok", library="mock.so", action="--open"):
        env = dict(os.environ, MOCK_MODE=mode)
        result = subprocess.run([str(self.out / "client"), str(self.out / library), action],
                                env=env, capture_output=True, text=True, timeout=5)
        self.assertNotIn("PRIVATE_VENDOR", result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        return result

    def test_client_open_and_close(self):
        result = self.client()
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.splitlines(), ["MEDION_BASELINE:" + x for x in ("ABI_OK", "OPEN_START", "OPEN_OK", "CLOSE_OK")])

    def test_client_failure_stages(self):
        for mode, event in (("context", "CONTEXT_FAILED"), ("none", "NO_DEVICE"),
                            ("multiple", "MULTIPLE_DEVICES"), ("open", "OPEN_FAILED code=42"),
                            ("close", "CLOSE_FAILED code=43")):
            with self.subTest(mode=mode):
                result = self.client(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("MEDION_BASELINE:" + event, result.stdout)
                self.assertNotIn("CLOSE_OK", result.stdout)

    def test_abi_check_does_not_enumerate_or_open(self):
        result = self.client("context", action="--check-abi")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "MEDION_BASELINE:ABI_OK\n")

    def test_missing_library_and_symbol(self):
        for library, event in (("absent.so", "ABI_LOAD_FAILED"), ("missing.so", "ABI_MISSING_SYMBOL")):
            result = self.client(library=library)
            self.assertEqual(result.returncode, 1)
            self.assertIn(event, result.stdout)

    def test_pinned_core_actual_functions(self):
        raw = (ROOT / "reference/ctfdavis-focal_spi.c").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), prepare.REFERENCE_SHA)
        (self.out / "ctfdavis-core.inc").write_text(prepare.extract_core(raw.decode()))
        build(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined", "-I", str(self.out),
               str(ROOT / "tests/core-harness.c"), "-o", str(self.out / "core-test")])
        self.assertIn("PASS:", build([str(self.out / "core-test")]).stdout)

    def test_adapter_actual_transfer_functions(self):
        source = (ROOT / "module/focal_medion_baseline.c").read_text()
        frame = re.search(r"struct frame \{[^\n]+", source).group()
        (self.out / "transfer-functions.inc").write_text(frame + "\n" + function(source, "transfer_read") + "\n" + function(source, "transfer_write"))
        build(["cc", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=undefined",
               "-I", str(self.out), str(ROOT / "tests/transfer-harness.c"), "-o", str(self.out / "transfer-test")])
        self.assertIn("PASS:", build([str(self.out / "transfer-test")]).stdout)

    def test_bad_reference_function_refused(self):
        with self.assertRaises(ValueError): prepare.extract_core("static void focal_spi_power_off(void) {")

    def test_build_and_run_source_lists_agree(self):
        self.assertEqual(prepare.SOURCES, runner.SOURCES)

class SandboxTests(unittest.TestCase):
    def test_sensor_access_is_opt_in_and_narrow(self):
        for sensor in (False, True):
            cmd = runner.sandbox_command(Path("/run/private-stage"), "--open", sensor)
            self.assertIn("--unshare-all", cmd)
            self.assertIn("--clearenv", cmd)
            self.assertIn("--cap-drop", cmd)
            self.assertNotIn("--share-net", cmd)
            self.assertNotIn("/home", cmd)
            self.assertEqual(cmd.count("/dev/focal_moh_spi"), 2 if sensor else 0)
            self.assertEqual(cmd.count("--dev-bind"), 1 if sensor else 0)

    def worker(self, code, action="--open", timeout=2):
        with patch.object(runner, "sandbox_command", return_value=[sys.executable, "-c", code]):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                runner.isolated_run(Path("/unused"), action, timeout)
            return output.getvalue()

    def test_filters_untrusted_output(self):
        result = self.worker("print('SECRET'); print('MEDION_BASELINE:OPEN_OK'); print('MEDION_BASELINE:CLOSE_OK')")
        self.assertNotIn("SECRET", result)
        self.assertIn("CLOSE_OK", result)

    def test_exit_zero_without_success_events_fails(self):
        with self.assertRaisesRegex(RuntimeError, "did not complete"):
            self.worker("pass")

    def test_nonzero_exit_after_success_events_still_fails(self):
        with self.assertRaisesRegex(RuntimeError, "did not complete"):
            self.worker("print('MEDION_BASELINE:OPEN_OK'); print('MEDION_BASELINE:CLOSE_OK'); exit(1)")

    def test_timeout_kills_child(self):
        with tempfile.TemporaryDirectory() as directory:
            pid = Path(directory) / "pid"
            with self.assertRaises(TimeoutError):
                self.worker("import os,time; from pathlib import Path; Path(" + repr(str(pid)) + ").write_text(str(os.getpid())); time.sleep(30)", timeout=0.3)
            with self.assertRaises(ProcessLookupError): os.kill(int(pid.read_text()), 0)

    def test_excessive_output_is_stopped(self):
        with self.assertRaisesRegex(RuntimeError, "excessive"):
            self.worker("print('x' * 20000)")

class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / ".baseline/build"
        self.build.mkdir(parents=True)
        self.stage = self.root / "private-stage"
        self.stage.mkdir(mode=0o700)
        files = {}
        for name in ("baseline-client", "libfprint-legacy.so", runner.MODULE + ".ko"):
            data = ("MOCK " + name).encode()
            (self.build / name).write_bytes(data)
            files[name] = hashlib.sha256(data).hexdigest()
        sources = {}
        for name in runner.SOURCES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("MOCK SOURCE")
            sources[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.manifest = {"format": 1, "library_sha256": files["libfprint-legacy.so"],
                         "vermagic": os.uname().release + " SMP", "files": files, "sources": sources}
        for target, name, value in ((runner, "ROOT", self.root),
                                    (runner, "LIB_SHA", files["libfprint-legacy.so"]),
                                    (runner.tempfile, "mkdtemp", lambda **kwargs: str(self.stage))):
            patcher = patch.object(target, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def stage_files(self, actual_vermagic=None):
        (self.build / "manifest.json").write_text(json.dumps(self.manifest))
        with patch.object(runner, "command") as command:
            command.return_value.stdout = actual_vermagic or self.manifest["vermagic"]
            return runner.stage_artifacts()

    def test_valid_artifacts_are_copied_and_private(self):
        self.assertEqual(self.stage_files(), self.stage)
        for name in self.manifest["files"]:
            self.assertEqual((self.stage / name).read_bytes(), (self.build / name).read_bytes())
            self.assertEqual((self.stage / name).stat().st_mode & 0o077, 0)

    def test_changed_sources_refused(self):
        (self.root / runner.SOURCES[0]).write_text("CHANGED")
        with self.assertRaisesRegex(RuntimeError, "Sources changed"): self.stage_files()
        self.assertEqual(list(self.stage.iterdir()), [])

    def test_changed_artifacts_refused(self):
        (self.build / "baseline-client").write_text("CHANGED")
        with self.assertRaisesRegex(RuntimeError, "hash mismatch"): self.stage_files()

    def test_wrong_kernel_refused(self):
        self.manifest["vermagic"] = "wrong-kernel SMP"
        with self.assertRaisesRegex(RuntimeError, "running kernel"): self.stage_files()

    def test_actual_module_metadata_must_agree(self):
        with self.assertRaisesRegex(RuntimeError, "metadata mismatch"):
            self.stage_files(actual_vermagic="unexpected-metadata")

    def test_symlink_artifacts_refused(self):
        (self.build / "baseline-client").unlink()
        (self.build / "baseline-client").symlink_to(self.build / "libfprint-legacy.so")
        with self.assertRaisesRegex(RuntimeError, "Invalid build artifact"): self.stage_files()

    def test_unrecognized_package_is_not_extracted(self):
        fake = self.root / "fake.deb"
        fake.write_bytes(b"not the pinned package")
        with patch.object(prepare, "checked") as command:
            with self.assertRaisesRegex(RuntimeError, "size/SHA256 mismatch"):
                prepare.prepare_library(self.build, fake)
        command.assert_not_called()

if __name__ == "__main__":
    unittest.main(verbosity=2)
