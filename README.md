# FTE3600 / FT9361 Linux Driver

[![FTE3600 CI](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml/badge.svg)](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml)
[![License: LGPL-2.1-or-later](https://img.shields.io/badge/License-LGPL--2.1--or--later-blue.svg)](COPYING)

Production-grade, clean-room `libfprint` driver and host-side biometric matching system for the **FocalTech FT9361** capacitive SPI fingerprint sensor (`ACPI\FTE3600`, 64 × 80 pixel active array).

Supports device discovery, fail-closed platform probing, automated volatile SRAM cold-boot firmware recovery, full-duplex SPI image capture, eight-stage enrollment, and opt-in host-side biometric verification.

> [!NOTE]
> Default builds expose standard image capture. Host-side personal authentication requires explicit opt-in (`-Dfte3600_personal_auth=true`). See [Security Policy](SECURITY.md) for details.

---

## Architecture & System Overview

```text
 ┌─────────────────────────────────────────────────────────────┐
 │                Sensor Hardware (FT9361)                     │
 │          64 × 80 px Capacitive Array (SPI + GPIO)           │
 └──────────────┬───────────────────────────────┬──────────────┘
                │ SPI Bus (Mode 0, 1 MHz)       │ Active-High IRQ / Active-Low Reset
                ▼                               ▼
 ┌─────────────────────────────────────────────────────────────┐
 │                 Linux Kernel Subsystems                     │
 │      spidev (bufsiz=32768)     │      gpiochip (libgpiod)   │
 └──────────────┬─────────────────┴─────────────┬──────────────┘
                │                               │
 ┌──────────────▼───────────────────────────────▼──────────────┐
 │                  libfprint: fte3600 Driver                  │
 │                                                             │
 │  ┌────────────────────────┐   ┌──────────────────────────┐  │
 │  │ DMI Fail-Closed Probe  │   │  SRAM Cold-Boot Recovery │  │
 │  │ (A1, GPD Pocket 3,...) │   │  (SHA256-verified 10KB)  │  │
 │  └────────────────────────┘   └──────────────────────────┘  │
 │  ┌───────────────────────────────────────────────────────┐  │
 │  │  Clean-Room Computer Vision Matcher (fte3600-brisk)   │  │
 │  │  - Multi-scale Difference-of-Gaussians (DoG)          │  │
 │  │  - 45-point concentric circular BRISK descriptors     │  │
 │  │  - Rigid/Affine RANSAC consensus (Policy Version 3)   │  │
 │  └───────────────────────────────────────────────────────┘  │
 └──────────────────────────────┬──────────────────────────────┘
                                │
 ┌──────────────────────────────▼──────────────────────────────┐
 │                 fprintd & PAM Integration                   │
 │        Lock Screen Unlock  ·  Console Authentication        │
 └─────────────────────────────────────────────────────────────┘
```

---

## Key Highlights

* **100% Clean-Room Implementation**: Independently written without vendor source code, closed-source blobs, or proprietary template formats. Licensed under `LGPL-2.1-or-later`.
* **Micro-Aperture Matching**: Overcomes the physical limits of standard minutiae matchers (NBIS / `bozorth3`), which fail on tiny 64 × 80 pixel areas (3.2 × 4.0 mm), by using robust sub-pixel DoG keypoints and rotation-normalized binary descriptors.
* **Native Cold-Boot SRAM Recovery**: Automatically detects uninitialized sensor RAM (`MCU status 00 00`) upon cold boot or suspend-resume, injecting size-pinned (10,396 bytes), SHA256-verified firmware directly into volatile sensor SRAM.
* **Fail-Closed Hardware Gating**: Validates motherboard DMI profiles before claiming GPIOs, preventing invalid pin assertions on unverified hardware.
* **Hardened Testing & CI**: Includes comprehensive offline unit tests and a mock asynchronous lifecycle test suite running under AddressSanitizer and UndefinedBehaviorSanitizer without physical hardware.

---

## Supported Hardware

| Platform | Model String (`sys_vendor` / `product_name`) | Status | Branch |
| :--- | :--- | :--- | :--- |
| **One-Netbook A1** | `ONE-NETBOOK TECHNOLOGY CO., LTD.` / `A1` | **Fully Verified** | `main` |
| **GPD Pocket 3** | `GPD` / `Pocket 3` (Jasper Lake & Tiger Lake) | **Supported** | `main` |
| **Medion Akoya E3224** | `MEDION` / `E3224` | **In Progress** (Reverse engineered, awaiting hardware test) | `medion-e3224` |

> [!IMPORTANT]
> Because GPIO pin assignments and polarities vary between motherboards, unknown machines fail closed during probe. See [Hardware Status](docs/fte3600/status.md) to check hardware profiles or contribute a new platform.

---

## Quick Start

### 1. Download and install verified sensor firmware
Run the clean-room installer to fetch the vendor-signed cabinet directly from the Microsoft Update Catalog, verify the SHA256 hash, and install it to `/usr/lib/firmware/fte3600/ft9361.bin`:

```sh
git clone https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600
./scripts/install-firmware.sh
```

### 2. Build and install (Arch Linux)
For Arch Linux users, build the package using the included `PKGBUILD`:

```sh
cd packaging/arch
makepkg -si
sudo reboot
```

For other distributions (Fedora, Ubuntu) or custom Meson builds, refer to the [Installation Guide](docs/fte3600/install.md).

### 3. Enroll and verify
After rebooting and ensuring the `spidev` buffer size is set:

```sh
fprintd-enroll -f left-index-finger "$USER"
fprintd-verify -f left-index-finger "$USER"
```

---

## Documentation Index

| Document | Purpose |
| :--- | :--- |
| **[Installation & Setup](docs/fte3600/install.md)** | Full guide for Arch, Fedora, and Ubuntu: dependencies, firmware setup, Meson compilation, systemd/SELinux permissions, and safe enrollment. |
| **[Hardware Status & Calibration](docs/fte3600/status.md)** | Technical specifications of supported platforms, verified capabilities, and empirical statistical calibration metrics. |
| **[Troubleshooting Guide](docs/fte3600/troubleshooting.md)** | Step-by-step diagnostic workflows: probing hardware, fixing `00 00` status, buffer size issues, and SELinux permission denials. |
| **[Clean-Room & Architecture Rationale](docs/fte3600/clean-room.md)** | Provenance, clean-room boundary, algorithm references, and why the driver derives from `FpDevice` rather than `FpImageDevice`. |
| **[Security Policy](SECURITY.md)** | Threat model, fail-closed hardware protection, biometric privacy, and vulnerability reporting. |
| **[Contributing](CONTRIBUTING.md)** | Coding style, test vector requirements, clean-room standards, and how to submit new hardware profiles. |
| **[Changelog](CHANGELOG.md)** | Detailed release history and milestone notes. |

---

## Upstream & Provenance

This project is maintained downstream of [upstream libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) (baseline commit `c4654fdc85c2`, version `1.94.100`). All FTE3600 driver code, matcher algorithms, firmware recovery routines, and tests are clean-room implementations licensed under `LGPL-2.1-or-later`.

Thanks to upstream `libfprint` and `fprintd` maintainers and contributors. [OpenAI Codex](https://openai.com/codex/) and [Google Antigravity](https://deepmind.google/) substantially assisted implementation, testing, review, and documentation.
