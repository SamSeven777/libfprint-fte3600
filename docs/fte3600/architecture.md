# FTE3600 Architecture and Implementation Design

[Documentation Index](README.md) · [Wire Protocols](protocols.md) · [Installation](install.md)

This document describes the design architecture, module boundaries, dynamic chip discovery, host matchers, and clean-room provenance of the `libfprint-fte3600` driver stack.

---

## 1. System Layers & Data Flow

```text
[ Linux Kernel ]
  ├── spidev (/dev/spidevX.Y)      ──> Raw SPI duplex transfers
  └── fte3600.ko (/dev/uioX)       ──> Reset GPIO (cdev v2) & IRQ event notification
                                              │
[ libfprint FTE3600 Driver ]                  ▼
  ├── Transport Layer              ──> Verified SPI/GPIO/UIO triple-node pairing
  ├── Discovery & Catalog          ──> Runtime identity handshake & geometry selection
  ├── Protocol State Machines      ──> A8 / FW9369 / FT9365 / FT9769 capture pipelines
  └── Matching Adapter             ──> Multi-frame enrollment, canvas stitching & auth
                                              │
[ Host Biometric Matchers ]                   ▼
  ├── BRISK Core (Default)         ──> Scale-adaptive keypoint & 64-bit Hamming matching
  └── 2D-IPA Core (Alternative)    ──> Fixed-geometry 64×80 minutiae matching
```

---

## 2. Dynamic Discovery & Sensor Identification

The driver avoids DMI/board whitelists by dynamically identifying the connected sensor at runtime using a multi-stage handshake:

1. **Hardware Reset**: Trigger logical reset pulse via kernel module.
2. **Special Wake Check**: Send `0x5A` and execute C6 mode negotiation; detect `FW9369` (`0x9362`) or `FT9368`.
3. **Word ID Check**: Read 16-bit registers to identify `FT9365` (`0x9365`) or `FT9769` (`0x9391`/`0x9392`).
4. **Runtime Geometry / Boot Check**: Read legacy registers `0x14/0x15` to detect:
   - `0x6060` $\implies$ **FT9348** ($96 \times 96$)
   - `0x4050` $\implies$ **FT9361** ($64 \times 80$)
   - `0x5858` $\implies$ **FT9338** ($88 \times 88$, requires SRAM firmware load)
   - `0x4080` $\implies$ **FT9536** ($64 \times 128$, requires SRAM firmware load)

---

## 3. Host Biometric Matcher (BRISK)

FocalTech hardware sensors stream raw capacitive pixel arrays and do not execute on-chip matching firmware. Authentication is handled on the host:

- **Keypoint Detection**: AGAST corner detection with multi-scale octave filtering.
- **Descriptor Extraction**: 512-bit (64-byte) bitwise descriptor vectors.
- **Matching Acceleration**: Accelerated 64-bit Hamming distance computation (`__builtin_popcountll`) with geometric RANSAC consistency checks.
- **Adaptive Canvas**: Dynamically stitches multiple enrollment impressions to accommodate small/narrow form-factor sensors (such as FT9769 $40 \times 196$).

---

## 4. Physical Resolution & Metadata

Resolution metadata is recorded in pixels per millimeter (`ppmm = DPI / 25.4`):
- **508 DPI (20.0 ppmm)**: FT9338, FT9348, FT9361, FT9536 (50 µm pixel pitch).
- **552 DPI (21.73 ppmm)**: FT9365.
- **564 DPI (22.20 ppmm)**: FT9769 (ultra-narrow side power key).
- **Unknown (0.0 ppmm)**: FT9368, FW9369 (uncalibrated / not publicly specified).

---

## 5. Clean-Room Provenance

- **Independent Codebase**: All driver and matcher logic is an original, clean-room implementation licensed under LGPL-2.1-or-later.
- **No Proprietary Code**: Contains zero vendor binaries, decompiled source code, or proprietary algorithms.
- **External Firmware**: Proprietary sensor application blobs remain external in `/usr/lib/firmware/fte3600/` and are never embedded into `libfprint` binaries.
