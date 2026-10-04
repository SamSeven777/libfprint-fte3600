#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic sysfs and command tests; never bind hardware or manage services."""

from contextlib import nullcontext
import importlib.util
import io
import itertools
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


if sys.platform.startswith("linux"):
    SCRIPT = Path(__file__).resolve().parents[1] / "scripts/medion-spidev.py"
    SPEC = importlib.util.spec_from_file_location("medion_spidev", SCRIPT)
    MEDION = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = MEDION
    SPEC.loader.exec_module(MEDION)
else:
    MEDION = None


class FakeMachine:
    def __init__(self, root):
        self.root = root
        self.sys = root / "sys"
        self.dev = root / "dev"
        self.proc = root / "proc"
        self.dev.mkdir()
        self.proc.mkdir()
        self.log = []
        self.fail = set()
        self.enabled = "enabled"
        self.original_enabled = "enabled"
        self.active = True
        self.load_state = "loaded"
        self.false_mask = False
        self.tool_error = None
        self.delayed_node_attempts = 0
        self.tool = root / "fte3600-medion"
        self.tool.write_text("synthetic diagnostic placeholder\n")
        self.tool.chmod(0o755)
        self.acpi_sensor = self.acpi("FTE3600:00", "FTE3600", MEDION.SPI_ACPI)
        self.acpi_reset = self.acpi("INT3453:17", "INT3453", MEDION.RESET_ACPI)
        self.acpi_irq = self.acpi("INT3453:42", "INT3453", MEDION.IRQ_ACPI)
        self.controller = self.sys / "devices/platform/controller/spi9"
        self.controller.mkdir(parents=True)
        self.link(self.sys / "class/spi_master/spi9", self.controller)
        self.spi = self.spi_device("spi-FTE3600:00", self.controller, self.acpi_sensor)
        (self.spi / "driver_override").write_text("(null)\n")
        self.reset = self.gpio("gpiochip7", self.acpi_reset, "254:7", 64)
        self.irq = self.gpio("gpiochip12", self.acpi_irq, "254:12", 32)
        self.driver = self.sys / "bus/spi/drivers/spidev"
        self.driver.mkdir(parents=True)
        (self.driver / "bind").touch()
        (self.driver / "unbind").touch()

    @staticmethod
    def link(path, target):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)

    def acpi(self, name, hid, namespace):
        path = self.sys / "bus/acpi/devices" / name
        path.mkdir(parents=True)
        (path / "hid").write_text(hid + "\n")
        (path / "path").write_text(namespace + "\n")
        return path

    def spi_device(self, name, controller, acpi=None):
        path = controller / name
        path.mkdir(parents=True)
        self.link(self.sys / "bus/spi/devices" / name, path)
        if acpi:
            self.link(path / "firmware_node", acpi)
        return path

    def gpio(self, name, acpi, number, count):
        parent = self.sys / "devices/platform" / acpi.name
        self.link(parent / "firmware_node", acpi)
        path = parent / name
        path.mkdir()
        (path / "dev").write_text(number + "\n")
        (path / "ngpio").write_text(str(count) + "\n")
        self.link(self.sys / "bus/gpio/devices" / name, path)
        (self.dev / name).write_text("fake node; fstat is tested separately\n")
        return path

    def loaded(self, bufsiz=32768):
        parameters = self.sys / "module/spidev/parameters"
        parameters.mkdir(parents=True, exist_ok=True)
        (parameters / "bufsiz").write_text(str(bufsiz) + "\n")

    def bind(self, driver="spidev"):
        driver_path = self.sys / "bus/spi/drivers" / driver
        driver_path.mkdir(parents=True, exist_ok=True)
        self.link(self.spi / "driver", driver_path)
        if driver == "spidev":
            entry = self.sys / "class/spidev/spidev9.0"
            self.link(entry / "device", self.spi)
            (entry / "dev").write_text("153:0\n")
            (self.dev / "spidev9.0").write_text("fake spidev\n")

    def write(self, path, value):
        self.log.append(("write", str(path), value))
        if path == self.spi / "driver_override":
            if "override-restore" in self.fail and value != "spidev":
                raise MEDION.DiagnosticError("injected override restoration failure")
            path.write_text(value + "\n")
        elif path == self.driver / "bind":
            if "bind" in self.fail:
                raise MEDION.DiagnosticError("injected bind failure")
            self.bind()
        elif path == self.driver / "unbind":
            if "unbind" in self.fail:
                raise MEDION.DiagnosticError("injected unbind failure")
            (self.spi / "driver").unlink()
            shutil.rmtree(self.sys / "class/spidev/spidev9.0")
            (self.dev / "spidev9.0").unlink()
        else:
            raise AssertionError(f"Unexpected sysfs write: {path}")

    def verify(self, node, writable=False):
        self.log.append(("verify", str(node.path), writable))
        if writable and self.delayed_node_attempts:
            self.delayed_node_attempts -= 1
            raise MEDION.DiagnosticError("node not published yet") from FileNotFoundError(2, "not found")
        if "node" in self.fail:
            raise MEDION.DiagnosticError("injected device identity mismatch")

    def command(self, arguments, check=True, inherit=False):
        args = tuple(map(str, arguments))
        self.log.append(("command", *args))
        output, status = "", 0
        operation = args[1] if len(args) > 1 else ""
        if args[0] == str(self.tool):
            if self.tool_error:
                raise self.tool_error
            if "--output" in args:
                Path(args[args.index("--output") + 1]).write_bytes(b"synthetic capture placeholder")
        elif args[0] == "modprobe":
            if "modprobe" in self.fail:
                status = 1
            else:
                self.loaded()
        elif args[0] == "systemctl":
            if operation in self.fail:
                status = 1
            elif operation == "show":
                output = self.load_state + "\n"
            elif operation == "is-enabled":
                output = self.enabled + "\n"
                status = 1 if self.enabled in ("disabled", "masked", "masked-runtime") else 0
            elif operation == "is-active":
                status = 0 if self.active else 3
            elif operation == "mask":
                if not self.false_mask:
                    self.original_enabled = self.enabled
                    self.enabled = "masked-runtime"
            elif operation == "unmask":
                self.enabled = self.original_enabled
            elif operation == "stop":
                self.active = False
            elif operation == "start":
                self.active = True
            else:
                raise AssertionError(f"Unexpected service operation: {args}")
        else:
            raise AssertionError(f"Unexpected external command: {args}")
        if check and status:
            raise MEDION.DiagnosticError(f"injected command failure: {args}")
        return subprocess.CompletedProcess(args, status, output, "")


