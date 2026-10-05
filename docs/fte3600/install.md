# Build & Installation Guide

[Documentation index](README.md)

Use this guide to choose a build, install any required external firmware, and
test capture or opt-in authentication. Check the [support matrix](status.md#implemented-functions-and-test-limits)
first: implemented chip protocols are not a guarantee that a particular laptop
has passed hardware testing.

The current driver uses stock spidev with an out-of-tree reset/IRQ glue module.
Follow the sections below for dependencies and the library build, then
[system integration](acpi-spidev.md) for kernel configuration, old-installation
migration, permissions and removal. Start with capture-only mode; enrollment
requires an explicit authentication build. Secure Boot signing still applies
to the glue module.

---

## 1. Prerequisites & Dependencies

Clone the repository and ensure all commands are run from the repository root:

```sh
git clone https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600
```

Install build dependencies for your distribution:

### Arch Linux
```sh
sudo pacman -S --needed base-devel git meson ninja glib2 glib2-devel \
  libgusb libgudev systemd cairo fprintd python
```

### Fedora 43+
```sh
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel systemd-devel \
  systemd cairo-devel fprintd python3
```

### Ubuntu 26.04+
```sh
sudo apt install build-essential git meson ninja-build pkg-config \
  libglib2.0-dev libgusb-dev libgudev-1.0-dev libudev-dev \
  systemd systemd-dev libcairo2-dev fprintd python3
```

The device-permission helper requires `systemd-escape`, provided by the
`systemd` package on these distributions. Development packages such as
`systemd-dev` alone do not supply this runtime tool; it is also required by
the setup helper's local tests.

The experimental transport requires stock `spidev`, kernel UIO support
(`CONFIG_UIO=y` or `m`), the `fte3600` reset/IRQ glue and matching kernel headers.
libgpiod is not a dependency: userspace uses the GPIO character-device v2 ABI
for reset and the standard UIO event counter for IRQ notification.
Follow [ACPI glue and stock spidev](acpi-spidev.md)
before installing. This transport has not yet been validated on hardware.
Record the exact commit used for the build; older revisions use a different
custom SPI bridge interface and must follow the migration procedure.

The firmware installation helper now requires **Python 3** (standard library
only); it checks for that dependency before doing any work. CAB input or a
download also requires `cabextract` or `7z`. Local DLL and raw binary inputs
need no archive tool. Python handles optional HTTPS downloads; `curl` and
`wget` are no longer required by this helper.

---

## Record a rollback plan before installing

Prefer the distribution package path on a test machine. Manual `--prefix=/usr`
installation can overwrite package-owned files and is not tracked as a separate
package; a later system update can overwrite this fork. Keep the build directory,
source commit, build options and `meson-logs/install-log.txt`.

Before installing, list the destinations with
`meson introspect build-fte3600 --installed` after configuration. Record which
files were already present and which package owned them. Back up each preexisting
file that you will overwrite and retain the original package version.

Do the same **before** installing any of these optional/manual items:

- `/etc/systemd/system/fprintd.service.d/10-fte3600-acpi-spidev.conf`
- `/lib/modules/<kernel>/extra/fte3600.ko` or the DKMS installation
- Each selected file under `/usr/lib/firmware/fte3600/` (listed below)
- Any old FTE3600-specific spidev override, GPIO service permission or SELinux
  configuration that you replace during migration.

Record an absent destination explicitly; absence and an existing identical file
are different rollback cases. Keep a copy/hash of the newly installed file as
well. If an existing configuration or SELinux module belongs to somebody else,
review/merge it instead of blindly overwriting it. Do not proceed without a
working password login and a recovery path.

## 2. Install the firmware for the identified chip

The repository and its packages contain no vendor firmware payloads. The
installer selects files explicitly; it does not detect hardware, authorize
recovery, change authentication policy or flash the sensor. Runtime discovery
must independently establish identity before any upload. An all-zero response
alone is not proof of a chip model or missing firmware.

| Chip | Files below `/usr/lib/firmware/fte3600/` | Bytes | When the driver can use them |
| --- | --- | ---: | --- |
| FT9338 | `ft9338.bin` | 14184 | RAM recovery after runtime `5858` in this open and matching boot-B OTP; first unidentified cold boot remains unsupported |
| FT9348 | `ft9348.bin` | 10312 | RAM recovery after matching A8 ROM family and SPI OTP |
| FT9361 | `ft9361.bin` | 10396 | RAM recovery after matching A8 ROM family and SPI OTP |
| FT9536 | `ft9536.bin` | 11934 | Positive boot-A identification, or runtime `4080` in this open plus matching boot-B OTP |
| FT9368 | `ft9368-app.bin` | 27120 | Explicit persistent update; requires the PRAM file and a healthy identified application |
| FT9368 | `ft9368-pramboot.bin` | 6096 | Required together with the application image |

FT9365, FW9369 (silicon ID `9362`) and FT9769 use host configuration and have
no application firmware to install. Selecting them, or an unknown chip, produces
an error instead of selecting FT9361. See the [hardware inventory](windows-hardware-inventory.md)
for extraction provenance and the [status](status.md) for current support limits.

Print exact filenames, byte counts and SHA-256 values, then validate local input
without installing anything:

```sh
./scripts/install-firmware.sh --list
./scripts/install-firmware.sh --chip ft9348 \
  --input /path/to/ftWbioUmdfDriverV2.dll --verify-only
```

The same interface accepts a `.cab` archive or a raw application `.bin`.
`--format dll`, `--format cab` or `--format binary` can override filename-based
format selection. DLL extraction checks the known locations from the audited
2.0.3.99/.100/.102 packages and verifies each extracted payload. An unsupported
DLL layout fails explicitly; supply the separately extracted, matching binary.
The helper does not execute the DLL or validate its Windows package signature.
CAB extraction sets a kernel-enforced 16 MiB output-file limit before starting
the extractor, suppresses its diagnostic streams and kills/reaps it after a
60-second timeout. An oversized compressed DLL is rejected during writing,
before it can grow beyond that limit; the extracted file is checked again
before payload selection.

Stage files in a writable directory, or install them to the system after
recording the rollback information above:

```sh
./scripts/install-firmware.sh --chip ft9348 \
  --input /path/to/ftWbioUmdfDriverV2.dll --destdir ./firmware-staging
sudo ./scripts/install-firmware.sh --chip ft9348 \
  --input ./firmware-staging/fte3600/ft9348.bin
```

`--destdir` is the **firmware root**; the helper appends `fte3600/`. Inputs must
be bounded regular files, not symlinks or FIFOs. The installer validates all
selected files before creating the destination, stages them in that directory,
and replaces each final file atomically with mode `0644`. The two FT9368 files
are not a single atomic transaction: interruption can leave one old file and
one new file. Runtime validation of both exact hashes prevents programming an
incompatible pair. Existing destination symlinks/directories are rejected.

To extract and install both FT9368 images from one DLL, select `--chip ft9368`.
When starting with raw binaries, supply both explicitly:

```sh
./scripts/install-firmware.sh --chip ft9368 --input /path/to/ft9368-app.bin \
  --pramboot /path/to/ft9368-pramboot.bin --verify-only
sudo ./scripts/install-firmware.sh --chip ft9368 --input /path/to/ft9368-app.bin \
  --pramboot /path/to/ft9368-pramboot.bin
```

FT9368 normally uses its running application without updating it. Installing
these files does not enable `FTE3600_FT9368_UPDATE=1`; that separate, explicit
driver setting requests persistent flash programming at open. It requires a
positive current application identity and both validated files. Remove the
setting after an intended update, since leaving it set requests another update
on the next open. A blank or unresponsive FT9368 has no supported automatic
recovery path. See the [FT9368 update boundary](ft9368-protocol.md#firmware-boundary).

Optional downloading uses the pinned Microsoft Update Catalog package:

```sh
./scripts/install-firmware.sh --chip ft9361 --download --destdir ./firmware-staging
```

For compatibility, a lone positional input still selects **only FT9361**, and
the historical no-argument invocation still downloads FT9361. Option-based
calls require `--chip` and exactly one of `--input` or `--download`. Missing or
invalid local inputs fail without downloading or trying another chip. The
helper never invokes `sudo` itself; use a writable staging directory or run an
explicit installation command with the required privileges.

Legacy RAM backends use `/usr/lib/firmware` by default; their diagnostic
`FTE3600_FIRMWARE_PATH` override still checks the selected chip's size/hash.
FT9368 uses `FTE3600_FIRMWARE_DIR` as a firmware root containing `fte3600/`.
Neither override relaxes the hardware identity checks.

---

## 3. Build & Installation

### Option A: Arch Linux Package (Recommended for Arch)

The developer `PKGBUILD` compiles with `-Dfte3600_personal_auth=true` and installs
DKMS 0.2 glue sources. It packages the checkout's committed HEAD; local
uncommitted edits are excluded. Record that commit, install matching kernel
headers and follow [the migration guide](acpi-spidev.md) before this sequence.
For a capture-only build, use the manual route below instead of this package's
explicit personal-auth policy.

```sh
cd packaging/arch
makepkg -si
sudo reboot
```

### Option B: Manual Meson Compilation (All Distributions)

Start with capture-only mode. To enable experimental personal authentication after reviewing the security policy, explicitly reconfigure with `meson configure build-fte3600 -Dfte3600_personal_auth=true` and rebuild/retest. Do not enable system-wide sudo/root biometrics.

To build and install manually:

```sh
# Configure build directory
meson setup build-fte3600 --prefix=/usr \
  -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=false \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dintrospection=false \
  -Dinstalled-tests=false \
  -Dwerror=true

# Compile
meson compile -C build-fte3600

# Execute unit and lifecycle test suite
meson test -C build-fte3600 --print-errorlogs \
  fte3600-context fte3600-resources fpi-spi-transfer \
  fte3600-driver fte3600-brisk fte3600-template \
  fte3600-family-template fte3600-lifecycle fte3600-auth-lifecycle

# Local synthetic installer tests; no downloads or system installation
python3 tests/test-install-firmware.py
python3 tests/test-setup-fte3600.py
python3 tests/test-fte3600-pair.py

# Install library
sudo meson install -C build-fte3600
```

> [!NOTE]
> Setting `-Dfte3600_personal_auth=false` builds a capture-only driver without host verification capabilities. The upstream proposal retains authentication disabled by default.

---

## 4. System Configuration

Follow [the system integration guide](acpi-spidev.md) for prerequisites,
rollback records, old-bridge migration, installation and checks. Its setup
helper builds DKMS 0.2 when available (or installs the manually built module),
verifies stock spidev's running buffer is at least 32768, and validates the
related SPI/reset/UIO nodes before publishing exact-node fprintd permissions.

```sh
sudo ./scripts/setup-fte3600.sh install-all
sudo ./scripts/setup-fte3600.sh check
```

The helper will not take a device from a bound old bridge or unload global
spidev. A smaller buffer or a replaced loaded module requires reboot. Every
failed installation stage returns nonzero; completed earlier stages can remain.
Successful setup proves neither sensor identity nor capture.

SELinux configuration labels only the validated companion nodes with
`fte3600_spidev_t`, `fte3600_gpio_t` and read-only `fte3600_irq_t`.
Actual fprintd-domain access, including GPIO line-request descriptors and UIO
events, still needs a runtime test on the distribution.
Do not install the old broad GPIO policy or disable SELinux. The guide explains
labeling, service-device lifetime, crash limitations and exact rollback files.

ACPI must describe one SPI connection, one single-pin reset GpioIo and one
edge-triggered interrupt: single-pin GpioInt or a single ordinary ACPI
IRQ/Interrupt resource. Missing or contradictory resources fail
explicitly. Firmware installation cannot repair board wiring. Reset polarity
is independent of negotiated SPI chip-select polarity; there is no model-name
bypass. See [GPIO polarity](gpio-polarity.md) for the electrical evidence.

## 5. Enrollment & Verification

This section requires an explicit personal-auth build followed by rebuild, retest and installation. Capture-only builds intentionally cannot enroll or verify; do not treat that as a hardware fault.

After rebooting, validate the related SPI/reset/UIO nodes before authentication testing:
```sh
sudo /usr/libexec/fte3600-pair
sudo ./scripts/setup-fte3600.sh check
```

Ensure you have a working root/user password fallback, then enroll a finger:
```sh
fprintd-enroll -f left-index-finger "$USER"
```

Verify authentication against the enrolled template:
```sh
fprintd-verify -f left-index-finger "$USER"
```

> [!IMPORTANT]
> **Extractor Schema 3 / new templates: Diagnostic Policy 7 / Authentication Policy 8**:
> Verification accepts a passing match against any of the eight enrolled samples
> or their canonically reconstructed mosaic. Every modern mosaic connection
> must pass the complete diagnostic policy; rigid-fit success and an inlier
> count alone are insufficient.
> All eight chip profiles use their native image dimensions. New templates carry
> the chip identity and processing revision. Existing wire-v1 FT9361 templates
> with Schema 3 / Policy 5/6 remain supported without re-enrollment.
> Earlier unsupported policies or extractor schemas require re-enrollment.
> Schema 3 applies image normalization exactly once. Keep a working password;
> if replacing an unsupported enrollment, `fprintd-delete "$USER"` deletes that
> user's prints before enrolling again. Changing stored version fields is not
> a valid migration. See [template compatibility](family-authentication.md).

## Removing this fork and restoring the previous installation

Close fingerprint clients and stop `fprintd.service` while restoring libraries.
Do not run a broad `rm` against library directories or an unreviewed
`ninja uninstall`: those can remove files shared with the distribution package.

1. **Arch package installation:** reinstall the distribution's `libfprint`
   with `sudo pacman -S libfprint`, accepting replacement of the conflicting
   fork package only after checking the transaction. Its package-owned DKMS sources are removed by the package manager; the removal hook
   reloads systemd/udev. Files installed manually in `/etc` or the firmware path
   are separate and are not removed by this transaction.
2. **Manual library installation:** compare the retained Meson install log with
   your pre-install record. Restore overwritten files from the original package
   or backup. Reinstall the original library package (for example
   `sudo dnf reinstall libfprint` on Fedora or
   `sudo apt install --reinstall libfprint-2-2` on Ubuntu).
   Reinstallation does not necessarily remove extra files introduced by a newer
   source build: review the exact logged destinations and remove only files
   recorded as previously absent, still matching your installed copy, and not
   now owned by another package. If those conditions are unknown, stop and
   resolve ownership instead of deleting files.
3. **Manual configuration and firmware:** for each exact path in the rollback
   list above, restore the saved original if one existed. Remove a file only if
   you recorded it as absent before this installation and it still matches the
   copy you installed. Leave subsequently edited/unrecognized files untouched
   for manual review. Do not delete their parent directories.
4. **Kernel glue:** for a manual installation, restore any saved previous
   module, or remove only the exact new module file recorded in the rollback
   list. Run `sudo depmod -a` for the affected kernel (pass its version if it
   differs from the running kernel). DKMS package removal handles its own module
   files. Reboot before expecting the previous SPI driver to bind again.
5. **SELinux:** the setup helper removes only its own `fte3600-acpi-spidev`
   module at default local priority 400 and restores remaining validated-node
   labels. If a policy existed before installation, restore its recorded
   original. Handle older `fte3600-gpio`/`fte3600-bridge` configurations according
   to their saved ownership and migration record; do not remove other modules
   or disable SELinux.
6. Reload the library cache with `sudo ldconfig`, run
   `sudo systemctl daemon-reload` and `sudo udevadm control --reload`, then reboot
   to restore the previous driver binding and kernel module configuration. Verify password login and
   the restored package/service state before changing authentication settings.

These steps do not delete enrolled fingerprints. If you deliberately wish to
remove enrollment, `fprintd-delete "$USER"` deletes that user's enrolled prints;
it is separate from removing the driver and requires subsequent re-enrollment.
Restore any PAM changes from their own pre-change backups; this guide does not
authorize removal of an existing password authentication flow.
