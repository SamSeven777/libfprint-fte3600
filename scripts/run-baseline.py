#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Temporary Medion-only binding + isolated archived library open/close."""
import argparse
from contextlib import contextmanager
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MODULE = "focal_medion_baseline"
DRIVER = "focal-medion-baseline"
DEVICE = "spi-FTE3600:00"
LIB_SHA = "4ee33eb988a62413698d1761b9c9baf09faca6a645f82ac19db6f33d04c8cb1b"
SAFE_ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C", "LC_ALL": "C"}
SOURCES = ("reference/ctfdavis-focal_spi.c", "module/Makefile", "module/focal_medion_baseline.c",
           "tools/baseline-client.c", "scripts/prepare.py", "scripts/run-baseline.py")

@contextmanager
def protect_cleanup():
    previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        yield
    finally:
        for sig, handler in previous.items(): signal.signal(sig, handler)

@contextmanager
def prevent_sleep():
    """Hold a systemd inhibitor, confirming acquisition before hardware changes."""
    proc = subprocess.Popen(["systemd-inhibit", "--what=sleep", "--mode=block",
                             "--who=Medion baseline", "--why=Temporary SPI initialization",
                             sys.executable, "-I", "-c",
                             "import sys; print('READY', flush=True); sys.stdin.read()"],
                            env=SAFE_ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, start_new_session=True)
    selector = selectors.DefaultSelector()
    try:
        selector.register(proc.stdout, selectors.EVENT_READ)
        if not selector.select(10) or os.read(proc.stdout.fileno(), 64) != b"READY\n":
            raise RuntimeError("Could not inhibit system sleep; hardware was not touched")
        yield
    finally:
        selector.close()
        with protect_cleanup():
            proc.stdin.close()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=5)
            proc.stdout.close()

