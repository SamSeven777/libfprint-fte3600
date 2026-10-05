#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic sysfs topology and native file-boundary tests; no real device I/O."""
import ast
import importlib.util
import contextlib
import io
import os
from pathlib import Path
import re
import stat
import struct
import tempfile
import unittest
import subprocess
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fte_pair", ROOT / "scripts/fte3600-pair.py")
pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair)


class PairTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sys = self.root / "sys"
        self.dev = self.root / "dev"
        self.dev.mkdir()
        self.patch(pair, "SYS", self.sys)
        self.patch(pair, "DEV", self.dev)
        self.spi = self.sys / "devices/pci/spi1/spi-FTE3600:00"
        self.glue = self.spi / "fte3600-glue.0"
        self.gpio = self.glue / "gpiochip7"
        self.spidev = self.spi / "spidev/spidev1.0"
        self.irq = self.glue / "uio/uio4"
        self.acpi = self.sys / "devices/LNXSYSTM/FTE3600:00"
        for node in (self.spi, self.glue, self.gpio, self.spidev, self.irq, self.acpi):
            node.mkdir(parents=True, exist_ok=True)
        self.write(self.acpi, "hid", "FTE3600")
        self.link(self.spi / "firmware_node", self.acpi)
        for node, kind, driver in ((self.spi, "spi", "spidev"), (self.glue, "platform", "fte3600-glue")):
            target = self.sys / "bus" / kind / "drivers" / driver
            target.mkdir(parents=True)
            self.link(node / "driver", target)
        self.link(self.sys / "bus/platform/devices/fte3600-glue.0", self.glue)
        self.link(self.sys / "class/spidev/spidev1.0", self.spidev)
        self.link(self.spidev / "device", self.spi)
        self.link(self.sys / "bus/gpio/devices/gpiochip7", self.gpio)
        self.link(self.gpio / "device", self.glue)
        self.link(self.irq / "device", self.glue)
        self.link(self.sys / "class/uio/uio4", self.irq)
        self.write(self.irq, "name", "fte3600-irq")
        self.write(self.irq, "version", "2")
        for key, value in {"fte3600_glue_abi": "2", "fte3600_generation": "3",
                           "fte3600_status": "ready", "fte3600_acpi_mode": "0",
                           "fte3600_acpi_speed_hz": "1000000", "fte3600_irq_active_low": "0",
                           "fte3600_ngpio": "1", "fte3600_irq_source": "acpi", "fte3600_cs_control": "1"}.items():
            self.write(self.glue, key, value)
        for role, node, number in (("spi", self.spidev, "153:0"), ("gpio", self.gpio, "254:7"), ("irq", self.irq, "247:4")):
            self.write(node, "dev", number)
            (self.dev / node.name).touch()
            self.link(self.dev / f"fte3600-{role}-FTE3600:00", self.dev / node.name)

    def patch(self, target, name, value):
        patcher = mock.patch.object(target, name, value)
        patcher.start()
        self.addCleanup(patcher.stop)

    @staticmethod
    def write(node, key, value):
        (node / key).write_text(value + "\n")

    @staticmethod
    def link(path, target):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)

    def found(self):
        # Topology tests isolate sysfs policy from the separately tested native
        # cdev boundary; they never substitute production matching logic.
        with mock.patch.object(pair, "character_node", side_effect=lambda node, gpio=False, irq=False: str(self.dev / node.name)):
            return pair.pairs()

    def test_pair_and_dynamic_aliases(self):
        self.assertEqual(self.found(), [
            ("spi", str(self.dev / "spidev1.0"), str(self.dev / "fte3600-spi-FTE3600:00")),
            ("gpio", str(self.dev / "gpiochip7"), str(self.dev / "fte3600-gpio-FTE3600:00")),
            ("irq", str(self.dev / "uio4"), str(self.dev / "fte3600-irq-FTE3600:00")),
        ])

    def test_duplicate_class_view_is_one_gpio(self):
        self.link(self.sys / "class/gpio/gpiochip7", self.gpio)
        self.assertEqual(len(self.found()), 3)

    def test_gpio_bus_child_without_device_link(self):
        (self.gpio / "device").unlink()
        self.assertEqual(len(self.found()), 3)
        code, output, _ = self.invoke("--udev", "/" + str(self.gpio.relative_to(self.sys)))
        self.assertEqual(code, 0)
        self.assertEqual(output, "FTE3600_PAIR_ROLE=gpio\nFTE3600_PAIR_ID=FTE3600:00\n")

    def test_spidev_class_directory_without_device_link(self):
        (self.spidev / "device").unlink()
        self.assertEqual(pair.spi_parent(self.spidev), self.spi)
        self.assertEqual(len(self.found()), 3)

    def test_invalid_metadata(self):
        cases = {"fte3600_glue_abi": ("0", "1", "3"), "fte3600_status": ("suspended", "dead"),
                 "fte3600_generation": ("-1", str(1 << 64)), "fte3600_acpi_mode": ("1", "7"),
                 "fte3600_acpi_speed_hz": ("0", "NaN"), "fte3600_irq_active_low": ("2",),
                 "fte3600_ngpio": ("0", "2", "3"), "fte3600_irq_source": ("", "pci", "GPIO"),
                 "fte3600_cs_control": ("2", "-1", "", "true")}
        for key, values in cases.items():
            old = (self.glue / key).read_text()
            for value in values:
                with self.subTest(key=key, value=value):
                    self.write(self.glue, key, value)
                    with self.assertRaises(ValueError):
                        self.found()
            (self.glue / key).write_text(old)

    def test_cs_control_is_required_but_fixed_cs_is_valid(self):
        self.write(self.glue, "fte3600_cs_control", "0")
        self.assertEqual(len(self.found()), 3)
        (self.glue / "fte3600_cs_control").unlink()
        with self.assertRaises(FileNotFoundError):
            self.found()

    @contextlib.contextmanager
    def label_boundary(self):
        labels = {str(self.dev / node.name): "system_u:object_r:device_t:s0"
                  for node in (self.spidev, self.gpio, self.irq)}

        @contextlib.contextmanager
        def pinned(node):
            path = self.dev / node.name
            yield path, str(path)

        def setting(path, attribute, value):
            self.assertEqual(attribute, "security.selinux")
            labels[str(path)] = value.rstrip(b"\0").decode("ascii")

        with mock.patch.object(pair, "pinned_node", side_effect=pinned), \
             mock.patch.object(pair, "character_node", side_effect=lambda node, gpio=False, irq=False: str(self.dev / node.name)), \
             mock.patch.object(pair.os, "getxattr", side_effect=lambda path, attr: labels[str(path)].encode()), \
             mock.patch.object(pair.os, "setxattr", side_effect=setting) as setter:
            yield labels, setter

    def test_change_relabels_unchanged_permissions(self):
        with self.label_boundary() as (labels, setter):
            for node in (self.spidev, self.gpio, self.irq):
                # No chmod or permission transition enables the label change.
                with mock.patch.object(pair.os, "chmod") as chmod:
                    pair.relabel(node)
                    chmod.assert_not_called()
                role = pair.role_for_node(node)
                self.assertEqual(labels[str(self.dev / node.name)],
                                 "system_u:object_r:" + pair.LABEL_TYPES[role] + ":s0")
            self.assertEqual(setter.call_count, 3)
        rule = (ROOT / "config/udev/71-fte3600-acpi-spidev-selinux.rules").read_text()
        self.assertIn('ACTION=="add|change"', rule)
        self.assertIn('--relabel %p', rule)
        self.assertNotIn('SECLABEL{selinux}=', rule)

    def test_relabel_rejects_invalid_identity_before_xattr(self):
        with self.label_boundary() as (_, setter):
            self.write(self.acpi, "hid", "OTHER")
            with self.assertRaises(ValueError):
                pair.relabel(self.spidev)
            setter.assert_not_called()

    def test_relabel_stale_udev_identity_does_not_bypass_removed_metadata(self):
        with self.label_boundary() as (_, setter), mock.patch.object(pair.os, "geteuid", return_value=0):
            (self.glue / "fte3600_glue_abi").unlink()
            # A stale FTE3600_PAIR_ROLE in udev cannot authorize this write.
            code, _, _ = self.invoke("--relabel", "/" + str(self.spidev.relative_to(self.sys)))
            self.assertEqual(code, 1)
            setter.assert_not_called()

    def test_relabel_propagates_xattr_failure(self):
        with self.label_boundary(), mock.patch.object(pair.os, "setxattr", side_effect=PermissionError):
            with self.assertRaises(PermissionError):
                pair.relabel(self.spidev)

    def test_relabel_generation_change_prevents_write(self):
        original = pair.metadata
        calls = 0

        def changing(*args):
            nonlocal calls
            calls += 1
            result = original(*args)
            if calls == 3:
                self.write(self.glue, "fte3600_generation", "4")
            return result

        with self.label_boundary() as (_, setter), mock.patch.object(pair, "metadata", side_effect=changing):
            with self.assertRaises(ValueError):
                pair.relabel(self.spidev)
            setter.assert_not_called()

    def test_label_actions_require_root(self):
        with mock.patch.object(pair.os, "geteuid", return_value=1000):
            code, _, _ = self.invoke("--restore-labels")
            self.assertEqual(code, 1)
            code, _, _ = self.invoke("--relabel", "/" + str(self.spidev.relative_to(self.sys)))
            self.assertEqual(code, 1)

    def test_pinning_rejects_replaced_device_and_does_not_open_driver(self):
        before = type("DeviceStat", (), {"st_mode": stat.S_IFCHR | 0o600, "st_rdev": os.makedev(153, 0),
                                          "st_dev": 4, "st_ino": 123})()
        changed = type("DeviceStat", (), {"st_mode": stat.S_IFCHR | 0o600, "st_rdev": os.makedev(153, 0),
                                           "st_dev": 4, "st_ino": 124})()
        with mock.patch.object(Path, "lstat", return_value=before), \
             mock.patch.object(os, "open", return_value=9) as opened, \
             mock.patch.object(os, "fstat", return_value=changed), \
             mock.patch.object(os, "close") as closed:
            with self.assertRaises(ValueError):
                with pair.pinned_node(self.spidev):
                    self.fail("changed inode must not be exposed for labeling")
            self.assertEqual(opened.call_args.args[1], os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
            closed.assert_called_once_with(9)

    def test_restore_lone_spidev_after_glue_removal(self):
        with self.label_boundary() as (labels, setter):
            labels[str(self.dev / self.spidev.name)] = "system_u:object_r:fte3600_spidev_t:s0"
            (self.glue / "fte3600_glue_abi").unlink()
            (self.spidev / "device").unlink()
            for alias in self.dev.glob("fte3600-*"):
                alias.unlink()
            response = subprocess.CompletedProcess([], 0, stdout="system_u:object_r:device_t:s0\n")
            with mock.patch.object(pair.subprocess, "run", return_value=response) as command:
                pair.restore_labels()
            self.assertEqual(setter.call_count, 1)
            self.assertEqual(command.call_args.args[0], ["matchpathcon", "-n", str(self.dev / self.spidev.name)])
            self.assertEqual(labels[str(self.dev / self.spidev.name)], "system_u:object_r:device_t:s0")

    def test_restore_foreign_identity_or_default_override_is_rejected(self):
        for foreign in (False, True):
            with self.subTest(foreign=foreign), self.label_boundary() as (labels, setter):
                labels[str(self.dev / self.spidev.name)] = "system_u:object_r:fte3600_spidev_t:s0"
                self.write(self.acpi, "hid", "OTHER" if foreign else "FTE3600")
                response = subprocess.CompletedProcess([], 0, stdout="system_u:object_r:fte3600_spidev_t:s0\n")
                with mock.patch.object(pair.subprocess, "run", return_value=response):
                    with self.assertRaises(ValueError):
                        pair.restore_labels()
                setter.assert_not_called()

    def test_restore_default_lookup_failure_keeps_original_label(self):
        with self.label_boundary() as (labels, setter):
            labels[str(self.dev / self.spidev.name)] = "system_u:object_r:fte3600_spidev_t:s0"
            with mock.patch.object(pair.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "matchpathcon")):
                with self.assertRaises(subprocess.CalledProcessError):
                    pair.restore_labels()
            setter.assert_not_called()

    @unittest.skipUnless(hasattr(os, "O_PATH"), "Linux O_PATH is required")
    def test_pinned_inode_xattr_survives_path_replacement(self):
        original = self.root / "original"
        original.touch()
        fd = os.open(original, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
        self.addCleanup(os.close, fd)
        moved = self.root / "moved"
        original.rename(moved)
        original.touch()
        with mock.patch.object(pair, "LABEL_ATTRIBUTE", "user.fte3600-label-test"):
            context = "system_u:object_r:fte3600_spidev_t:s0"
            pair.set_label(f"/proc/self/fd/{fd}", context)
            self.assertEqual(pair.get_label(str(moved)), context)
            with self.assertRaises(OSError):
                pair.get_label(str(original))

    def test_suspended_label_identity_allowed_not_service(self):
        self.write(self.glue, "fte3600_status", "suspended")
        self.assertEqual(pair.metadata(self.glue, False)[2], "FTE3600:00")
        with self.assertRaises(ValueError):
            self.found()

    def invoke(self, *arguments):
        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch.object(pair.sys, "argv", ["fte3600-pair", *arguments]), \
             mock.patch.object(pair, "character_node", side_effect=lambda node, gpio=False, irq=False: str(self.dev / node.name)), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = pair.main()
        return result, output.getvalue(), errors.getvalue()

    def test_udev_can_label_first_node_without_companion_alias(self):
        (self.dev / "fte3600-gpio-FTE3600:00").unlink()
        self.write(self.glue, "fte3600_status", "suspended")
        code, output, _ = self.invoke("--udev", "/" + str(self.spidev.relative_to(self.sys)))
        self.assertEqual(code, 0)
        self.assertEqual(output, "FTE3600_PAIR_ROLE=spi\nFTE3600_PAIR_ID=FTE3600:00\n")
        self.write(self.glue, "fte3600_ngpio", "3")
        code, output, _ = self.invoke("--udev", "/" + str(self.spidev.relative_to(self.sys)))
        self.assertEqual((code, output), (1, ""))

    def test_refresh_only_verified_spidev_event(self):
        self.write(self.spi, "uevent", "original-parent")
        self.write(self.spidev, "uevent", "original-spidev")
        code, _, _ = self.invoke("--refresh-spi", "/" + str(self.spi.relative_to(self.sys)))
        self.assertEqual(code, 0)
        self.assertEqual((self.spidev / "uevent").read_text(), "change\n")
        self.assertEqual((self.spi / "uevent").read_text(), "original-parent\n")
        # A spidev change is not a physical SPI event and cannot refresh itself.
        code, _, _ = self.invoke("--refresh-spi", "/" + str(self.spidev.relative_to(self.sys)))
        self.assertEqual(code, 1)

    def test_refresh_unvalidated_node_has_no_event(self):
        self.write(self.spidev, "uevent", "unchanged")
        self.write(self.glue, "fte3600_glue_abi", "1")
        code, _, _ = self.invoke("--refresh-spi", "/" + str(self.spi.relative_to(self.sys)))
        self.assertEqual(code, 1)
        self.assertEqual((self.spidev / "uevent").read_text(), "unchanged\n")

    def test_unbound_or_wrong_parent_driver(self):
        for node in (self.spi, self.glue):
            target = (node / "driver").resolve()
            (node / "driver").unlink()
            with self.assertRaises(FileNotFoundError):
                self.found()
            wrong = self.sys / "bus/other/drivers/wrong"
            wrong.mkdir(parents=True, exist_ok=True)
            (node / "driver").symlink_to(wrong)
            with self.assertRaises(ValueError):
                self.found()
            (node / "driver").unlink()
            (node / "driver").symlink_to(target)

    def test_wrong_hid(self):
        self.write(self.acpi, "hid", "OTHER")
        with self.assertRaises(ValueError):
            self.found()

    def test_missing_or_other_gpio_is_rejected(self):
        (self.gpio / "device").unlink()
        (self.gpio / "device").symlink_to(self.acpi)
        with self.assertRaises(ValueError):
            self.found()

    def test_ambiguous_spi_rejected(self):
        other = self.spi / "spidev/spidev1.1"
        other.mkdir()
        self.write(other, "dev", "153:1")
        self.link(other / "device", self.spi)
        self.link(self.sys / "class/spidev/spidev1.1", other)
        with self.assertRaises(ValueError):
            self.found()

    def test_alias_must_target_verified_node(self):
        alias = self.dev / "fte3600-gpio-FTE3600:00"
        alias.unlink()
        with self.assertRaises(ValueError):
            self.found()
        alias.symlink_to(self.dev / "spidev1.0")
        with self.assertRaises(ValueError):
            self.found()

    def test_generation_change_rejects_publication(self):
        original = pair.metadata
        count = 0

        def changed(*args):
            nonlocal count
            result = original(*args)
            count += 1
            if count == 2:
                self.write(self.glue, "fte3600_generation", "4")
            return result
        with mock.patch.object(pair, "metadata", side_effect=changed):
            with self.assertRaises(ValueError):
                self.found()

    def test_no_sysfs_escape(self):
        (self.spi / "firmware_node").unlink()
        (self.spi / "firmware_node").symlink_to(self.root)
        with self.assertRaises(ValueError):
            self.found()

    def test_suspend_during_metadata_is_rejected(self):
        original = pair.read

        def suspend(path):
            value = original(path)
            if path.name == "fte3600_status":
                self.write(self.glue, "fte3600_generation", "4")
                self.write(self.glue, "fte3600_status", "suspended")
            return value
        with mock.patch.object(pair, "read", side_effect=suspend):
            with self.assertRaises(ValueError):
                pair.metadata(self.glue, True)

    def test_regular_file_and_symlink_are_not_devices(self):
        with self.assertRaises(ValueError):
            pair.character_node(self.gpio, True)
        (self.dev / "gpiochip7").unlink()
        (self.dev / "gpiochip7").symlink_to("/dev/null")
        with self.assertRaises(ValueError):
            pair.character_node(self.gpio, True)

    def test_gpio_native_boundary(self):
        info = type("DeviceStat", (), {"st_mode": stat.S_IFCHR | 0o600, "st_rdev": os.makedev(254, 7)})()
        for lines in (1, 2, 300):
            def ioctl(fd, command, buf, mutate):
                self.assertEqual((fd, command, mutate), (9, 0x8044B401, True))
                struct.pack_into("=I", buf, 64, lines)
            with mock.patch.object(Path, "lstat", return_value=info), \
                 mock.patch.object(os, "open", return_value=9) as opened, \
                 mock.patch.object(os, "fstat", return_value=info), \
                 mock.patch.object(os, "close") as closed, \
                 mock.patch.object(pair.fcntl, "ioctl", side_effect=ioctl):
                if lines == 1:
                    self.assertEqual(pair.character_node(self.gpio, True), str(self.dev / "gpiochip7"))
                else:
                    with self.assertRaises(ValueError):
                        pair.character_node(self.gpio, True)
                self.assertTrue(opened.call_args.args[1] & os.O_NOFOLLOW)
                closed.assert_called_once_with(9)

    def test_both_interrupt_origins_use_same_triplet(self):
        for source in ("gpio", "acpi"):
            self.write(self.glue, "fte3600_irq_source", source)
            self.assertEqual(len(self.found()), 3)

    def test_missing_and_foreign_irq_are_rejected(self):
        link = self.irq / "device"
        link.unlink()
        link.symlink_to(self.acpi)
        with self.assertRaises(ValueError):
            self.found()
        (self.sys / "class/uio/uio4").unlink()
        with self.assertRaises(ValueError):
            self.found()

    def test_irq_native_boundary_never_opens_or_maps(self):
        info = type("DeviceStat", (), {"st_mode": stat.S_IFCHR | 0o600,
                                       "st_rdev": os.makedev(247, 4)})()
        with mock.patch.object(Path, "lstat", return_value=info), \
             mock.patch.object(os, "open") as opened:
            self.assertEqual(pair.character_node(self.irq, irq=True), str(self.dev / "uio4"))
            for name, value in (("name", "other"), ("version", "1")):
                old = (self.irq / name).read_text()
                self.write(self.irq, name, value)
                with self.assertRaises(ValueError):
                    pair.character_node(self.irq, irq=True)
                (self.irq / name).write_text(old)
            for name in ("maps", "portio"):
                (self.irq / name).mkdir()
                with self.assertRaises(ValueError):
                    pair.character_node(self.irq, irq=True)
                (self.irq / name).rmdir()
            opened.assert_not_called()

    def test_generated_and_setup_rules_are_identical_and_guarded(self):
        source = (ROOT / "libfprint/fprint-list-udev-rules.c").read_text()
        block = re.search(r'g_print \("%s",\s*((?:"(?:\\.|[^"\\])*"\s*)+)\);', source)
        self.assertIsNotNone(block)
        generated = "".join(ast.literal_eval(token) for token in
                            re.findall(r'"(?:\\.|[^"\\])*"', block.group(1)))
        standalone = (ROOT / "config/udev/70-fte3600-acpi-spidev.rules").read_text()
        self.assertEqual(generated, standalone)
        # The leading dot makes this property event-local, so the duplicate
        # installed copy is skipped without suppressing later change events.
        guard = 'ENV{.FTE3600_RULES_DONE}=="1", GOTO="fte3600_pair_end"'
        assign = 'ENV{.FTE3600_RULES_DONE}="1"'
        self.assertLess(standalone.index(guard), standalone.index(assign))
        self.assertLess(standalone.index(assign), standalone.index('RUN{builtin}'))
        self.assertTrue(standalone.endswith('LABEL="fte3600_pair_end"\n'))
        self.assertIn('ACTION=="add|change|bind", SUBSYSTEM=="spi", DRIVER=="spidev"', standalone)


if __name__ == "__main__":
    unittest.main(verbosity=2)
