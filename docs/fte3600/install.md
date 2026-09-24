# Build & Installation Guide

This guide covers dependency installation, firmware setup for cold-boot recovery, compilation, system configuration, and enrollment verification for `libfprint-fte3600`.

---

## 1. Prerequisites & Dependencies

Clone the repository and ensure all commands are run from the repository root:

```sh
git clone --branch main https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600
```

Install build dependencies for your distribution:

### Arch Linux
```sh
sudo pacman -S --needed base-devel git meson ninja glib2 glib2-devel \
  libgusb libgudev libgpiod cairo fprintd
```

### Fedora 43+
```sh
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel libgpiod-devel systemd-devel \
  systemd cairo-devel fprintd
```

### Ubuntu 26.04+
```sh
sudo apt install build-essential git meson ninja-build pkg-config \
  libglib2.0-dev libgusb-dev libgudev-1.0-dev libgpiod-dev libudev-dev \
  systemd-dev libcairo2-dev fprintd
```

> [!NOTE]
> The driver requires **`libgpiod` 2.x**. Older distributions (e.g., Ubuntu 22.04 / 24.04) provide `libgpiod` 1.x and require a backport or manual build of libgpiod 2.x.
> Hardware validation is primarily conducted on Arch Linux; Fedora and Ubuntu are validated via build and unit tests.

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

- `/etc/modprobe.d/fte3600-spidev.conf`
- `/etc/systemd/system/fprintd.service.d/10-fte3600-gpio.conf`
- `/usr/lib/firmware/fte3600/ft9361.bin`
- The SELinux module named `fte3600-gpio`, if applicable.

Record an absent destination explicitly; absence and an existing identical file
are different rollback cases. Keep a copy/hash of the newly installed file as
well. If an existing configuration or SELinux module belongs to somebody else,
review/merge it instead of blindly overwriting it. Do not proceed without a
working password login and a recovery path.

## 2. Firmware for Cold-Boot Recovery

When the machine is completely powered off, the FT9361 sensor loses its volatile SRAM code and boots into an uninitialized MCU state (`00 00`). The A1 recovery implementation reads a 10,396-byte image from `/usr/lib/firmware/fte3600/ft9361.bin` and uploads it to sensor RAM over SPI. A `00 00` response alone does not distinguish missing runtime code from transport, reset or power problems.

Neither the repository nor its binary packages bundle this proprietary binary. You must extract it using one of the methods below.

### Option A: Automated Download & Extraction (Recommended)

Run the included extraction script. It downloads the signed vendor driver package directly from the Microsoft Update Catalog, extracts the firmware payload at the verified byte offset, validates the SHA256 checksum, and installs it. It validates the extracted payload hash, not the Windows package signature:

```sh
./scripts/install-firmware.sh
```
*(Requires `curl` or `wget`, and `cabextract` or `7z`).*

### Option B: Local Windows Driver Extraction

If you have access to the vendor driver (`ftWbioUmdfDriverV2.dll` or the vendor `.cab` archive):

```sh
# Automated extraction from local file:
./scripts/install-firmware.sh /path/to/ftWbioUmdfDriverV2.dll

# Or manual extraction using dd (offset 489824 for driver v2.0.3.102):
tmp_fw=$(mktemp -d)
dd if=/path/to/ftWbioUmdfDriverV2.dll of="$tmp_fw/ft9361.bin" \
  bs=1 skip=489824 count=10396 status=none
printf '027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f  %s\n' \
  "$tmp_fw/ft9361.bin" | sha256sum -c - && \
  sudo install -Dm644 "$tmp_fw/ft9361.bin" /usr/lib/firmware/fte3600/ft9361.bin
rm -rf "$tmp_fw"
```

Firmware integrity specifications:
- **Location**: `/usr/lib/firmware/fte3600/ft9361.bin`
- **File Size**: `10,396` bytes
- **SHA256**: `027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`

---

## 3. Build & Installation

### Option A: Arch Linux Package (Recommended for Arch)

The repository includes a ready-to-build `PKGBUILD` that compiles `libfprint` with `-Dfte3600_personal_auth=true` and automatically installs required systemd drop-ins and modprobe configurations:

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
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template fte3600-lifecycle

# Install library
sudo meson install -C build-fte3600
```

> [!NOTE]
> Setting `-Dfte3600_personal_auth=false` builds a capture-only driver without host verification capabilities. The upstream proposal retains authentication disabled by default.

---

## 4. System Configuration

For manual installations (non-Arch package), two system configurations are mandatory:

### 1. SPI Buffer Size (`bufsiz=32768`)
Linux kernel's default `spidev` buffer size is 4,096 bytes. The cold-boot recovery transfer requires a continuous 10,403-byte payload that cannot be chunked. Set the buffer size to 32,768 bytes:

```sh
sudo install -Dm644 config/modprobe.d/fte3600-spidev.conf /etc/modprobe.d/fte3600-spidev.conf
```

### 2. Systemd Sandbox GPIO Permission
Modern `fprintd.service` units run with restricted device access. To allow `fprintd` to toggle the hardware reset and interrupt GPIO lines, install the systemd service drop-in:

```sh
sudo install -Dm644 config/systemd/10-fte3600-gpio.conf /etc/systemd/system/fprintd.service.d/10-fte3600-gpio.conf
sudo systemctl daemon-reload
```

### 3. SELinux Policy (Fedora Only)
On Fedora, first confirm an AVC denial for `fprintd_t` and the local GPIO labels. Only then consider the provided broad GPIO-class CIL module, after recording/backing up any existing module:

```sh
sudo semodule -i config/selinux/fte3600-gpio.cil
```

After completing system configuration, reboot the system:
```sh
sudo reboot
```

---

## 5. Enrollment & Verification

This section requires an explicit personal-auth build followed by rebuild, retest and installation. Capture-only builds intentionally cannot enroll or verify; do not treat that as a hardware fault.

After rebooting, confirm that `spidev` bufsiz is active:
```sh
cat /sys/module/spidev/parameters/bufsiz
# Expected output: 32768
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
> **Policy Version 3 Notice**:
> If updating from older experimental versions, existing stored templates in `/var/lib/fprint/` will be rejected due to incompatible geometric consensus formats. Delete old templates using `fprintd-delete "$USER"` and perform a fresh enrollment.

## Removing this fork and restoring the previous installation

Close fingerprint clients and stop `fprintd.service` while restoring libraries.
Do not run a broad `rm` against library directories or an unreviewed
`ninja uninstall`: those can remove files shared with the distribution package.

1. **Arch package installation:** reinstall the distribution's `libfprint`
   with `sudo pacman -S libfprint`, accepting replacement of the conflicting
   fork package only after checking the transaction. Its package-owned service
   and modprobe drop-ins are removed by the package manager; the removal hook
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
4. **SELinux:** only if this procedure added a previously absent
   `fte3600-gpio` module, remove that exact module with
   `sudo semodule -r fte3600-gpio`. If a policy existed before, restore its saved
   original instead. Do not remove other modules or disable SELinux.
5. Reload the library cache with `sudo ldconfig`, run
   `sudo systemctl daemon-reload` and `sudo udevadm control --reload`, then reboot
   to restore the prior spidev module configuration. Verify password login and
   the restored package/service state before changing authentication settings.

These steps do not delete enrolled fingerprints. If you deliberately wish to
remove enrollment, `fprintd-delete "$USER"` deletes that user's enrolled prints;
it is separate from removing the driver and requires subsequent re-enrollment.
Restore any PAM changes from their own pre-change backups; this guide does not
authorize removal of an existing password authentication flow.

