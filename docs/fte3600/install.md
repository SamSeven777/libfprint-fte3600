# Installation

Build natively on the target distribution. The driver needs Linux spidev,
the included ACPI reset/IRQ glue (ABI 2), matching kernel headers and fprintd.
Save the current libfprint package and service configuration for rollback.

| Distribution | Build dependencies |
| --- | --- |
| Ubuntu | `build-essential pkg-config meson ninja-build python3 libglib2.0-dev libgusb-dev libgudev-1.0-dev libudev-dev systemd-dev` |
| Fedora | `gcc gcc-c++ make pkgconf-pkg-config meson ninja-build python3 glib2-devel libgusb-devel libgudev-devel systemd-devel` |
| Arch | `base-devel pkgconf meson ninja python glib2 libgusb libgudev systemd` |

Install fprintd and the headers for the running kernel separately. Commands
below run from the repository root. On NixOS, package the library and kernel
module in derivations and configure firmware and fprintd permissions through
NixOS configuration; retain a working generation. The imperative installation
commands below are for conventional distributions, not NixOS.

## Device and firmware

```sh
sudo ./scripts/setup-fte3600.sh install-kernel
./scripts/setup-fte3600.sh check
cat /sys/module/spidev/parameters/bufsiz
```

The buffer must be at least `65536`. Reboot if a loaded spidev module retains
a smaller buffer or an older glue module remains loaded. Secure Boot requires
a trusted module signature. Resources are discovered from ACPI; do not guess
GPIO pins or select firmware solely from the FTE3600 ACPI name.

Select firmware for the identified chip. Example for FT9361:

```sh
sudo ./scripts/install-firmware.sh --chip ft9361 \
  --input /path/to/driver.dll --destdir /usr/lib/firmware
```

FT9338, FT9348, FT9361 and FT9536 need their corresponding payloads. FT9368
needs its application and PRAM boot files plus `ft9536.bin`; install both
`ft9368` and `ft9536` selections. FT9365, FW9369 and FT9769 need no external
payload through these backends. See `./scripts/install-firmware.sh --help`
for supported inputs and explicit download selection.

The installer checks exact sizes and hashes. The runtime reads raw regular
files under `/usr/lib/firmware/fte3600`; compressed files and payload-file
symlinks are rejected. To use another service-readable directory, configure
`-Dfte3600-firmware-dir=/path/to/firmware/fte3600` when building. On NixOS,
point this at a store directory containing real raw files.

## Build and service

```sh
meson setup build --prefix=/usr --buildtype=release -Ddrivers=fte3600 \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson compile -C build
sudo meson install -C build
sudo ldconfig
sudo ./scripts/setup-fte3600.sh install-systemd
sudo ./scripts/setup-fte3600.sh install-selinux
sudo systemctl restart fprintd.service
```

Prefer distribution packaging when replacing the system library. `fte3600`
builds only this reader; select additional drivers if needed. BRISK
personal-use authentication is enabled with the driver. There are no
`fte3600_personal_auth` or `fte3600_ipa_auth` options in this implementation.

fprintd needs root access to the validated SPI/GPIO/UIO tuple. Inspect its
permissions with `./scripts/fte3600-device-allow.sh` and
`systemctl cat fprintd.service`. The SELinux helper does nothing when SELinux
is disabled; on enforcing systems it installs the scoped policy.

## Use and troubleshooting

Keep the sensor untouched during startup. Follow the enrollment prompts,
lifting and replacing the finger between eight accepted samples:

```sh
fprintd-enroll -f right-index-finger "$USER"
fprintd-verify -f right-index-finger "$USER"
```

Low-quality, duplicate and inconsistent samples cause retries. FT9368 and
FW9369 have explicit release detection; other families do not confirm finger
removal separately. Physical suspend/resume remains unverified. Keep password
access. Re-enroll when migrating from the previous driver; template
compatibility is not established, so preserve existing prints for rollback.

For errors, run `./scripts/setup-fte3600.sh check` and inspect
`journalctl -u fprintd -b`. Check resource pairing, service permissions and
firmware before diagnosing capture or matching. Keep biometric data private.

To roll back, restore the previous libfprint package and service configuration,
then restart fprintd. To remove this helper's kernel/service integration, use
`sudo ./scripts/setup-fte3600.sh uninstall`. Firmware and enrolled prints are
not automatically deleted.

## Migrate an earlier bridge installation

The old private `/dev/fte3600-*` bridge is incompatible. Save its configuration,
use the old checkout's removal procedure, and reboot before installing this
version. Review old fprintd drop-ins and `fte3600-bridge` / `fte3600-gpio`
SELinux modules; the new policy does not revoke old permissions. Do not force
an active device to unbind to bypass the migration check.