@unittest.skipUnless(MEDION is not None, "Linux-specific sysfs and GPIO diagnostic")
class MedionTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="fte3600-medion-test-")
        self.addCleanup(directory.cleanup)
        self.machine = FakeMachine(Path(directory.name))
        for name, value in (("SYS", self.machine.sys), ("DEV", self.machine.dev),
                            ("PROC", self.machine.proc)):
            self.enterContext(patch.object(MEDION, name, value))
        self.enterContext(patch.object(MEDION, "command", self.machine.command))
        self.enterContext(patch.object(MEDION, "write_sysfs", self.machine.write))
        self.enterContext(patch.object(MEDION, "verify_node", self.machine.verify))
        self.enterContext(patch.object(MEDION, "exclusive_lock", nullcontext))
        self.enterContext(patch.object(MEDION.os, "geteuid", return_value=0))
        self.output = io.StringIO()
        self.errors = io.StringIO()
        self.enterContext(patch.object(sys, "stdout", self.output))
        self.enterContext(patch.object(sys, "stderr", self.errors))

    def run_action(self, *args):
        return MEDION.main(["--tool", str(self.machine.tool), *args])

    def assert_restored(self, override="", binding=None, active=True, enabled="enabled"):
        m = self.machine
        self.assertEqual(MEDION.binding(m.spi), binding)
        self.assertIn(MEDION.attribute(m.spi / "driver_override"), (override, "(null)" if not override else override))
        self.assertEqual(m.active, active)
        self.assertEqual(m.enabled, enabled)

    def assert_no_binding_writes(self):
        self.assertFalse(any(item[0] == "write" for item in self.machine.log), self.machine.log)

    def test_inspect_without_spidev_is_read_only_and_unprivileged(self):
        with patch.object(MEDION.os, "geteuid", return_value=1000):
            self.assertEqual(self.run_action("--inspect"), 0, self.errors.getvalue())
        self.assertEqual(self.machine.log, [])
        self.assertIn("gpiochip7", self.output.getvalue())
        self.assertIn("gpiochip12", self.output.getvalue())
        self.assertIn("not bound", self.output.getvalue())

    def test_exact_fte3600_acpi_path_is_required(self):
        (self.machine.acpi_sensor / "path").write_text(r"\_SB_.PCI0.SPI2.FP05")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertEqual(self.machine.log, [])

    def test_exact_gpio_controller_hid_is_required(self):
        (self.machine.acpi_reset / "hid").write_text("INT3452\n")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertEqual(self.machine.log, [])

    def test_duplicate_acpi_device_is_ambiguous(self):
        self.machine.acpi("FTE3600:99", "FTE3600", MEDION.SPI_ACPI)
        self.assertNotEqual(self.run_action("--inspect"), 0)

    def test_duplicate_gpio_sysfs_alias_is_not_another_chip(self):
        m = self.machine
        m.link(m.sys / "class/gpio/gpiochip7", m.reset)
        resources = MEDION.discover()
        self.assertEqual(resources.reset.path, m.dev / "gpiochip7")
        self.assertEqual(resources.irq.path, m.dev / "gpiochip12")

    def test_gpio_line_count_must_include_reset_line(self):
        (self.machine.reset / "ngpio").write_text("39\n")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertEqual(self.machine.log, [])

    def test_any_spi_sibling_blocks_execution_regardless_of_name(self):
        m = self.machine
        m.spi_device("unusually-named-device", m.controller)
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertEqual(m.log, [])

    def test_spi_device_on_another_controller_does_not_block(self):
        m = self.machine
        other = m.sys / "devices/platform/other/spi88"
        m.spi_device("spi88.0", other)
        self.assertEqual(MEDION.discover().spi, m.spi)

    def test_spidev_must_belong_to_selected_spi_device(self):
        m = self.machine
        m.bind()
        entry = m.sys / "class/spidev/spidev9.0/device"
        entry.unlink()
        entry.symlink_to(m.controller)
        with self.assertRaises(MEDION.DiagnosticError):
            MEDION.spidev_node(m.spi)

    def test_probe_temporarily_binds_and_restores_override_and_service(self):
        m = self.machine
        (m.spi / "driver_override").write_text("previous-driver\n")
        self.assertEqual(self.run_action("--probe"), 0, self.errors.getvalue())
        self.assert_restored(override="previous-driver")
        calls = [item for item in m.log if item[0] == "command"]
        self.assertLess(calls.index(("command", "systemctl", "mask", "--runtime", MEDION.SERVICE)),
                        calls.index(("command", "systemctl", "stop", MEDION.SERVICE)))
        self.assertIn(("command", "modprobe", "spidev", "bufsiz=32768"), calls)
        self.assertIn(("command", str(m.tool), "--device", str(m.dev / "spidev9.0"),
                       "--reset-chip", str(m.dev / "gpiochip7"), "--irq-chip", str(m.dev / "gpiochip12"),
                       "--action", "probe"), calls)

    def test_existing_spidev_and_permanent_service_mask_are_preserved(self):
        m = self.machine
        m.loaded()
        m.bind()
        m.enabled = "masked"
        m.active = False
        self.assertEqual(self.run_action("--init"), 0, self.errors.getvalue())
        self.assert_restored(binding="spidev", active=False, enabled="masked")
        self.assert_no_binding_writes()
        self.assertFalse(any(item[:3] in (("command", "systemctl", "mask"),
                                         ("command", "systemctl", "unmask"),
                                         ("command", "systemctl", "start")) for item in m.log))

    def test_existing_runtime_mask_is_not_removed(self):
        m = self.machine
        m.enabled = "masked-runtime"
        m.active = False
        self.assertEqual(self.run_action("--probe"), 0, self.errors.getvalue())
        self.assert_restored(active=False, enabled="masked-runtime")

    def test_another_bound_driver_is_never_unbound(self):
        self.machine.bind("fte3600")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertEqual(self.machine.log, [])
        self.assertEqual(MEDION.binding(self.machine.spi), "fte3600")

    def test_modprobe_failure_restores_service(self):
        self.machine.fail.add("modprobe")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assert_no_binding_writes()

    def test_loaded_spidev_with_4k_buffer_can_probe_without_global_reload(self):
        self.machine.loaded(4096)
        self.assertEqual(self.run_action("--probe"), 0, self.errors.getvalue())
        self.assert_restored()
        self.assertIn("Actual spidev bufsiz: 4096", self.output.getvalue())
        self.assertFalse(any(item[:2] in (("command", "modprobe"), ("command", "rmmod"))
                             for item in self.machine.log))

    def test_loaded_spidev_buffer_too_small_for_probe_is_rejected(self):
        self.machine.loaded(32)
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assert_no_binding_writes()

    def test_kernel_rejecting_override_restores_previous_override(self):
        self.machine.fail.add("bind")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assertIn("kernel may reject", self.errors.getvalue())

    def test_character_device_mismatch_prevents_c_tool_and_restores(self):
        self.machine.fail.add("node")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assertFalse(any(item[:2] == ("command", str(self.machine.tool)) for item in self.machine.log))

    def test_new_binding_waits_for_only_the_target_character_node(self):
        m = self.machine
        m.delayed_node_attempts = 2
        with patch.object(MEDION.time, "sleep") as sleep:
            self.assertEqual(self.run_action("--probe"), 0, self.errors.getvalue())
        self.assertEqual(sleep.call_count, 2)
        self.assert_restored()
        self.assertTrue(any(item[:2] == ("command", str(m.tool)) for item in m.log))

    def test_missing_character_node_has_bounded_wait_and_restores(self):
        m = self.machine
        m.delayed_node_attempts = 100
        with patch.object(MEDION.time, "monotonic", side_effect=itertools.count(0, 3)):
            self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertIn("Timed out waiting", self.errors.getvalue())
        self.assert_restored()
        self.assertFalse(any(item[:2] == ("command", str(m.tool)) for item in m.log))

    def test_tool_failure_restores_binding_and_service(self):
        self.machine.tool_error = MEDION.DiagnosticError("synthetic tool failure")
        self.assertNotEqual(self.run_action("--init"), 0)
        self.assert_restored()
        self.assertNotIn("[done]", self.output.getvalue())

    def test_signal_interruption_restores_binding_and_service(self):
        self.machine.tool_error = MEDION.Interrupted(signal.SIGTERM)
        self.assertEqual(self.run_action("--probe"), 128 + signal.SIGTERM)
        self.assert_restored()
        self.assertNotIn("[done]", self.output.getvalue())

    def test_failed_unbind_does_not_skip_override_or_service_restoration(self):
        self.machine.fail.add("unbind")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored(binding="spidev")
        self.assertIn("Restoration failed", self.errors.getvalue())
        self.assertNotIn("[done]", self.output.getvalue())

    def test_failed_override_restore_still_restores_service(self):
        self.machine.fail.add("override-restore")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored(override="spidev")
        self.assertIn("Restoration failed", self.errors.getvalue())

    def test_stop_failure_prevents_spi_binding(self):
        self.machine.fail.add("stop")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assert_no_binding_writes()

    def test_mask_must_be_verified_before_hardware_changes(self):
        self.machine.false_mask = True
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assert_no_binding_writes()

    def test_manually_running_fprintd_prevents_binding(self):
        process = self.machine.proc / "12345"
        process.mkdir()
        (process / "comm").write_text("fprintd\n")
        self.assertNotEqual(self.run_action("--probe"), 0)
        self.assert_restored()
        self.assert_no_binding_writes()

    def test_capture_output_is_explicit_and_existing_files_are_preserved(self):
        target = self.machine.root / "capture.pgm"
        target.write_bytes(b"existing data")
        self.assertNotEqual(self.run_action("--capture", str(target)), 0)
        self.assertEqual(target.read_bytes(), b"existing data")
        self.assertEqual(self.machine.log, [])

    def test_capture_passes_only_requested_output_to_tool(self):
        target = self.machine.root / "capture.pgm"
        self.assertEqual(self.run_action("--capture", str(target)), 0, self.errors.getvalue())
        self.assertEqual(target.read_bytes(), b"synthetic capture placeholder")
        self.assert_restored()

    def test_execution_requires_root_before_service_changes(self):
        with patch.object(MEDION.os, "geteuid", return_value=1000):
            self.assertNotEqual(self.run_action("--probe"), 0)
        self.assertEqual(self.machine.log, [])


