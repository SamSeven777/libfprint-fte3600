#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the real setup helpers against an isolated synthetic Linux tree.

Only copies of the scripts have their absolute system paths redirected. The
production tools deliberately have no environment-controlled installation root.
All system commands are mocked; filesystem commands are allowed only within
the temporary tree. No module, service, policy or real device is changed.
"""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
KERNEL = "fte3600-test-kernel"
NODE = "spidev1.0"
SENTINEL = "[Service]\n# pre-existing configuration must survive failure\n"


MOCK = r'''
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(os.environ["FTE_TEST_ROOT"])
tool = Path(sys.argv[0]).name
args = sys.argv[1:]
config = json.loads((root / "mock-config.json").read_text())
state = json.loads((root / "mock-state.json").read_text())
with (root / "commands.jsonl").open("a") as stream:
    stream.write(json.dumps([tool, *args]) + "\n")

def finish(code=0):
    (root / "mock-state.json").write_text(json.dumps(state))
    sys.exit(code)

operation = args[0] if args else ""
if tool == "systemctl":
    operation = next((arg for arg in args if not arg.startswith("-")), "")
elif tool == "semodule":
    operation = next((arg for arg in args if arg in ("-i", "-r", "-l")
                      or arg.startswith("--list")), operation)
failure = config.get("fail", {}).get(tool + ":" + operation,
                                    config.get("fail", {}).get(tool, 0))
if isinstance(failure, list):
    key = tool + ":" + operation
    count = state.setdefault("calls", {}).get(key, 0)
    state["calls"][key] = count + 1
    failure = failure[count] if count < len(failure) else 0
if failure:
    print("injected failure: " + tool + " " + operation, file=sys.stderr)
    finish(failure)

if tool in config["filesystem_tools"]:
    # Reject absolute destinations/sources outside our tree, including a
    # future helper change that forgets to redirect a new system directory.
    for arg in args:
        if arg.startswith("/"):
            try:
                Path(os.path.abspath(arg)).relative_to(root)
            except ValueError:
                print("refusing filesystem access outside fixture: " + arg,
                      file=sys.stderr)
                finish(99)
    result = subprocess.run([config["filesystem_tools"][tool], *args], check=False)
    finish(result.returncode)
elif tool == "id":
    print(config.get("uid", 0))
elif tool == "uname":
    print(config["kernel"])
elif tool == "lsmod":
    print("Module Size Used_by")
    if state["loaded"]:
        print("fte3600 16384 1")
elif tool == "modprobe":
    if "fte3600" in args:
        state["loaded"] = "-r" not in args
elif tool == "rmmod":
    state["loaded"] = False
elif tool == "insmod":
    state["loaded"] = True
elif tool == "dkms":
    if operation == "status" and state["dkms"]:
        print("fte3600/0.2, " + config["kernel"] + ", x86_64: installed")
    elif operation == "add":
        state["dkms"] = True
    elif operation == "remove":
        state["dkms"] = False
elif tool == "getenforce":
    print(config.get("selinux", "Disabled"))
elif tool == "semodule":
    if operation.startswith("--list") and state["policy"]:
        print(str(config.get("policy_priority", 400)) + " fte3600-acpi-spidev cil")
    elif operation == "-l" and state["policy"]:
        print("fte3600-acpi-spidev")
    elif operation == "-i":
        state["policy"] = True
    elif operation == "-r":
        state["policy"] = False
elif tool == "stat":
    if "%C" in args:
        print(config.get("file_context", state.get("labels", {}).get(Path(args[-1]).name,
                          "system_u:object_r:device_t:s0")))
    else:
        print("1:3")
elif tool == "systemctl":
    if operation == "show":
        print(config.get("service_load_state", "loaded"))
    elif operation == "is-active":
        if "--quiet" not in args:
            print("active" if state["service"] else "inactive")
        finish(0 if state["service"] else 3)
    elif operation == "stop":
        state["service"] = False
    elif operation in ("start", "restart"):
        state["service"] = True
elif tool == "udevadm":
    # Real udev does not apply SECLABEL on CHANGE when permissions are unchanged.
    # In this model only the explicit validated label helper changes labels.
    if operation == "info":
        node = next(arg.split("=", 1)[1] for arg in args if arg.startswith("--name="))
        print("/devices/fixture/" + Path(node).name)
    elif operation == "settle":
        state.setdefault("labels", {}).update(config.get("pending_relabel", {}))
        if config.get("reload_on_settle", False):
            state["loaded"] = True
elif tool not in ("make", "gcc", "depmod", "restorecon", "matchpathcon"):
    print("unimplemented mock: " + tool, file=sys.stderr)
    finish(98)
finish()
'''

PAIR_MOCK = r'''
import json,os,sys
from pathlib import Path
root=Path(os.environ['FTE_TEST_ROOT'])
config=json.loads((root/'mock-config.json').read_text())
state=json.loads((root/'mock-state.json').read_text())
args=sys.argv[1:]
if not args:
    print(config.get('pair_output',''),end='')
    sys.exit(config.get('pair_exit',1))
with (root/'commands.jsonl').open('a') as stream:
    stream.write(json.dumps(['pair-label',*args])+'\n')
if args[0]=='--relabel':
    if config.get('label_exit',0):
        sys.exit(config['label_exit'])
    name=Path(args[1]).name
    kind='spidev' if name.startswith('spidev') else 'gpio' if name.startswith('gpiochip') else 'irq'
    state.setdefault('labels',{})[name]='system_u:object_r:fte3600_'+kind+'_t:s0'
elif args[0]=='--restore-labels':
    if config.get('restore_exit',0):
        sys.exit(config['restore_exit'])
    for name,label in state.get('labels',{}).items():
        if label.split(':')[2] in ('fte3600_spidev_t','fte3600_gpio_t','fte3600_irq_t'):
            state['labels'][name]='system_u:object_r:device_t:s0'
else:
    sys.exit(98)
(root/'mock-state.json').write_text(json.dumps(state))
'''


class Fixture:
    """Fresh scripts, fake hardware and stateful command mocks for one test."""

    def __init__(self, root):
        self.root = root
        self.checkout = root / "checkout"
        self.bin = root / "bin"
        self.bin.mkdir()
        (root / "tmp").mkdir()
        self.scripts = self.checkout / "scripts"
        self.scripts.mkdir(parents=True)
        for name in ("setup-fte3600.sh", "fte3600-device-allow.sh"):
            text = (REPO / "scripts" / name).read_text()
            text = text.replace("/dev/null", "@FTE_TEST_NULL@")
            for prefix in ("/etc", "/usr/src", "/usr/libexec", "/lib/modules", "/var/lib/dkms", "/sys", "/dev/"):
                replacement = str(root / prefix.lstrip("/"))
                if prefix.endswith("/"):
                    replacement += "/"
                # A /sys replacement must not rewrite the /systemd component
                # of a previously redirected /etc/systemd path.
                pattern = re.escape(prefix)
                if not prefix.endswith("/"):
                    pattern += r'(?=/|[\s"\']|$)'
                text = re.sub(pattern, lambda match: replacement, text)
            text = text.replace("@FTE_TEST_NULL@", "/dev/null")
            target = self.scripts / name
            target.write_text(text)
            target.chmod(0o755)

        kernel_tree = self.checkout / "kernel/fte3600"
        kernel_tree.mkdir(parents=True)
        for name in ("fte3600.c", "fte3600-policy.h", "fte3600-policy-test.c", "Makefile", "dkms.conf"):
            shutil.copyfile(REPO / "kernel/fte3600" / name, kernel_tree / name)
        (kernel_tree / "fte3600.ko").write_bytes(b"synthetic module; never loaded")
        for relative in ("config/selinux/fte3600-acpi-spidev.cil",
                         "config/udev/70-fte3600-acpi-spidev.rules",
                         "config/udev/71-fte3600-acpi-spidev-selinux.rules",
                         "config/modprobe.d/fte3600-acpi-spidev.conf"):
            target = self.checkout / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / relative, target)
        # Pairing has its own real-sysfs-policy unit suite. This integration
        # fixture supplies that command's contract without requiring mknod/root.
        (self.scripts / "fte3600-pair.py").write_text(PAIR_MOCK)
        buffer = root / "sys/module/spidev/parameters/bufsiz"
        buffer.parent.mkdir(parents=True)
        buffer.write_text("32768\n")
        (root / f"lib/modules/{KERNEL}/build").mkdir(parents=True)
        (root / "etc").mkdir()
        (root / "etc/os-release").write_text('ID=test\nPRETTY_NAME="Fixture Linux"\n')
        (root / "dev").mkdir()
        self.dropin = root / "etc/systemd/system/fprintd.service.d/10-fte3600-acpi-spidev.conf"
        self.module = root / f"lib/modules/{KERNEL}/extra/fte3600.ko"
        self.dkms_source = root / "usr/src/fte3600-0.2"

        filesystem_tools = {}
        for name in ("cp", "mkdir", "rm", "mv", "chmod", "install", "ln", "touch"):
            filesystem_tools[name] = shutil.which(name)
            if not filesystem_tools[name]:
                raise RuntimeError("Required filesystem utility is unavailable: " + name)
        self.config = {"kernel": KERNEL, "fail": {}, "filesystem_tools": filesystem_tools}
        self.initial_state = {"loaded": False, "dkms": False,
                              "policy": False, "service": True}
        self.save_config()
        self.set_state(**self.initial_state)
        commands = ("id", "uname", "dkms", "make", "gcc", "depmod", "modprobe",
                    "insmod", "rmmod", "lsmod", "getenforce", "semodule", "matchpathcon",
                    "systemctl", "restorecon", "stat", "udevadm", *filesystem_tools)
        for name in commands:
            target = self.bin / name
            target.write_text("#!" + sys.executable + "\n" + MOCK)
            target.chmod(0o755)
        # An allowlist instead of appending the host PATH prevents accidentally
        # invoking newly added system-management commands against the host.
        for name in ("bash", "sh", "basename", "dirname", "cat", "sed", "grep",
                     "readlink", "realpath", "mktemp", "tr", "cut", "find",
                     "sort", "awk", "head", "tail", "tee", "date", "python3", "systemd-escape"):
            executable = shutil.which(name)
            if executable:
                (self.bin / name).symlink_to(executable)

    def save_config(self):
        (self.root / "mock-config.json").write_text(json.dumps(self.config))

    def set_state(self, **changes):
        path = self.root / "mock-state.json"
        state = json.loads(path.read_text()) if path.exists() else {}
        state.update(changes)
        path.write_text(json.dumps(state))

    def state(self):
        return json.loads((self.root / "mock-state.json").read_text())

    def commands(self):
        path = self.root / "commands.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def preserve_dropin(self):
        self.dropin.parent.mkdir(parents=True, exist_ok=True)
        self.dropin.write_text(SENTINEL)

    def installed(self):
        self.preserve_dropin()
        self.module.parent.mkdir(parents=True, exist_ok=True)
        self.module.write_bytes(b"previous installed module")
        self.dkms_source.mkdir(parents=True, exist_ok=True)
        (self.dkms_source / "saved-source").write_text("previous installed source")
        self.set_state(loaded=True, dkms=True, policy=True)

    def bridge(self, abi="2", driver="fte3600", character=True, name=NODE):
        self.config["pair_exit"] = 0 if abi == "2" and driver == "fte3600" and character else 1
        rows = []
        self.aliases = []
        self.devices = []
        for role, basename in (("spi", name), ("gpio", "gpiochip7"), ("irq", "uio4")):
            device = self.root / "dev" / basename
            if not device.exists():
                device.symlink_to("/dev/null")
            alias = self.root / "dev" / ("fte3600-" + role + "-FTE3600:00")
            if not alias.exists():
                alias.symlink_to(device)
            rows.append(f"{role} {device} {alias}\n")
            self.aliases.append(alias)
            self.devices.append(device)
        self.config["pair_output"] = "".join(rows) if not self.config["pair_exit"] else ""
        return self.devices[0]

    def expected_dropin(self):
        units = []
        for alias in self.aliases:
            unit = subprocess.check_output([shutil.which("systemd-escape"), "--path", "--suffix=device", str(alias)], text=True).strip()
            units.append(f"BindsTo={unit}\nAfter={unit}\n")
        return "[Unit]\n" + "".join(units) + "[Service]\n" + "".join(f"DeviceAllow={a} {'r' if a.name.startswith('fte3600-irq-') else 'rw'}\n" for a in self.aliases)

    def run(self, script, *arguments):
        self.save_config()
        env = {"PATH": str(self.bin), "LC_ALL": "C", "HOME": str(self.root),
               "TMPDIR": str(self.root / "tmp"), "FTE_TEST_ROOT": str(self.root)}
        result = subprocess.run([str(self.bin / "bash"), str(self.scripts / script), *arguments],
                                env=env, cwd=self.checkout, text=True,
                                capture_output=True, timeout=20, check=False)
        result.stdout = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
        return result


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "Linux shell helpers require POSIX and bash")
class SetupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="fte3600-setup-test-")
        self.addCleanup(self.tmp.cleanup)
        self.fixture = Fixture(Path(self.tmp.name))

    def assert_failed(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotRegex(result.stdout, r"(?im)^\[ OK \].*(?:all .*applied|uninstall .*complete|fprintd .*restarted)")

    def assert_preserved(self):
        f = self.fixture
        self.assertEqual(f.dropin.read_text(), SENTINEL)
        self.assertEqual(f.module.read_bytes(), b"previous installed module")
        self.assertEqual((f.dkms_source / "saved-source").read_text(), "previous installed source")

    def assert_command_before(self, first, second):
        commands = self.fixture.commands()
        first_index = next(i for i, command in enumerate(commands) if command[:len(first)] == first)
        second_index = next(i for i, command in enumerate(commands) if command[:len(second)] == second)
        self.assertLess(first_index, second_index, commands)

    def assert_called(self, prefix):
        commands = self.fixture.commands()
        self.assertTrue(any(command[:len(prefix)] == prefix for command in commands), commands)

    def test_device_allow_prints_only_verified_node_without_writing(self):
        f = self.fixture
        device = f.bridge()
        f.preserve_dropin()
        result = f.run("fte3600-device-allow.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f.expected_dropin())
        self.assertEqual(f.dropin.read_text(), SENTINEL)
        self.assertNotIn("systemctl", [command[0] for command in f.commands()])

    def test_device_allow_installs_exact_node(self):
        f = self.fixture
        device = f.bridge()
        result = f.run("fte3600-device-allow.sh", "--install")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.dropin.read_text(), f.expected_dropin())
        self.assertIn(["systemctl", "daemon-reload"], f.commands())

    def test_device_allow_missing_bridge_preserves_configuration(self):
        f = self.fixture
        f.preserve_dropin()
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_device_allow_acpi_alone_preserves_configuration(self):
        f = self.fixture
        (f.root / "sys/bus/acpi/devices/FTE3600:00").mkdir(parents=True)
        f.preserve_dropin()
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_device_allow_wrong_abi_preserves_configuration(self):
        f = self.fixture
        f.bridge(abi="999")
        f.preserve_dropin()
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_device_allow_wrong_driver_preserves_configuration(self):
        f = self.fixture
        f.bridge(driver="spidev")
        f.preserve_dropin()
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_device_allow_regular_file_preserves_configuration(self):
        f = self.fixture
        f.bridge(character=False)
        f.preserve_dropin()
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_device_allow_atomic_write_failure_preserves_configuration(self):
        f = self.fixture
        f.bridge()
        f.preserve_dropin()
        for command in ("chmod", "mv"):
            with self.subTest(command=command):
                f.config["fail"] = {command: 1}
                self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
                self.assertEqual(f.dropin.read_text(), SENTINEL)
                self.assertEqual(list(f.dropin.parent.iterdir()), [f.dropin])

    def test_device_allow_conflicting_actions_preserve_configuration(self):
        f = self.fixture
        f.bridge()
        f.preserve_dropin()
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install", "--remove"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_device_allow_unprivileged_install_preserves_configuration(self):
        f = self.fixture
        f.bridge()
        f.preserve_dropin()
        f.config["uid"] = 1000
        self.assert_failed(f.run("fte3600-device-allow.sh", "--install"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_install_all_success(self):
        f = self.fixture
        device = f.bridge()
        result = f.run("setup-fte3600.sh", "install-all")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(f.state()["loaded"])
        self.assertTrue(f.state()["dkms"])
        self.assertEqual(f.dropin.read_text(), f.expected_dropin())
        self.assertIn(["systemctl", "restart", "fprintd.service"], f.commands())

    def test_install_module_load_failure_is_not_success(self):
        f = self.fixture
        f.bridge()
        f.preserve_dropin()
        f.config["fail"]["modprobe:fte3600"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertFalse(f.state()["loaded"])
        self.assertEqual(f.dropin.read_text(), SENTINEL)
        self.assert_called(["modprobe", "fte3600"])
        self.assertNotIn("insmod", [command[0] for command in f.commands()])

    def test_install_no_live_bridge_is_not_success(self):
        f = self.fixture
        (f.root / "sys/bus/acpi/devices/FTE3600:00").mkdir(parents=True)
        f.preserve_dropin()
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertEqual(f.dropin.read_text(), SENTINEL)

    def test_install_service_restart_failure_is_not_success(self):
        f = self.fixture
        f.bridge()
        f.config["fail"]["systemctl:restart"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertIn(["systemctl", "restart", "fprintd.service"], f.commands())

    def test_install_dkms_failure_is_not_success(self):
        f = self.fixture
        f.bridge()
        f.config["fail"]["dkms:build"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertFalse(f.state()["loaded"])
        self.assert_called(["dkms", "build"])

    def test_install_selinux_failure_is_not_success(self):
        f = self.fixture
        f.bridge()
        f.config["selinux"] = "Enforcing"
        f.config["fail"]["semodule:-i"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertFalse(f.state()["policy"])
        self.assert_called(["semodule", "-i"])

    def test_selinux_install_labels_only_verified_companions(self):
        f = self.fixture
        f.bridge()
        (f.root / "dev/unrelated-device").symlink_to("/dev/null")
        f.config["selinux"] = "Enforcing"
        result = f.run("setup-fte3600.sh", "install-selinux")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(f.state()["policy"])
        self.assertEqual([c for c in f.commands() if c[0] == "stat"],
                         [["stat", "-c", "%C", "--", str(n)] for n in f.devices])
        self.assertNotIn("restorecon", [c[0] for c in f.commands()])
        self.assertEqual(set(f.state()["labels"]), {n.name for n in f.devices})
        self.assertEqual(len([c for c in f.commands() if c[:2] == ["pair-label", "--relabel"]]), 3)

    def test_selinux_explicit_label_failure_prevents_restart(self):
        f = self.fixture
        f.bridge()
        f.config.update(selinux="Enforcing", label_exit=1)
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertNotIn(["systemctl", "restart", "fprintd.service"], f.commands())

    def test_selinux_wrong_actual_context_is_rejected(self):
        f = self.fixture
        f.bridge()
        f.config["selinux"] = "Enforcing"
        f.config["file_context"] = "system_u:object_r:gpio_device_t:s0"
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertNotIn(["systemctl", "restart", "fprintd.service"], f.commands())

    def test_selinux_event_or_label_read_failure_is_not_success(self):
        f = self.fixture
        f.bridge()
        f.config["selinux"] = "Enforcing"
        for failure in ("udevadm:trigger", "udevadm:settle", "udevadm:info", "stat"):
            with self.subTest(failure=failure):
                f.config["fail"] = {failure: 1}
                self.assert_failed(f.run("setup-fte3600.sh", "install-selinux"))

    def test_loaded_small_spidev_is_not_unloaded_or_misreported(self):
        f = self.fixture
        f.bridge()
        (f.root / "sys/module/spidev/parameters/bufsiz").write_text("4096\n")
        result = f.run("setup-fte3600.sh", "install-all")
        self.assert_failed(result)
        self.assertIn("32768", result.stderr)
        self.assertFalse(any(c[0] == "rmmod" or (c[0] == "modprobe" and "-r" in c) for c in f.commands()))
        self.assertFalse(f.state()["loaded"])

    def test_legacy_dropin_blocks_install_before_mutation(self):
        f = self.fixture
        f.bridge()
        old = f.root / "etc/systemd/system/fprintd.service.d/10-fte3600-bridge.conf"
        old.parent.mkdir(parents=True)
        old.write_text("old configuration")
        self.assert_failed(f.run("setup-fte3600.sh", "install-all"))
        self.assertEqual(old.read_text(), "old configuration")
        self.assertFalse(any(c[0] in ("modprobe", "install", "dkms") for c in f.commands()))

    def test_check_failure_is_nonzero(self):
        self.assert_failed(self.fixture.run("setup-fte3600.sh", "check"))

    def test_uninstall_stops_service_before_unload(self):
        f = self.fixture
        f.installed()
        f.config["selinux"] = "Enforcing"
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_command_before(["systemctl", "stop"], ["rmmod", "fte3600"])
        self.assertFalse(f.state()["loaded"])
        self.assertFalse(f.state()["dkms"])
        self.assertFalse(f.state()["policy"])
        self.assertFalse(f.module.exists())
        self.assertFalse(f.dkms_source.exists())
        self.assertFalse(f.dropin.exists())
        self.assertTrue(f.state()["service"])

    def test_uninstall_does_not_start_previously_inactive_service(self):
        f = self.fixture
        f.installed()
        f.set_state(service=False)
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(f.state()["service"])
        self.assertFalse(any(command[:2] in (["systemctl", "start"], ["systemctl", "restart"])
                             for command in f.commands()))

    def test_uninstall_without_dkms_retains_registered_sources(self):
        f = self.fixture
        f.installed()
        (f.bin / "dkms").unlink()
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assert_preserved()
        self.assertNotIn("rmmod", [command[0] for command in f.commands()])

    def test_uninstall_dkms_status_failure_preserves_configuration(self):
        f = self.fixture
        f.installed()
        f.config["fail"]["dkms:status"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assert_called(["dkms", "status"])
        self.assert_preserved()

    def test_uninstall_module_queries_fail_closed(self):
        f = self.fixture
        for call in (1, 2, 3, 4):
            with self.subTest(query=call):
                f.installed()
                f.set_state(service=True, calls={})
                f.config["fail"]["lsmod"] = [0] * (call - 1) + [1]
                self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
                self.assert_preserved()

    def test_uninstall_drains_queued_label_writers_before_restoration(self):
        f = self.fixture
        f.installed()
        node = f.bridge()
        f.config.update(selinux="Enforcing", pending_relabel={
            node.name: "system_u:object_r:fte3600_spidev_t:s0"})
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = f.commands()
        settle = commands.index(["udevadm", "settle", "--timeout=10"])
        restore = commands.index(["pair-label", "--restore-labels"])
        queries = [i for i, command in enumerate(commands) if command == ["lsmod"]]
        self.assertLess(commands.index(["rmmod", "fte3600"]), settle)
        self.assertLess(settle, queries[-1])
        self.assertLess(queries[-1], restore)
        self.assertEqual(f.state()["labels"][node.name], "system_u:object_r:device_t:s0")

    def test_uninstall_queued_module_reload_retains_configuration(self):
        f = self.fixture
        for loaded in (False, True):
            with self.subTest(initially_loaded=loaded):
                f.installed()
                f.set_state(loaded=loaded, service=True)
                f.config.update(selinux="Enforcing", reload_on_settle=True)
                result = f.run("setup-fte3600.sh", "uninstall")
                self.assert_failed(result)
                self.assertIn("reloaded", result.stderr)
                self.assertTrue(f.state()["loaded"])
                self.assertTrue(f.state()["policy"])
                self.assert_preserved()
                self.assertNotIn(["pair-label", "--restore-labels"], f.commands())

    def test_uninstall_device_event_timeout_retains_configuration(self):
        f = self.fixture
        f.installed()
        f.config.update(selinux="Enforcing")
        f.config["fail"]["udevadm:settle"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assertTrue(f.state()["policy"])
        self.assert_preserved()
        self.assertNotIn(["pair-label", "--restore-labels"], f.commands())

    def test_uninstall_disabled_selinux_removes_stored_policy_without_relabel(self):
        f = self.fixture
        f.installed()
        f.bridge()
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(f.state()["policy"])
        self.assertIn(["semodule", "-n", "-X", "400", "-r", "fte3600-acpi-spidev"], f.commands())
        self.assertNotIn("restorecon", [command[0] for command in f.commands()])
        self.assertNotIn(["pair-label", "--restore-labels"], f.commands())

    def test_uninstall_incomplete_pair_restores_surviving_node_before_policy_removal(self):
        f = self.fixture
        f.installed()
        node = f.bridge()
        f.config.update(selinux="Enforcing", pair_exit=1, pair_output="")
        f.set_state(loaded=False, labels={node.name: "system_u:object_r:fte3600_spidev_t:s0"})
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(f.state()["labels"][node.name], "system_u:object_r:device_t:s0")
        self.assert_command_before(["pair-label", "--restore-labels"], ["semodule", "-X", "400", "-r"])
        self.assertFalse(f.state()["policy"])
        # Repeated removal is successful without requiring a live pair.
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_uninstall_label_failure_retains_policy_and_retry_files(self):
        f = self.fixture
        f.installed()
        node = f.bridge()
        f.config.update(selinux="Enforcing", restore_exit=1, pair_exit=1, pair_output="")
        f.set_state(loaded=False, labels={node.name: "system_u:object_r:fte3600_spidev_t:s0"})
        for relative in ("etc/udev/rules.d/71-fte3600-acpi-spidev-selinux.rules", "usr/libexec/fte3600-pair"):
            target = f.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("keep for retry")
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assertTrue(f.state()["policy"])
        self.assertTrue(f.dropin.exists())
        self.assertTrue((f.root / "etc/udev/rules.d/71-fte3600-acpi-spidev-selinux.rules").exists())
        self.assertTrue((f.root / "usr/libexec/fte3600-pair").exists())

    def test_uninstall_retains_distribution_priority_policy(self):
        f = self.fixture
        f.installed()
        f.config["selinux"] = "Enforcing"
        f.config["policy_priority"] = 100
        result = f.run("setup-fte3600.sh", "uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(f.state()["policy"])
        self.assertFalse(any(command[0] == "semodule" and "-r" in command
                             for command in f.commands()))

    def test_uninstall_busy_module_preserves_disk_configuration(self):
        f = self.fixture
        f.installed()
        f.config["fail"]["rmmod"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assert_command_before(["systemctl", "stop"], ["rmmod", "fte3600"])
        self.assertTrue(f.state()["loaded"])
        self.assertTrue(f.state()["service"])
        self.assert_preserved()

    def test_uninstall_service_stop_failure_preserves_disk_configuration(self):
        f = self.fixture
        f.installed()
        f.config["fail"]["systemctl:stop"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assertNotIn("rmmod", [command[0] for command in f.commands()])
        self.assert_preserved()

    def test_uninstall_dkms_failure_preserves_source(self):
        f = self.fixture
        f.installed()
        f.config["fail"]["dkms:remove"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assertTrue(f.state()["dkms"])
        self.assert_called(["dkms", "remove"])
        self.assert_preserved()

    def test_uninstall_selinux_failure_is_not_success(self):
        f = self.fixture
        f.installed()
        f.config["selinux"] = "Enforcing"
        f.config["fail"]["semodule:-r"] = 1
        self.assert_failed(f.run("setup-fte3600.sh", "uninstall"))
        self.assertTrue(f.state()["policy"])
        self.assertIn(["semodule", "-X", "400", "-r", "fte3600-acpi-spidev"], f.commands())


if __name__ == "__main__":
    unittest.main(verbosity=2)
