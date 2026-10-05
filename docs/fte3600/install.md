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

```bash
cd kernel/fte3600
make
sudo make install
sudo depmod -a
sudo modprobe fte3600
cd ../..
```

*(Optional DKMS)*:
```bash
sudo make -C kernel/fte3600 dkms-install
```

---

## 3. Firmware Installation

Sensors FT9338, FT9348, FT9361, FT9536, and FT9368 require external firmware placed in `/usr/lib/firmware/fte3600/`:

```bash
sudo mkdir -p /usr/lib/firmware/fte3600

# Copy extracted firmware files to the firmware directory
# Supported files: ft9338.bin, ft9348.bin, ft9361.bin, ft9536.bin, ft9368-app.bin, ft9368-pramboot.bin
sudo cp /path/to/extracted/firmware/*.bin /usr/lib/firmware/fte3600/
```

> **Note**: Chips FT9365, FW9369, and FT9769 operate from ROM/Host mode and do not require firmware binaries.

---

## 4. Build and Install libfprint

Configure the build with Meson. Set `-Dfte3600_personal_auth=true` to enable host-side matching (BRISK):

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

---

## 5. System Integration & Verification

### Install Udev & Systemd Rules
```bash
sudo ./install/setup-fte3600-systemd-device.sh --install
sudo udevadm control --reload-rules && sudo udevadm trigger
```

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
# Remove systemd & udev overrides
sudo ./install/setup-fte3600-systemd-device.sh --uninstall

# Uninstall kernel module
sudo make -C kernel/fte3600 uninstall
sudo depmod -a

# Reinstall upstream libfprint
# (e.g., sudo pacman -S libfprint / sudo apt install --reinstall libfprint-2-2)
```
