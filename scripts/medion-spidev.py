#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Temporary, resource-checked spidev access for the Medion E3224 experiment.

This launcher does not identify a sensor from a computer name. It requires the
specific ACPI wiring already established for this experiment, then delegates
protocol identification to the C diagnostic. No persistent rules are written.
"""

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import errno
import fcntl
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time


SYS = Path("/sys")
DEV = Path("/dev")
PROC = Path("/proc")
LOCK = Path("/run/lock/fte3600-medion.lock")
BUFFER_SIZE = 32768
MIN_PROBE_BUFFER = 64
NODE_TIMEOUT = 2.0
SERVICE = "fprintd.service"
SPI_ACPI = r"\_SB_.PCI0.SPI1.FP05"
RESET_ACPI = r"\_SB_.GPO1"
IRQ_ACPI = r"\_SB_.GPO2"


class DiagnosticError(Exception):
    exit_code = 1


class Interrupted(DiagnosticError):
    def __init__(self, signum):
        super().__init__(f"Interrupted by signal {signum}")
        self.exit_code = 128 + signum


def attribute(path):
    try:
        with path.open("r", encoding="ascii") as stream:
            value = stream.read(4097)
    except (OSError, UnicodeError) as error:
        raise DiagnosticError(f"Cannot read {path}: {error}") from error
    if len(value) > 4096:
        raise DiagnosticError(f"Oversized sysfs attribute: {path}")
    return value.strip()


def canonical(path):
    try:
        return path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise DiagnosticError(f"Cannot resolve {path}: {error}") from error


def unique(items, description):
    items = list(dict.fromkeys(items))
    if len(items) != 1:
        raise DiagnosticError(f"Expected one {description}; found {len(items)}")
    return items[0]


def find_acpi(hid, acpi_path):
    matches = []
    for device in sorted((SYS / "bus/acpi/devices").glob("*")):
        if not (device / "hid").is_file() or not (device / "path").is_file():
            continue
        if attribute(device / "hid") == hid and attribute(device / "path") == acpi_path:
            matches.append(canonical(device))
    return unique(matches, f"ACPI {hid} at {acpi_path}")


def firmware_nodes(device):
    """Support gpiolib's direct and parent-device firmware-node layouts."""
    device = canonical(device)
    result = set()
    for owner in (device, device / "device", device.parent):
        node = owner / "firmware_node"
        if node.exists():
            result.add(canonical(node))
    return result


@dataclass(frozen=True)
class DeviceNode:
    path: Path
    sysfs: Path
    device_number: int


@dataclass(frozen=True)
class Resources:
    spi: Path
    controller: Path
    reset: DeviceNode
    irq: DeviceNode


def character_descriptor(sysfs, name):
    if not re.fullmatch(r"[A-Za-z0-9_.:-]+", name):
        raise DiagnosticError(f"Invalid device name: {name!r}")
    value = attribute(sysfs / "dev")
    if not re.fullmatch(r"\d+:\d+", value):
        raise DiagnosticError(f"Invalid device number at {sysfs}: {value!r}")
    major, minor = map(int, value.split(":"))
    try:
        number = os.makedev(major, minor)
    except (OverflowError, ValueError) as error:
        raise DiagnosticError(f"Invalid device number at {sysfs}") from error
    return DeviceNode(DEV / name, sysfs, number)


def gpio_for(acpi, line):
    matches = []
    candidates = list((SYS / "bus/gpio/devices").glob("gpiochip*"))
    candidates += list((SYS / "class/gpio").glob("gpiochip*"))
    for candidate in candidates:
        device = canonical(candidate)
        if not re.fullmatch(r"gpiochip\d+", device.name) or not (device / "dev").is_file():
            continue
        if acpi in firmware_nodes(device):
            if (device / "ngpio").is_file():
                count = attribute(device / "ngpio")
                if not count.isdecimal() or int(count) <= line:
                    raise DiagnosticError(f"{device} cannot provide GPIO line {line}")
            matches.append(device)
    device = unique(matches, f"GPIO character device for {acpi.name}")
    return character_descriptor(device, device.name)