def command(args, check=True, timeout=20):
    result = subprocess.run(args, env=SAFE_ENV, check=False, timeout=timeout,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode:
        raise RuntimeError(str(args[0]) + " failed (" + str(result.returncode) + "): " + result.stderr.strip()[:1000])
    return result

def write(path, value):
    with path.open("w") as stream:
        stream.write(value + "\n")

def driver_name(sensor):
    link = sensor / "driver"
    return link.resolve().name if link.is_symlink() else None

def sandbox_command(stage, action, sensor=False):
    cmd = ["bwrap", "--die-with-parent", "--unshare-all", "--new-session",
           "--cap-drop", "ALL", "--clearenv", "--setenv", "PATH", "/usr/bin",
           "--setenv", "LANG", "C", "--setenv", "HOME", "/tmp",
           "--ro-bind", "/usr", "/usr", "--proc", "/proc", "--dev", "/dev",
           "--tmpfs", "/tmp", "--dir", "/run", "--dir", "/etc",
           "--ro-bind", str(stage), "/work", "--chdir", "/tmp"]
    for path in ("/lib", "/lib64"):
        if Path(path).exists():
            cmd += ["--ro-bind", path, path]
    if Path("/etc/ld.so.cache").exists():
        cmd += ["--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"]
    if sensor:
        cmd += ["--ro-bind", "/sys", "/sys", "--dev-bind", "/dev/focal_moh_spi", "/dev/focal_moh_spi"]
        if Path("/run/udev").exists():
            cmd += ["--ro-bind", "/run/udev", "/run/udev"]
    cmd += ["/work/baseline-client", "/work/libfprint-legacy.so", action]
    return cmd

def isolated_run(stage, action, timeout, sensor=False):
    """Bound runtime and event output; discard vendor stdout/stderr in the C client."""
    proc = subprocess.Popen(sandbox_command(stage, action, sensor), env=SAFE_ENV,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            start_new_session=True)
    data = bytearray()
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    try:
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("Archived library timed out")
            ready = selector.select(min(0.2, max(0, deadline - time.monotonic())))
            if ready:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                data.extend(chunk)
                if len(data) > 16384:
                    raise RuntimeError("Unexpected excessive event output")
            elif proc.poll() is not None:
                break
        rc = proc.wait(timeout=max(1, deadline - time.monotonic()))
    finally:
        selector.close()
        with protect_cleanup():
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait(timeout=5)
            proc.stdout.close()
            lines = data.decode("ascii", errors="replace").splitlines()
            events = [s for s in lines if re.fullmatch(r"MEDION_BASELINE:[A-Z_]+(?: code=-?\d+)?", s)]
            for event in events: print(event, flush=True)
    if action == "--check-abi" and (rc or "MEDION_BASELINE:ABI_OK" not in events):
        raise RuntimeError("Isolated ABI check failed: missing dependency, library ABI or namespace permission; hardware was not touched")
    if action == "--open" and (rc or not all("MEDION_BASELINE:" + x in events for x in ("OPEN_OK", "CLOSE_OK"))):
        raise RuntimeError("Archived initialization did not complete; see bounded events")

class Session:
    """Order and rollback are independently tested with a fake host."""
    def __init__(self, host):
        self.host = host

    def run(self):
        paused = powered = rebound = False
        try:
            paused = True
            self.host.pause()
            powered = True
            self.host.power_on()
            rebound = True
            self.host.bind()
            self.host.open_library()
        finally:
            with protect_cleanup():
                errors = []
                if rebound:
                    try: self.host.restore_binding()
                    except Exception as exc: errors.append("binding: " + str(exc))
                if powered:
                    try: self.host.restore_power()
                    except Exception as exc: errors.append("power: " + str(exc))
                # Do not restart authentication while a temporary driver may remain.
                if paused and not errors:
                    try: self.host.resume()
                    except Exception as exc: errors.append("service: " + str(exc))
                if errors:
                    raise RuntimeError("RESTORE INCOMPLETE; keep password login, reboot before further fingerprint use: " + "; ".join(errors))

class Host:
    def __init__(self, stage, timeout):
        self.stage, self.timeout = stage, timeout
        self.sensor = Path("/sys/bus/spi/devices") / DEVICE
        self.controls = []
        self.created_mask = False
        self.bind_changed = False
        self.kmsg = None
        self.trace = []

    def preflight(self):
        expected = {"sys_vendor": "MEDION", "product_name": "E3224", "product_version": "FT", "board_name": "YS13G"}
        for key, value in expected.items():
            if (Path("/sys/class/dmi/id") / key).read_text().strip() != value:
                raise RuntimeError("Not the MEDION/E3224/FT/YS13G profile; refusing")
        actual = self.sensor.resolve(strict=True)
        if not actual.is_relative_to("/sys/devices") or "0000:00:19.0" not in actual.parts:
            raise RuntimeError("Unexpected SPI topology")
        if not self.sensor.joinpath("modalias").read_text().strip().startswith("acpi:FTE3600:"):
            raise RuntimeError("Unexpected modalias")
        self.original_driver = driver_name(self.sensor)
        if self.original_driver not in (None, "spidev"):
            raise RuntimeError("Another driver is bound; refusing to replace it")
        if Path("/dev/focal_moh_spi").exists() or Path("/sys/module/" + MODULE).exists():
            raise RuntimeError("An old/baseline driver is already present; refusing")
        lock = Path("/sys/kernel/security/lockdown")
        if lock.exists() and "[none]" not in lock.read_text():
            raise RuntimeError("Kernel lockdown active; unsigned module refused. Do not disable Secure Boot for this test")
        if Path("/sys/power/autosleep").exists() and Path("/sys/power/autosleep").read_text().strip() != "off":
            raise RuntimeError("Automatic system sleep enabled; refusing")
        self.override = (self.sensor / "driver_override").read_text().strip()
        if self.override not in ("", "(null)", "spidev"):
            raise RuntimeError("Unexpected driver_override; refusing")
        for ancestor in reversed([actual, *actual.parents]):
            control = ancestor / "power/control"
            if ancestor.is_relative_to("/sys/devices") and control.is_file():
                value = control.read_text().strip()
                if value not in ("on", "auto"):
                    raise RuntimeError("Unexpected power-control value")
                self.controls.append((control, value))
        info = command(["systemctl", "show", "fprintd.service", "-p", "LoadState", "-p", "ActiveState", "-p", "UnitFileState"]).stdout
        self.service = dict(s.split("=", 1) for s in info.splitlines() if "=" in s)
        if self.service.get("ActiveState") not in ("active", "inactive", "failed"):
            raise RuntimeError("fprintd is transitioning; try later")
        self.mask = Path("/run/systemd/system/fprintd.service")
        if self.mask.exists() or self.mask.is_symlink():
            if not self.mask.is_symlink() or self.mask.resolve() != Path("/dev/null"):
                raise RuntimeError("Existing runtime service override; refusing to change it")
        if self.service.get("ActiveState") == "active" and (
                self.service.get("UnitFileState") in ("masked", "masked-runtime") or self.mask.is_symlink()):
            raise RuntimeError("fprintd is running but already masked; cannot restore it without changing your existing mask")
        state = {"sensor": str(actual), "driver": self.original_driver, "override": self.override,
                 "power": [(str(p), v) for p, v in self.controls], "service": self.service}
        (self.stage / "restore-state.json").write_text(json.dumps(state, indent=2) + "\n")
        self.kmsg = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        os.lseek(self.kmsg, 0, os.SEEK_END)
        print("Profile and saved state verified. Do not suspend or close the lid during the test.", flush=True)

    def pause(self):
        if self.service.get("LoadState") == "not-found": return
        if self.service.get("UnitFileState") not in ("masked", "masked-runtime") and not self.mask.is_symlink():
            # Record ownership before the command, including partial command failures.
            self.created_mask = True
            command(["systemctl", "mask", "--runtime", "fprintd.service"])
        command(["systemctl", "stop", "fprintd.service"])
        if command(["systemctl", "is-active", "fprintd.service"], check=False).stdout.strip() not in ("inactive", "failed"):
            raise RuntimeError("fprintd did not stop")

    def power_on(self):
        for path, value in self.controls: write(path, "on")
        controller = next(p for p in self.sensor.resolve().parents if p.name == "0000:00:19.0")
        for _ in range(30):
            if (controller / "power/runtime_status").read_text().strip() == "active": return
            time.sleep(0.1)
        raise RuntimeError("LPSS controller did not become active")

    def bind(self):
        self.ensure_unused()
        self.bind_changed = True
        write(self.sensor / "driver_override", DRIVER)
        if self.original_driver:
            write(self.sensor / "driver/unbind", DEVICE)
        command(["insmod", str(self.stage / (MODULE + ".ko"))])
        if driver_name(self.sensor) is None:
            write(Path("/sys/bus/spi/drivers") / DRIVER / "bind", DEVICE)
        if driver_name(self.sensor) != DRIVER:
            raise RuntimeError("Baseline driver did not bind")
        command(["udevadm", "settle", "--timeout=5"], timeout=10)
        for _ in range(30):
            node = Path("/dev/focal_moh_spi")
            if node.exists() and stat.S_ISCHR(node.stat().st_mode):
                info = node.stat()
                if info.st_uid or info.st_mode & 0o077:
                    raise RuntimeError("A local udev rule relaxed baseline device ownership/permissions; refusing")
                major, minor = map(int, Path("/sys/class/misc/focal_moh_spi/dev").read_text().strip().split(":"))
                if info.st_rdev != os.makedev(major, minor):
                    raise RuntimeError("Baseline node does not match its misc device")
                return
            time.sleep(0.1)
        raise RuntimeError("Baseline device node missing")

    def ensure_unused(self):
        numbers = set()
        for entry in Path("/sys/class/spidev").glob("*"):
            if entry.resolve().is_relative_to(self.sensor.resolve()):
                major, minor = map(int, (entry / "dev").read_text().strip().split(":"))
                numbers.add(os.makedev(major, minor))
        if not numbers: return
        for process in Path("/proc").glob("[0-9]*"):
            try:
                for fd in (process / "fd").iterdir():
                    try: info = fd.stat()
                    except OSError as exc:
                        if exc.errno in (errno.ENOENT, errno.ESRCH): continue
                        raise
                    if stat.S_ISCHR(info.st_mode) and info.st_rdev in numbers:
                        raise RuntimeError("SPI device is still open by PID " + process.name + "; close that program first")
            except OSError as exc:
                if exc.errno not in (errno.ENOENT, errno.ESRCH): raise

    def open_library(self):
        isolated_run(self.stage, "--open", self.timeout, sensor=True)

    def restore_binding(self):
        if not self.bind_changed: return
        if Path("/sys/module/" + MODULE).exists():
            command(["rmmod", MODULE])  # never force; an open fd must prevent unload
        if driver_name(self.sensor) not in (None, self.original_driver):
            raise RuntimeError("Unexpected driver during restore")
        write(self.sensor / "driver_override", "" if self.override == "(null)" else self.override)
        if self.original_driver and driver_name(self.sensor) is None:
            write(Path("/sys/bus/spi/drivers") / self.original_driver / "bind", DEVICE)
        if driver_name(self.sensor) != self.original_driver:
            raise RuntimeError("Original SPI binding not restored")
        accepted = {"", "(null)"} if self.override in ("", "(null)") else {self.override}
        if (self.sensor / "driver_override").read_text().strip() not in accepted:
            raise RuntimeError("Original driver override not restored")
        self.collect_trace()
        if any(re.search(r"SPI_RESTORE rc=-[0-9]+", line) for line in self.trace):
            raise RuntimeError("Kernel could not restore original SPI settings")

    def restore_power(self):
        failures = []
        for path, value in reversed(self.controls):
            try:
                write(path, value)
                if path.read_text().strip() != value: raise RuntimeError("readback mismatch")
            except Exception as exc: failures.append(str(path) + ": " + str(exc))
        if failures: raise RuntimeError("; ".join(failures))

    def resume(self):
        if self.created_mask:
            if self.mask.is_symlink() and self.mask.resolve() == Path("/dev/null"):
                command(["systemctl", "unmask", "--runtime", "fprintd.service"])
            elif self.mask.exists() or self.mask.is_symlink():
                raise RuntimeError("Runtime mask changed externally; not removing it")
        if self.service.get("ActiveState") == "active":
            command(["systemctl", "start", "fprintd.service"])

    def collect_trace(self):
        if self.kmsg is None: return
        for _ in range(4096):
            try: line = os.read(self.kmsg, 16384).decode(errors="replace")
            except BlockingIOError: return
            except OSError as exc:
                raise RuntimeError("Kernel trace lost; cannot confirm SPI restoration") from exc
            if "MEDION_BASELINE:" in line:
                if len(self.trace) >= 1024: raise RuntimeError("Excessive kernel events")
                self.trace.append(line[line.index("MEDION_BASELINE:"):].splitlines()[0])
        raise RuntimeError("Kernel trace limit exceeded")

    def print_trace(self):
        if self.kmsg is None: return
        try:
            self.collect_trace()
        finally:
            for line in self.trace: print(line, flush=True)
            os.close(self.kmsg)
            self.kmsg = None

def stage_artifacts():
    build = ROOT / ".baseline/build"
    manifest = json.loads((build / "manifest.json").read_text())
    if manifest.get("format") != 1 or manifest.get("library_sha256") != LIB_SHA:
        raise RuntimeError("Invalid build manifest; prepare again")
    if manifest["vermagic"].split()[0] != os.uname().release:
        raise RuntimeError("Module was not built for the running kernel; prepare again")
    for name in SOURCES:
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != manifest.get("sources", {}).get(name):
            raise RuntimeError("Sources changed since build; prepare again")
    stage = Path(tempfile.mkdtemp(prefix="medion-baseline-", dir="/run"))
    for name in ("baseline-client", "libfprint-legacy.so", MODULE + ".ko"):
        # Copy once into a root-private directory; hashes apply to copied bytes.
        source = build / name
        if source.is_symlink() or not source.is_file(): raise RuntimeError("Invalid build artifact")
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != manifest["files"][name]:
            raise RuntimeError("Build artifact hash mismatch")
        if name == "libfprint-legacy.so" and hashlib.sha256(data).hexdigest() != LIB_SHA:
            raise RuntimeError("Unrecognized library")
        target = stage / name
        target.write_bytes(data)
        target.chmod(0o500 if name == "baseline-client" else 0o400)
    actual = command(["modinfo", "-F", "vermagic", str(stage / (MODULE + ".ko"))]).stdout.strip()
    if actual != manifest["vermagic"]: raise RuntimeError("Module metadata mismatch")
    return stage

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="Explicitly permit transient hardware initialization")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if not args.run or os.geteuid() != 0 or not 10 <= args.timeout <= 300:
        parser.error("Use sudo and --run; timeout must be 10..300 seconds")
    for tool in ("bwrap", "systemctl", "systemd-inhibit", "udevadm", "insmod", "rmmod", "modinfo"):
        if not shutil.which(tool, path=SAFE_ENV["PATH"]): parser.error("Missing dependency: " + tool)
    os.umask(0o077)
    lock_fd = os.open("/run/medion-baseline.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    def interrupted(signum, frame): raise KeyboardInterrupt("signal " + str(signum))
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    stage = stage_artifacts()
    print("Private artifacts and restoration record:", stage, flush=True)
    host = Host(stage, args.timeout)
    try:
        isolated_run(stage, "--check-abi", 20)
        host.preflight()
        with prevent_sleep():
            Session(host).run()
        print("Initialization and normal-exit restoration completed. Enrollment was not tested.")
    finally:
        host.print_trace()
        os.close(lock_fd)
    # Deliberately keep root-private state in /run until reboot for recovery/audit.

if __name__ == "__main__":
    try: main()
    except (Exception, KeyboardInterrupt) as error:
        print("BASELINE STOPPED:", error, flush=True)
        raise SystemExit(1)
