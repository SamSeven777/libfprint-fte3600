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

## 2. Firmware for Cold-Boot Recovery

When the machine is completely powered off, the FT9361 sensor loses its volatile SRAM code and boots into an uninitialized MCU state (`00 00`). The driver automatically restores normal operation by uploading a 10,396-byte microcode payload over SPI to `/usr/lib/firmware/fte3600/ft9361.bin`.

Neither the repository nor its binary packages bundle this proprietary binary. You must extract it using one of the methods below.

### Option A: Automated Download & Extraction (Recommended)

Run the included clean-room script. It downloads the signed vendor driver package directly from the Microsoft Update Catalog, extracts the firmware payload at the verified byte offset, validates the SHA256 checksum, and installs it:

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

To build and install manually:

```sh
# Configure build directory
meson setup build-fte3600 --prefix=/usr \
  -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=true \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dintrospection=false \
  -Dinstalled-tests=false \
  -Dwerror=true

# Compile
meson compile -C build-fte3600

# Execute unit and lifecycle test suite
meson test -C build-fte3600 --print-errorlogs \
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template test-fte3600-lifecycle

# Install library
sudo meson install -C build-fte3600
```

> [!NOTE]
> Setting `-Dfte3600_personal_auth=false` builds a capture-only driver without host verification capabilities. Default upstream submissions use capture-only by default.

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
On Fedora systems with SELinux in Enforcing mode, `fprintd_t` is blocked from opening GPIO character devices. Install the provided CIL module:

```sh
sudo semodule -i config/selinux/fte3600-gpio.cil
```

After completing system configuration, reboot the system:
```sh
sudo reboot
```

---

## 5. Enrollment & Verification

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

