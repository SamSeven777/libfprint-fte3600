# FTE3600 Build & Installation Guide

[Documentation Index](README.md) · [Wire Protocols](protocols.md) · [Troubleshooting](troubleshooting.md)

This guide walks you through building and installing the `libfprint` FTE3600 driver, kernel ACPI reset/IRQ glue module, required firmware, and system integration.

---

## 1. Prerequisites & Dependencies

### Package Installation by Distribution

#### Arch Linux
```bash
sudo pacman -S --needed base-devel git meson ninja glib2 libgusb libgudev \
  systemd cairo fprintd python linux-headers
```

#### Fedora 40+
```bash
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel systemd-devel systemd \
  cairo-devel fprintd python3 kernel-devel
```

#### Ubuntu 24.04+ / Debian 12+
```bash
sudo apt update
sudo apt install build-essential git meson ninja-build pkg-config \
  libglib2.0-dev libgusb-dev libgudev-1.0-dev libudev-dev \
  systemd systemd-dev libcairo2-dev fprintd python3 linux-headers-$(uname -r)
```

---

## 2. Kernel Module Setup (ACPI Glue)

The sensor requires `spidev` for SPI transfers and an out-of-tree ACPI glue module (`kernel/fte3600`) to manage the reset GPIO and IRQ line without exposing raw system GPIO chips.

Run commands from the repository root:

```bash
sudo ./scripts/setup-fte3600.sh install-kernel
```

The installer uses DKMS when available; otherwise it builds and installs the
module for the running kernel. It also installs the validated device-pairing
helper, udev rules, and `spidev.bufsiz=32768` configuration. Matching kernel
headers are required. If spidev is already loaded with a smaller buffer, reboot
to apply the configuration and rerun the installer. It does not unload a shared
spidev module.

Secure Boot still requires a module signature trusted by the running kernel.
The installer does not enroll signing keys. Replacing an already loaded older
glue module also requires a reboot before validating the new device pair.

---

## 3. Firmware Installation

FT9338, FT9348, FT9361, and FT9536 RAM recovery uses separate external firmware
images. FT9368 uses its two images only for the explicit persistent update
path, not every ordinary open. The installer verifies each payload's size and
SHA-256 against the sensor catalog:

```bash
./scripts/install-firmware.sh --list
# Example: extract and install the FT9338 payload from a supported local DLL.
sudo ./scripts/install-firmware.sh --chip ft9338 --input /path/to/ftWbioUmdfDriverV2.dll
```

Use the corresponding `--chip` value for other profiles. Files are installed
under `/usr/lib/firmware/fte3600/`. FT9365, FW9369, and FT9769 do not load an
external firmware file through these backends.

---

## 4. Build and Install libfprint

Configure the build with Meson. Set `-Dfte3600_personal_auth=true` to enable host-side matching (BRISK):

The ordinary authentication build and Arch package use BRISK only.
`fte3600_ipa_auth` remains disabled by default; plain Meson defaults also leave
personal authentication disabled until explicitly requested.

```bash
# Configure build directory
meson setup build-fte3600 \
  -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=true \
  -Ddoc=false \
  --prefix=/usr

# Compile
ninja -C build-fte3600

# Run automated test suite
ninja -C build-fte3600 test

# Install to system
sudo ninja -C build-fte3600 install
```

### Optional Grand Synergy v3 Matching

FT9361 and FW9369 (raw ID `0x9362`, as found in GPD Pocket 3) can opt into
BRISK + 2D-IPA matching. FW9369 IPA and fusion are experimental and await
hardware validation; enabling them is not a claim of improved accuracy.

For an existing build directory, enable both authentication options and
rebuild:

```bash
meson configure build-fte3600 \
  -Dfte3600_personal_auth=true \
  -Dfte3600_ipa_auth=true
ninja -C build-fte3600
ninja -C build-fte3600 test
sudo ninja -C build-fte3600 install
sudo systemctl restart fprintd
```

The optional build defaults to dual fusion. To choose a verification strategy,
set `FP_FTE3600_MATCHER=brisk`, `ipa`, or `dual` in the process that loads
libfprint. For normal `fprintd` use, set it in the service environment; setting
it only on the `fprintd-verify` client does not configure the daemon. Profiles
without IPA support use BRISK in dual mode and reject explicit IPA-only mode.

Re-enroll fingerprints after enabling IPA to store features for both engines.
Existing BRISK templates still work in BRISK or dual mode, but do not gain IPA
features automatically and cannot be used for IPA-only verification. New
dual templates require an IPA-enabled build; re-enroll with BRISK after
returning to a BRISK-only build.

---

## 5. System Integration & Verification

### Install Systemd and SELinux Integration
```bash
sudo ./scripts/setup-fte3600.sh install-systemd
sudo ./scripts/setup-fte3600.sh install-selinux
./scripts/setup-fte3600.sh check
```

The systemd helper grants fprintd access to the validated SPI/GPIO/UIO pair.
When SELinux is active, the SELinux step installs the scoped policy and labels
that pair. A successful setup check does not replace a capture test in the
fprintd service domain. `install-all` combines kernel, systemd, and SELinux
installation and then restarts fprintd.

### Restart fprintd Service
```bash
sudo systemctl restart fprintd
```

### Verify & Enroll Fingerprint
```bash
# Check if device is detected
fprintd-enroll "$USER"

# Verify enrollment
fprintd-verify "$USER"
```

---

## 6. Uninstallation / Rollback

```bash
# Remove the experimental glue and its managed system integration.
sudo ./scripts/setup-fte3600.sh uninstall

# Reinstall upstream libfprint
# (e.g., sudo pacman -S libfprint / sudo apt install --reinstall libfprint-2-2)
```

The removal command retains files if the glue is busy or label restoration
fails. It does not unload global spidev or remove independently installed
firmware. Restore saved system configuration and reboot as needed.

### Migrate an Earlier Bridge Installation

The old `/dev/fte3600-*` SPI bridge and its service/policy configuration are
incompatible with the current GPIO/UIO glue. The installer detects them and
stops. Save the old configuration, use the old checkout's documented removal
procedure, and reboot before installing this version. Review old
`fte3600-bridge` or `fte3600-gpio` SELinux modules as well as the old fprintd
drop-ins; installing the new policy does not revoke permissions from an old
one. Do not forcibly unbind an active fingerprint device to bypass this check.