@unittest.skipUnless(MEDION is not None, "Linux-specific character-device validation")
class SystemBoundaryTests(unittest.TestCase):
    def test_advisory_lock_rejects_concurrent_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="fte3600-lock-test-") as directory:
            path = Path(directory) / "lock"
            real_fstat = os.fstat

            def root_owned_info(fd):
                info = real_fstat(fd)
                return SimpleNamespace(st_mode=info.st_mode, st_uid=0, st_nlink=info.st_nlink)

            with patch.object(MEDION, "LOCK", path), patch.object(MEDION.os, "fstat", root_owned_info):
                with MEDION.exclusive_lock():
                    with self.assertRaisesRegex(MEDION.DiagnosticError, "already holds the lock"):
                        with MEDION.exclusive_lock():
                            self.fail("Concurrent lock was granted")
                with MEDION.exclusive_lock():
                    pass

    def test_lock_cannot_follow_a_symlink(self):
        with tempfile.TemporaryDirectory(prefix="fte3600-lock-test-") as directory:
            root = Path(directory)
            destination = root / "existing-file"
            destination.write_text("preserve this data")
            lock = root / "lock"
            lock.symlink_to(destination)
            with patch.object(MEDION, "LOCK", lock):
                with self.assertRaises(OSError):
                    with MEDION.exclusive_lock():
                        self.fail("Symlink lock was accepted")
            self.assertEqual(destination.read_text(), "preserve this data")

    def test_character_node_is_verified_by_open_and_fstat(self):
        number = os.stat("/dev/null").st_rdev
        node = MEDION.DeviceNode(Path("/dev/null"), Path("/synthetic-sysfs"), number)
        MEDION.verify_node(node)
        with self.assertRaises(MEDION.DiagnosticError):
            MEDION.verify_node(MEDION.DeviceNode(node.path, node.sysfs, os.makedev(1, 5)))

    def test_regular_files_and_symlinks_cannot_impersonate_character_nodes(self):
        with tempfile.TemporaryDirectory(prefix="fte3600-node-test-") as directory:
            path = Path(directory) / "node"
            path.touch()
            number = os.stat("/dev/null").st_rdev
            with self.assertRaises(MEDION.DiagnosticError):
                MEDION.verify_node(MEDION.DeviceNode(path, path.parent, number))
            path.unlink()
            path.symlink_to("/dev/null")
            with self.assertRaises(MEDION.DiagnosticError):
                MEDION.verify_node(MEDION.DeviceNode(path, path.parent, number))

    def test_real_child_failure_is_not_success(self):
        with self.assertRaises(MEDION.DiagnosticError):
            MEDION.command([sys.executable, "-c", "raise SystemExit(7)"])

    def test_signal_handler_raises_and_restores_previous_handler(self):
        original = signal.getsignal(signal.SIGTERM)
        with self.assertRaises(MEDION.Interrupted):
            with MEDION.signal_cleanup():
                os.kill(os.getpid(), signal.SIGTERM)
        self.assertEqual(signal.getsignal(signal.SIGTERM), original)

    def test_child_exiting_before_kill_is_reaped_without_hiding_interruption(self):
        process = Mock()
        process.communicate.side_effect = MEDION.Interrupted(signal.SIGINT)
        process.poll.return_value = None
        process.pid = 123456
        with patch.object(MEDION.subprocess, "Popen", return_value=process), \
                patch.object(MEDION.os, "killpg", side_effect=ProcessLookupError):
            with self.assertRaises(MEDION.Interrupted):
                MEDION.command(["synthetic-diagnostic"])
        process.wait.assert_called_once()

    def test_second_interrupt_cannot_escape_child_termination_and_reaping(self):
        process = Mock()
        process.communicate.side_effect = MEDION.Interrupted(signal.SIGTERM)
        process.poll.return_value = None
        process.pid = 123456
        calls = []

        def wait(timeout=None):
            calls.append(timeout)
            self.assertEqual(signal.getsignal(signal.SIGINT), signal.SIG_IGN)
            os.kill(os.getpid(), signal.SIGINT)
            if len(calls) == 1:
                raise subprocess.TimeoutExpired("synthetic-diagnostic", timeout)
            return -signal.SIGKILL

        process.wait.side_effect = wait
        original = signal.getsignal(signal.SIGINT)
        with patch.object(MEDION.subprocess, "Popen", return_value=process), \
                patch.object(MEDION.os, "killpg") as kill:
            with self.assertRaises(MEDION.Interrupted):
                with MEDION.signal_cleanup():
                    MEDION.command(["synthetic-diagnostic"])
        self.assertEqual(len(calls), 2)
        self.assertEqual([call.args[1] for call in kill.call_args_list], [signal.SIGTERM, signal.SIGKILL])
        self.assertEqual(signal.getsignal(signal.SIGINT), original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
