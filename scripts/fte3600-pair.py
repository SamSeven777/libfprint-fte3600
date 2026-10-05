#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate the ACPI glue's reset GPIO, IRQ-only UIO and stock-spidev companions.

No bind, GPIO line request, transfer, reset or policy installation. Label
operations pin validated device inodes; GPIO validation queries chip info,
and UIO is never opened to acquire its IRQ lease.
--refresh-spi only requests a udev change event for a validated companion node.
Numbered paths are observations, never hardware identity. No environment root override
is accepted by the installed helper.
"""
import argparse
import contextlib
import errno
import fcntl
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys

SYS = Path("/sys")
DEV = Path("/dev")
GPIO_GET_CHIPINFO_IOCTL = 0x8044B401
LABEL_TYPES = {"spi": "fte3600_spidev_t", "gpio": "fte3600_gpio_t", "irq": "fte3600_irq_t"}
LABEL_ATTRIBUTE = "security.selinux"


def read(path):
    value = path.read_text(encoding="ascii").strip()
    if len(value) > 256:
        raise ValueError(f"oversized sysfs attribute: {path}")
    return value


def number(path, maximum):
    value = read(path)
    if not re.fullmatch(r"[0-9]+", value) or int(value) > maximum:
        raise ValueError(f"invalid numeric attribute: {path}")
    return int(value)


def resolve(path):
    result = path.resolve(strict=True)
    result.relative_to(SYS.resolve())
    return result


def spi_identity(spi):
    if resolve(spi / "driver").name != "spidev":
        raise ValueError("SPI device is not bound to stock spidev")
    acpi = resolve(spi / "firmware_node")
    if not re.fullmatch(r"FTE3600:[a-zA-Z0-9]+", acpi.name):
        raise ValueError("unexpected ACPI instance name")
    if read(acpi / "hid") != "FTE3600":
        raise ValueError("wrong ACPI HID")
    return acpi.name


def metadata(glue, ready):
    glue = resolve(glue)
    if resolve(glue / "driver").name != "fte3600-glue":
        raise ValueError("platform device is not bound to fte3600-glue")
    spi = glue.parent
    instance = spi_identity(spi)
    if read(glue / "fte3600_glue_abi") != "2":
        raise ValueError("unsupported ACPI glue ABI")
    generation = number(glue / "fte3600_generation", (1 << 64) - 1)
    status = read(glue / "fte3600_status")
    if status not in (("ready",) if ready else ("ready", "suspended")):
        raise ValueError("ACPI glue is not ready")
    if number(glue / "fte3600_ngpio", 1) != 1:
        raise ValueError("glue must advertise exactly one reset GPIO line")
    if number(glue / "fte3600_acpi_mode", 4) not in (0, 4):
        raise ValueError("unsupported ACPI SPI clock mode")
    if not number(glue / "fte3600_acpi_speed_hz", (1 << 32) - 1):
        raise ValueError("invalid ACPI SPI speed")
    number(glue / "fte3600_irq_active_low", 1)
    number(glue / "fte3600_cs_control", 1)
    if read(glue / "fte3600_irq_source") not in ("gpio", "acpi"):
        raise ValueError("unsupported ACPI interrupt source")
    if number(glue / "fte3600_generation", (1 << 64) - 1) != generation:
        raise ValueError("glue generation changed while reading metadata")
    return spi, generation, instance


def device_path(node):
    name = node.name
    if not re.fullmatch(r"gpiochip[0-9]+|uio[0-9]+|spidev[0-9]+\.[0-9]+", name):
        raise ValueError("unexpected character device name")
    devnum = read(node / "dev")
    if not re.fullmatch(r"[0-9]+:[0-9]+", devnum):
        raise ValueError("invalid sysfs device number")
    major, minor = map(int, devnum.split(":"))
    path = DEV / name
    info = path.lstat()
    if not stat.S_ISCHR(info.st_mode) or (os.major(info.st_rdev), os.minor(info.st_rdev)) != (major, minor):
        raise ValueError(f"device number/type mismatch: {path}")
    return path, info


def character_node(node, gpio=False, irq=False):
    pattern = r"gpiochip[0-9]+" if gpio else r"uio[0-9]+" if irq else r"spidev[0-9]+\.[0-9]+"
    if not re.fullmatch(pattern, node.name):
        raise ValueError("unexpected companion role")
    path, info = device_path(node)
    if gpio:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW)
        try:
            opened = os.fstat(fd)
            if not stat.S_ISCHR(opened.st_mode) or opened.st_rdev != info.st_rdev:
                raise ValueError("GPIO node changed while opening")
            chip = bytearray(68)
            fcntl.ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, chip, True)
            if struct.unpack_from("=I", chip, 64)[0] != 1:
                raise ValueError("glue GPIO chip must expose exactly one reset line")
        finally:
            os.close(fd)
    # Never open UIO here: doing so would acquire the IRQ/reset lease. Its
    # kernel name and no-mapping contract are checked through sysfs instead.
    if irq:
        if read(node / "name") != "fte3600-irq" or read(node / "version") != "2":
            raise ValueError("wrong IRQ-only UIO identity")
        if (node / "maps").exists() or (node / "portio").exists():
            raise ValueError("FTE3600 UIO must not expose memory or port mappings")
    return str(path)


@contextlib.contextmanager
def pinned_node(node):
    path, before = device_path(node)
    # O_PATH never calls a character driver's open method (notably UIO's lease).
    fd = os.open(path, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        opened = os.fstat(fd)
        if not stat.S_ISCHR(opened.st_mode) or (opened.st_dev, opened.st_ino, opened.st_rdev) != \
                (before.st_dev, before.st_ino, before.st_rdev):
            raise ValueError("device inode changed while pinning its label")
        yield path, f"/proc/self/fd/{fd}"
    finally:
        os.close(fd)


def get_label(pinned):
    return os.getxattr(pinned, LABEL_ATTRIBUTE).rstrip(b"\0").decode("ascii")


def set_label(pinned, context):
    if not re.fullmatch(r"[A-Za-z0-9_]+:object_r:[A-Za-z0-9_]+:[A-Za-z0-9_,:.-]+", context):
        raise ValueError("invalid SELinux object context")
    os.setxattr(pinned, LABEL_ATTRIBUTE, context.encode("ascii") + b"\0")
    if get_label(pinned) != context:
        raise ValueError("SELinux label did not match after setting it")


def role_for_node(node):
    return "spi" if node.name.startswith("spidev") else "gpio" if node.name.startswith("gpiochip") else "irq"


def relabel(node):
    role = role_for_node(node)
    glue, path, _ = identify_node(node, role, False)
    generation = metadata(glue, False)[1]
    with pinned_node(node) as (pinned_path, pinned):
        if str(pinned_path) != path or metadata(glue, False)[1] != generation:
            raise ValueError("companion changed before labeling")
        set_label(pinned, f"system_u:object_r:{LABEL_TYPES[role]}:s0")
        if metadata(glue, False)[1] != generation:
            raise ValueError("companion changed during labeling")


def restore_labels():
    # Restore only our exact types. A still-bound physical SPI can survive glue
    # removal, so it must not depend on a complete three-node pair or aliases.
    for node in nodes(("class/spidev/spidev*", "bus/gpio/devices/gpiochip*", "class/uio/uio*")):
        with pinned_node(node) as (path, pinned):
            try:
                context = get_label(pinned)
            except OSError as error:
                if error.errno == errno.ENODATA:
                    continue
                raise
            role = role_for_node(node)
            current_type = context.split(":")[2:3]
            if current_type not in ([value] for value in LABEL_TYPES.values()):
                continue
            if current_type != [LABEL_TYPES[role]]:
                raise ValueError(f"FTE3600 label does not match the device role: {path}")
            if role == "spi":
                spi_identity(spi_parent(node))
            else:
                identify_node(node, role, False)
            default = subprocess.run(["matchpathcon", "-n", str(path)], check=True,
                                     capture_output=True, text=True, timeout=10).stdout.strip()
            if default.split(":")[2:3] in ([value] for value in LABEL_TYPES.values()):
                raise ValueError(f"default context still depends on FTE3600 policy: {path}")
            set_label(pinned, default)


def gpio_parent(node):
    # The GPIO cdev is a direct child of the platform glue. Older sysfs class
    # views also expose that parent through the standard device symlink.
    parent = node / "device"
    return resolve(parent) if parent.exists() else resolve(node).parent


def spi_parent(node):
    link = node / "device"
    if link.exists():
        return resolve(link)
    # The class directory may sit between the cdev and its physical parent:
    # <SPI device>/spidev/spidevN.M. It is not itself the SPI device.
    parent = resolve(node).parent
    return parent.parent if parent.name == "spidev" else parent


def nodes(patterns):
    found = {}
    for pattern in patterns:
        for path in SYS.glob(pattern):
            real = resolve(path)
            if (real / "dev").exists():
                found[real] = real
    return sorted(found)


def identify_node(node, role, ready):
    if role == "spi":
        parent = spi_parent(node)
        candidates = [child for child in parent.iterdir()
                      if (child / "fte3600_glue_abi").is_file()]
        if len(candidates) != 1:
            raise ValueError("SPI device requires exactly one platform glue")
        glue = candidates[0]
    elif role == "gpio":
        glue = gpio_parent(node)
    elif role == "irq":
        glue = resolve(node / "device")
    else:
        raise ValueError("unknown companion role")
    _, generation, instance = metadata(glue, ready)
    path = character_node(node, gpio=role == "gpio", irq=role == "irq")
    if metadata(glue, ready)[1] != generation:
        raise ValueError("ACPI glue generation changed during validation")
    return resolve(glue), path, instance


def pairs():
    spi_nodes = nodes(("class/spidev/spidev*",))
    gpio_nodes = nodes(("bus/gpio/devices/gpiochip*", "class/gpio/gpiochip*"))
    irq_nodes = nodes(("class/uio/uio*",))
    result = []
    for glue_link in sorted(SYS.glob("bus/platform/devices/*")):
        # An advertised glue must be valid; a partially removed/suspended pair
        # may not silently leave an old service permission configuration active.
        if not (glue_link / "fte3600_glue_abi").exists():
            continue
        glue = resolve(glue_link)
        spi, generation, instance = metadata(glue, True)
        matched = {"spi": [], "gpio": [], "irq": []}
        for role, candidates in (("spi", spi_nodes), ("gpio", gpio_nodes), ("irq", irq_nodes)):
            for node in candidates:
                try:
                    if role == "spi":
                        candidate_parent = spi_parent(node)
                    elif role == "irq":
                        candidate_parent = node / "device" if (node / "device").exists() else node.parent
                    else:
                        candidate_parent = gpio_parent(node)
                    if resolve(candidate_parent) != (spi if role == "spi" else glue):
                        continue
                except FileNotFoundError:
                    continue
                _, path, _ = identify_node(node, role, True)
                matched[role].append(path)
        if any(len(companions) != 1 for companions in matched.values()):
            raise ValueError(f"expected exactly one spidev, reset GPIO and IRQ-only UIO companion for {instance}")
        if metadata(glue, True)[1] != generation:
            raise ValueError("ACPI glue generation changed during pairing")
        for role in ("spi", "gpio", "irq"):
            path = matched[role][0]
            alias = DEV / f"fte3600-{role}-{instance}"
            if not alias.is_symlink() or alias.resolve(strict=True) != Path(path):
                raise ValueError(f"missing or incorrect udev companion link: {alias}")
            result.append((role, path, str(alias)))
    if not result:
        raise ValueError("no complete, ready FTE3600 ACPI glue/spidev pair")
    if len({path for _, path, _ in result}) != len(result):
        raise ValueError("a character device belongs to more than one pair")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--udev", metavar="DEVPATH", help="validate one add/change event independently")
    action.add_argument("--refresh-spi", metavar="DEVPATH", help="request only the verified spidev companion's change event")
    action.add_argument("--relabel", metavar="DEVPATH", help="label one validated companion without acquiring reset/IRQ leases")
    action.add_argument("--restore-labels", action="store_true", help="restore defaults on surviving verified nodes with our exact labels")
    args = parser.parse_args()
    try:
        if args.relabel or args.restore_labels:
            if os.geteuid() != 0:
                raise ValueError("label changes require root")
        if args.restore_labels:
            restore_labels()
        elif args.udev or args.refresh_spi or args.relabel:
            devpath = args.udev or args.refresh_spi or args.relabel
            if not devpath.startswith("/devices/"):
                raise ValueError("udev DEVPATH must be beneath /devices")
            node = resolve(SYS / devpath.lstrip("/"))
            if args.relabel:
                relabel(node)
                return 0
            if args.refresh_spi:
                matches = [candidate for candidate in nodes(("class/spidev/spidev*",))
                           if spi_parent(candidate) == node]
                if len(matches) != 1:
                    raise ValueError("SPI event has no unique spidev companion")
                identify_node(matches[0], "spi", False)
                (matches[0] / "uevent").write_text("change\n", encoding="ascii")
                return 0
            role = role_for_node(node)
            _, _, instance = identify_node(node, role, False)
            # A suspended chip keeps its identity/label; service configuration
            # and runtime access separately require ready and the current epoch.
            print("FTE3600_PAIR_ROLE=" + role)
            print("FTE3600_PAIR_ID=" + instance)
        else:
            for role, path, alias in pairs():
                print(role, path, alias)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("FTE3600 pairing failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
