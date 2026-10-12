# Current FTE3600 implementation

This guide describes the independently written 0.2.1 SPI implementation now
integrated directly into this libfprint tree. Earlier driver documentation is
retained as historical reference; its authentication flags, template format
and runtime instructions do not describe this implementation.

## What has been established

The implementation provides capture, eight-sample BRISK enrollment, persistent
prints, verification and identification through libfprint. The personal-use
authentication policy is enabled for this driver. It has not been calibrated
for population false-accept or false-reject rates.

On 2026-10-12, a One-Netbook A1 with an FT9361 sensor running NixOS x86_64
completed eight-stage enrollment and three separate positive fprintd
verifications. Each verification reopened the device and loaded the saved
template. This hardware observation used the 0.2.1 stable libfprint integration;
the current tree integrates the same driver logic with official libfprint
master. Software checks of that integration are distinct from hardware tests.

FT9338, FT9348, FT9361, FT9536, FT9365, FT9368, FW9369 and FT9769 paths are
implemented. The A1 result does not validate the other families. Different-finger
and empty-sensor controls, physical suspend/resume, temperature behavior,
population accuracy and Windows initialization/capture parity remain open.
USB capture/authentication, including Pocket 4 USB `2808:0752`, is outside this
implementation.

## Prepare the system

Keep password access and record the installed libfprint package and any local
fprintd service overrides before changing them. Preserve previous prints in a
protected local backup. The previous SamSeven777 print format has not been
proven compatible: enroll new prints after migration. No automatic deletion
or conversion of old prints is provided. Avoid overwriting a previous finger
slot unless its backup and rollback path are available.

The transport needs Linux spidev, this repository's `kernel/fte3600` reset/IRQ
glue with ABI 2, and a spidev transfer buffer of at least 65536 bytes. Install
matching kernel headers and sign the module where Secure Boot requires it.
The retained integration helpers derive the SPI/GPIO/UIO tuple from ACPI:

```sh
sudo ./scripts/setup-fte3600.sh install-kernel
./scripts/setup-fte3600.sh check
cat /sys/module/spidev/parameters/bufsiz
```