def discover():
    spi_acpi = find_acpi("FTE3600", SPI_ACPI)
    reset_acpi = find_acpi("INT3453", RESET_ACPI)
    irq_acpi = find_acpi("INT3453", IRQ_ACPI)
    devices = [canonical(path) for path in (SYS / "bus/spi/devices").glob("*")]
    spi = unique((path for path in devices if (path / "firmware_node").exists()
                  and canonical(path / "firmware_node") == spi_acpi), "SPI device for FTE3600")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]+", spi.name):
        raise DiagnosticError("Invalid SPI device name")
    controller = spi.parent
    masters = [canonical(path) for path in (SYS / "class/spi_master").glob("*")]
    if controller not in masters:
        raise DiagnosticError(f"Cannot establish the SPI controller for {spi}")
    siblings = [path for path in devices if path.parent == controller]
    if len(siblings) != 1:
        raise DiagnosticError(f"SPI controller {controller.name} has other devices; refusing shared-bus experiment")
    reset = gpio_for(reset_acpi, 39)
    irq = gpio_for(irq_acpi, 0)
    if reset.sysfs == irq.sysfs or reset.device_number == irq.device_number:
        raise DiagnosticError("Reset and IRQ unexpectedly resolve to the same GPIO chip")
    return Resources(spi, controller, reset, irq)


def binding(spi):
    driver = spi / "driver"
    return canonical(driver).name if driver.exists() else None


def spidev_node(spi, required=True):
    matches = []
    for entry in (SYS / "class/spidev").glob("*"):
        if (entry / "device").exists() and canonical(entry / "device") == spi:
            matches.append(entry)
    if not matches and not required:
        return None
    entry = unique(matches, "spidev node associated with the target SPI device")
    if not re.fullmatch(r"spidev\d+\.\d+", entry.name):
        raise DiagnosticError(f"Unexpected spidev node name: {entry.name}")
    return character_descriptor(entry, entry.name)


def verify_node(node, writable=False):
    """Open only spidev/gpiochip nodes; never open an old resource bridge."""
    flags = (os.O_RDWR if writable else os.O_RDONLY) | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW
    try:
        fd = os.open(node.path, flags)
        try:
            info = os.fstat(fd)
            if not stat.S_ISCHR(info.st_mode) or info.st_rdev != node.device_number:
                raise DiagnosticError(f"{node.path} is not the character device declared by {node.sysfs}/dev")
        finally:
            os.close(fd)
    except OSError as error:
        raise DiagnosticError(f"Cannot validate {node.path}: {error}") from error


def wait_for_node(node):
    """Binding can finish before devtmpfs/udev has published this node."""
    deadline = time.monotonic() + NODE_TIMEOUT
    while True:
        try:
            verify_node(node, writable=True)
            return
        except DiagnosticError as error:
            cause = error.__cause__
            if not isinstance(cause, OSError) or cause.errno != errno.ENOENT:
                raise
            if time.monotonic() >= deadline:
                raise DiagnosticError(f"Timed out waiting for the target character device {node.path}") from error
            time.sleep(0.025)


def write_sysfs(path, value):
    data = (value + "\n").encode("ascii")
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            if os.write(fd, data) != len(data):
                raise DiagnosticError(f"Incomplete sysfs write: {path}")
        finally:
            os.close(fd)
    except OSError as error:
        raise DiagnosticError(f"Cannot write {path}: {error}") from error