The buffer output must be at least `65536`. A loaded spidev module keeps its
old parameter: reboot after changing its module configuration instead of
unloading shared SPI resources. The setup helper rejects old bridge layouts;
the preserved [earlier migration procedure](install.md#migrate-an-earlier-bridge-installation)
is relevant only to removing that old transport installation.

fprintd needs the validated SPI node, reset GPIO and IRQ UIO available inside
its service. Its normal SPI allowance alone is insufficient. Inspect and
install the exact companion permissions using:

```sh
./scripts/fte3600-device-allow.sh
sudo ./scripts/setup-fte3600.sh install-systemd
sudo ./scripts/setup-fte3600.sh install-selinux
systemctl cat fprintd.service
```

The glue lease requires global root; a service configured under another user
needs corresponding system integration. Device namespaces must expose the
real nodes, and SELinux access must be allowed on systems that enforce it.
The helper does not replace another distribution's service/package policy.
On NixOS, express the module, firmware and service settings in the system
configuration and retain a working generation for rollback.

## Supply firmware

Firmware is external. Identify the sensor before selecting its payloads. The
retained installer validates their exact sizes and hashes without executing
a vendor library. For an FT9361 with a locally supplied input:

```sh
sudo ./scripts/install-firmware.sh --chip ft9361 \
  --input /path/to/local/input.dll --destdir /usr/lib/firmware
```

Use the actual chip and supplied file; run
`./scripts/install-firmware.sh --help` for raw-file and other supported input
formats. FT9368 needs its app and PRAM boot files plus the auxiliary
`ft9536.bin`; install both the `ft9368` and `ft9536` selections from the
approved local source. FT9365, FW9369 and FT9769 use host configuration rather
than an installed payload.

The runtime reads exact raw regular files in `/usr/lib/firmware/fte3600` by
default. It does not accept compressed payloads or payload-file symlinks.
For another location, set `-Dfte3600-firmware-dir=/path/to/firmware/fte3600`
at build time. Use a readable location outside a protected home directory;
the system service may not be able to read files that an interactive shell
can read. For NixOS, point to a directory containing real raw files and check
the resolved service-visible paths. Missing or mismatched payloads stop
initialization.

## Build and install

Use the target distribution's native compiler and development packages.
The FTE3600-only build needs a C/C++ toolchain, Meson, Ninja, pkg-config,
Python 3, GLib/GIO/GObject, GUSB and GUdev. Distribution package examples:

| Distribution | Build dependencies |
| --- | --- |
| Ubuntu | `build-essential pkg-config meson ninja-build python3 libglib2.0-dev libgusb-dev libgudev-1.0-dev libudev-dev systemd-dev` |
| Fedora | `gcc gcc-c++ make pkgconf-pkg-config meson ninja-build python3 glib2-devel libgusb-devel libgudev-devel systemd-devel` |
| Arch | `base-devel pkgconf meson ninja python glib2 libgusb libgudev systemd` |

Install `fprintd` separately for its service and CLI. This is a normal Meson
source tree; no exporter or extra source kit is required:

```sh
meson setup build --prefix=/usr -Ddrivers=fte3600 \
  -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson compile -C build
```

The production library is under `build/libfprint/` and uses libfprint ABI 2.
`-Ddrivers=fte3600` includes only this driver, so a service using that library
will not serve other readers. Configure the desired driver set explicitly
when packaging. The old `fte3600_personal_auth` and `fte3600_ipa_auth` options
are not part of this implementation.

Prefer installing through a package built for the target distribution so
the package manager tracks replaced files. For a deliberate local source
installation after recording your rollback package:

```sh
sudo meson install -C build
sudo ldconfig
sudo systemctl restart fprintd.service
```

Use your distribution's service/package mechanism where these commands do not
apply. This direct installation replaces the library at the selected prefix;
it is different from the earlier private test-package deployment. A build on
one distribution does not establish binary compatibility with another.

## Enroll and check

Leave the sensor untouched during startup/calibration, then follow the
prompts. Eight distinct, consistent accepted samples are needed; lift and
replace the finger between captures. Low contrast, exact duplicates and
inconsistent samples cause retries.

```sh
fprintd-enroll -f right-index-finger "$USER"
fprintd-list "$USER"
fprintd-verify -f right-index-finger "$USER"
sudo systemctl restart fprintd.service
fprintd-verify -f right-index-finger "$USER"
```

Test a different finger and an empty sensor, plus cancellation and reopening,
before relying on authentication. Enrollment stages alone do not establish
independent physical touches: only FT9368 currently implements separate
release polling. The other families rely on their acquisition behavior and
duplicate rejection. Physical suspend/resume remains unvalidated; active
suspend follows libfprint's unsupported/cancellation fallback.

For failures, inspect `journalctl -u fprintd -b`, the device pairing and the
service's `DeviceAllow` configuration. Distinguish resource/permission errors,
firmware failures, capture timeouts and matcher outcomes. Preserve biometric
images and templates locally; do not attach them to issues.

To roll back, restore the previous libfprint package/build and service
configuration, then restart fprintd. Preserve both old and new prints; restore
the appropriate backed-up state only when needed. Library rollback does not
require deleting prints or unloading shared SPI resources.

## Software checks and implementation scope

```sh
./scripts/check-fte3600.sh
```

This creates separate production and synthetic-backend build directories.
It runs the 24-case native authentication executable, generic ACPI enumeration
through umockdev, framework device/state-machine checks, the real libfprint
public API against mathematical frames, and the kernel glue's host policy
check. Install `umockdev` and its development package (`libumockdev-dev` on
Ubuntu, `umockdev-devel` on Fedora) for enumeration coverage. The synthetic
library is not installed and must never be deployed for authentication.

CI configures native Ubuntu and Fedora builds and an Ubuntu kernel-header
build. A configured job is not evidence that every distribution or sensor
has been tested. These software checks do not establish biometric accuracy.

The maintained driver lives in `libfprint/drivers/fte3600/`. It remains an
`FpDevice` subclass: the selected BRISK workflow is separate from
`FpImageDevice`'s NBIS workflow. Generic ACPI enumeration belongs to the
framework; device protocol and authentication policy belong to the driver.

The independent implementation input is pinned to commit `47e3b32`.
The attributed SamSeven777 BRISK, reset/IRQ glue and firmware-installer inputs
are pinned to `7e12c09abbd53516076b6d67a480003dc272cc8e`. The framework baseline
is official libfprint master `6f9479c3d55f847c1b3769f28ceb99227f9858cf`.
This states the publication's source scope, not Windows behavioral parity or
upstream acceptance. See the retained license and attribution files with the
driver dependencies. Proprietary binaries, firmware payloads, captures and
templates are not included in the published source.