def command(arguments, check=True, inherit=False):
    """Keep subprocesses in their own group and reap them before restoration."""
    try:
        process = subprocess.Popen(arguments, text=True, start_new_session=True,
                                   stdout=None if inherit else subprocess.PIPE,
                                   stderr=None if inherit else subprocess.PIPE)
    except OSError as error:
        raise DiagnosticError(f"Cannot execute {arguments[0]}: {error}") from error
    try:
        stdout, stderr = process.communicate(timeout=None if inherit else 30)
    except BaseException:
        # Reap before the caller restores binding/GPIO/service state. A second
        # signal must not escape this block while the diagnostic is still live.
        with ignore_cleanup_signals():
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
        raise
    result = subprocess.CompletedProcess(arguments, process.returncode, stdout or "", stderr or "")
    if check and result.returncode:
        detail = result.stderr.strip()[:4096]
        raise DiagnosticError(f"{' '.join(map(str, arguments))} failed ({result.returncode})" +
                              (f": {detail}" if detail else ""))
    return result


@contextmanager
def ignore_cleanup_signals():
    previous = {sig: signal.signal(sig, signal.SIG_IGN)
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    try:
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


@contextmanager
def signal_cleanup():
    def interrupt(signum, frame):
        raise Interrupted(signum)

    previous = {sig: signal.signal(sig, interrupt) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    try:
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


@contextmanager
def exclusive_lock():
    fd = os.open(LOCK, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
            raise DiagnosticError(f"Unsafe lock file: {LOCK}")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in (errno.EAGAIN, errno.EACCES):
                raise DiagnosticError("Another Medion diagnostic already holds the lock") from error
            raise
        yield
    finally:
        os.close(fd)


def assert_no_fprintd():
    for entry in PROC.glob("[0-9]*"):
        try:
            name = (entry / "comm").read_text().strip()
        except FileNotFoundError:
            continue
        except OSError as error:
            raise DiagnosticError(f"Cannot inspect {entry}/comm: {error}") from error
        if name == "fprintd":
            raise DiagnosticError("A fprintd process is still running; close its clients before retrying")


class Session:
    def __init__(self, resources):
        self.resources = resources
        self.original_binding = binding(resources.spi)
        if self.original_binding not in (None, "spidev"):
            raise DiagnosticError(f"Target is bound to {self.original_binding}; refusing to unbind another driver. "
                                  "Review and release that binding manually first.")
        self.original_override = attribute(resources.spi / "driver_override")
        if self.original_override == "(null)":
            self.original_override = ""
        if "\n" in self.original_override or "\r" in self.original_override:
            raise DiagnosticError("Unexpected multiline driver_override")
        self.transport_started = False
        self.override_changed = False
        self.mask_added = False
        self.service_stopped = False
        self.service_was_active = False

    def stop_service(self):
        load = command(["systemctl", "show", "--property=LoadState", "--value", SERVICE]).stdout.strip()
        if load == "not-found":
            assert_no_fprintd()
            return
        if load not in ("loaded", "masked"):
            raise DiagnosticError(f"Unexpected fprintd service state: {load!r}")
        enabled = command(["systemctl", "is-enabled", SERVICE], check=False)
        allowed = {"enabled", "enabled-runtime", "disabled", "static", "indirect", "generated",
                   "transient", "alias", "linked", "linked-runtime", "masked", "masked-runtime"}
        mask_state = enabled.stdout.strip()
        if enabled.returncode not in (0, 1) or mask_state not in allowed:
            raise DiagnosticError("Cannot determine the existing fprintd mask state")
        active = command(["systemctl", "is-active", "--quiet", SERVICE], check=False)
        if active.returncode not in (0, 3):
            raise DiagnosticError("Cannot determine whether fprintd is active")
        self.service_was_active = active.returncode == 0
        if mask_state in ("masked", "masked-runtime"):
            if self.service_was_active:
                raise DiagnosticError("An active, already masked fprintd cannot be safely restored automatically")
        else:
            self.mask_added = True
            command(["systemctl", "mask", "--runtime", SERVICE])
            masked = command(["systemctl", "is-enabled", SERVICE], check=False)
            if masked.returncode not in (0, 1) or masked.stdout.strip() not in ("masked", "masked-runtime"):
                raise DiagnosticError("fprintd runtime mask could not be verified")
        self.service_stopped = True
        command(["systemctl", "stop", SERVICE])
        active = command(["systemctl", "is-active", "--quiet", SERVICE], check=False)
        if active.returncode != 3:
            raise DiagnosticError("fprintd did not reach an inactive state")
        assert_no_fprintd()

    def prepare_transport(self):
        self.transport_started = True
        module = SYS / "module/spidev"
        if not module.exists():
            command(["modprobe", "spidev", f"bufsiz={BUFFER_SIZE}"])
        value = attribute(module / "parameters/bufsiz")
        if not value.isdecimal() or int(value) < MIN_PROBE_BUFFER:
            raise DiagnosticError(f"Loaded spidev bufsiz is {value!r}; at least {MIN_PROBE_BUFFER} is required for probing")
        print(f"[binding] Actual spidev bufsiz: {value} bytes", flush=True)
        if int(value) < BUFFER_SIZE:
            print("[binding] Buffer is below 32768; the diagnostic will check each chip's actual transfer needs. "
                  "A confirmed identity can precede a transfer-limit failure. This launcher will not unload "
                  "the global driver; a larger buffer requires a separately planned reload or boot setting.", flush=True)
        current = binding(self.resources.spi)
        if current not in (None, "spidev"):
            raise DiagnosticError(f"Target binding changed to {current}; refusing to replace it")
        if current is None:
            self.override_changed = True
            write_sysfs(self.resources.spi / "driver_override", "spidev")
            try:
                write_sysfs(SYS / "bus/spi/drivers/spidev/bind", self.resources.spi.name)
            except DiagnosticError as error:
                raise DiagnosticError(f"spidev could not bind the ACPI device; this kernel may reject this override: {error}") from error
        if binding(self.resources.spi) != "spidev":
            raise DiagnosticError("spidev binding was not established")
        device = spidev_node(self.resources.spi)
        wait_for_node(device)
        verify_node(self.resources.reset)
        verify_node(self.resources.irq)
        return device

    def restore(self):
        errors = []

        def attempt(operation):
            try:
                operation()
            except (DiagnosticError, OSError, subprocess.SubprocessError) as error:
                errors.append(str(error))

        if self.transport_started and self.original_binding is None:
            def unbind():
                current = binding(self.resources.spi)
                if current == "spidev":
                    write_sysfs(SYS / "bus/spi/drivers/spidev/unbind", self.resources.spi.name)
                elif current is not None:
                    raise DiagnosticError(f"Unexpected driver {current} during restoration; left untouched")
                if binding(self.resources.spi) is not None:
                    raise DiagnosticError("Temporary spidev binding could not be removed")
            attempt(unbind)
        if self.override_changed:
            def restore_override():
                write_sysfs(self.resources.spi / "driver_override", self.original_override)
                restored = attribute(self.resources.spi / "driver_override")
                if restored not in (self.original_override, "(null)" if not self.original_override else self.original_override):
                    raise DiagnosticError("Original driver_override was not restored")
            attempt(restore_override)
        if self.mask_added:
            attempt(lambda: command(["systemctl", "unmask", "--runtime", SERVICE]))
        if self.service_stopped and self.service_was_active:
            attempt(lambda: command(["systemctl", "start", SERVICE]))
        return errors


def execute(resources, tool, action, output=None, chip=None, firmware=None):
    session = Session(resources)
    error = None
    try:
        print("[service] Temporarily excluding fprintd", flush=True)
        session.stop_service()
        # Recheck topology after excluding the daemon and before any binding.
        if discover() != resources:
            raise DiagnosticError("Hardware resources changed during preparation")
        print("[binding] Preparing system spidev, fixed chip select polarity", flush=True)
        device = session.prepare_transport()
        arguments = [str(tool), "--device", str(device.path),
                     "--reset-chip", str(resources.reset.path), "--irq-chip", str(resources.irq.path),
                     "--action", action]
        if output is not None:
            arguments += ["--output", str(output)]
        if chip is not None:
            arguments += ["--chip", chip]
        if firmware is not None:
            arguments += ["--firmware", str(firmware)]
        if action == "boot":
            print(f"[candidate] Explicit {chip.upper()} RAM boot requested; "
                  "the diagnostic must validate its firmware and application response.", flush=True)
        print(f"[diagnostic] Starting {action}; protocol stages follow", flush=True)
        command(arguments, inherit=True)
    except BaseException as caught:
        error = caught
    finally:
        # A second signal must not interrupt restoration halfway through.
        print("[restore] Restoring original binding and service state", flush=True)
        with ignore_cleanup_signals():
            cleanup_errors = session.restore()
    if cleanup_errors:
        failure = DiagnosticError((f"{error}; " if error else "") + "Restoration failed: " + "; ".join(cleanup_errors))
        if isinstance(error, Interrupted):
            failure.exit_code = error.exit_code
        raise failure from error
    if error is not None:
        raise error
    print("[done] Diagnostic completed; original binding, override and service state restored", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--inspect", action="store_true", help="Read-only resource inventory (default)")
    actions.add_argument("--identify-legacy", action="store_true",
                         help="Check FT9338/FT9348 application and ROM identity without firmware upload or capture")
    actions.add_argument("--boot", choices=("ft9338", "ft9348"),
                         help="Explicitly boot the selected candidate's verified RAM firmware, without capture")
    actions.add_argument("--probe", action="store_true", help="Run chip identification")
    actions.add_argument("--init", action="store_true", help="Identify and initialize the sensor")
    actions.add_argument("--capture", metavar="OUTPUT", type=Path, help="Capture one frame to a new local file")
    parser.add_argument("--tool", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build-medion/examples/fte3600-medion",
                        help="Path to the separately built fte3600-medion diagnostic")
    parser.add_argument("--firmware", metavar="PATH", type=Path,
                        help="Custom firmware file for --boot; otherwise use the selected chip's catalog path")
    args = parser.parse_args(argv)
    try:
        action = ("boot" if args.boot is not None else "identify-legacy" if args.identify_legacy
                  else "capture" if args.capture is not None
                  else "init" if args.init else "probe" if args.probe else None)
        firmware = None
        if args.firmware is not None:
            if action != "boot":
                raise DiagnosticError("--firmware requires --boot ft9338 or --boot ft9348")
            # The C loader checks the opened regular file and verifies a bounded
            # snapshot's size and catalog hash before sensor I/O. This pathname
            # check is only early input validation; symlinks to files are valid.
            firmware = Path(os.path.abspath(args.firmware))
            if not stat.S_ISREG(firmware.stat().st_mode):
                raise DiagnosticError(f"Firmware must be a regular file: {firmware}")
        resources = discover()
        print(f"[resources] ACPI {SPI_ACPI}: {resources.spi.name} on {resources.controller.name}", flush=True)
        print(f"[resources] reset {RESET_ACPI} INT3453: {resources.reset.path}, line 39, active-low", flush=True)
        print(f"[resources] IRQ {IRQ_ACPI} INT3453: {resources.irq.path}, line 0, rising edge", flush=True)
        current = binding(resources.spi)
        print(f"[resources] Current driver: {current or 'unbound'}", flush=True)
        if action is None:
            device = spidev_node(resources.spi, required=False)
            print(f"[resources] spidev: {device.path if device else 'not bound; execution will attempt a temporary binding'}")
            return 0
        if os.geteuid() != 0:
            raise DiagnosticError("Execution requires root; --inspect remains read-only and needs no root")
        tool = canonical(args.tool)
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise DiagnosticError(f"Diagnostic tool is not executable: {tool}")
        output = None
        if args.capture is not None:
            output = canonical(args.capture.parent) / args.capture.name
            if os.path.lexists(output):
                raise DiagnosticError(f"Capture destination already exists: {output}")
        with exclusive_lock(), signal_cleanup():
            execute(resources, tool, action, output, chip=args.boot, firmware=firmware)
        return 0
    except (DiagnosticError, OSError, subprocess.SubprocessError) as error:
        print(f"[error] {error}", file=sys.stderr)
        return getattr(error, "exit_code", 1)


if __name__ == "__main__":
    sys.exit(main())
