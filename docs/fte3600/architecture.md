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

The complete factory sequence is used for initial enumeration and whenever the
kernel glue reports a new generation. A successful discovery caches the exact
identity, glue path, generation, and working CS polarity. A later open may reuse
that backend only while the ACPI companion and generation remain unchanged.
Suspend/resume, a stale reset lease, rebind, failed initialization, or a failed
runtime session invalidates the cache and restores full factory discovery.

Fast open skips only the redundant cross-family search. The selected backend
still performs its own wake, identity validation, configuration, and image
calibration. Legacy backends also retain the vendor 350 ms post-wake geometry
settle before checking `0x14/0x15`.

FW9369 fast open first performs the existing factory physical H10/L20/H reset
and 10 ms settling, before reading SPI configuration or `0x1a8b`. Its previous
close enters C1 deep sleep, which a cached identity does not wake. A reset
failure or cancellation finishes the pulse, invalidates the session, and skips
runtime cleanup commands until a later full discovery establishes communication.

---

## 3. Host Biometric Matchers

FocalTech hardware sensors stream raw capacitive pixel arrays and do not execute on-chip matching firmware. Authentication is handled on the host:

- **Keypoint Detection**: AGAST corner detection with multi-scale octave filtering.
- **Descriptor Extraction**: 512-bit (64-byte) bitwise descriptor vectors.
- **Matching Acceleration**: Accelerated 64-bit Hamming distance computation (`__builtin_popcountll`) with geometric RANSAC consistency checks.
- **Adaptive Canvas**: Dynamically stitches multiple enrollment impressions to accommodate small/narrow form-factor sensors (such as FT9769 $40 \times 196$).

Personal authentication uses BRISK alone unless the build explicitly enables
`-Dfte3600_ipa_auth=true` as well as `-Dfte3600_personal_auth=true`. That optional
build enables Grand Synergy v3: BRISK and 2D-IPA feature extraction with dual
fusion as the default verification mode. `FP_FTE3600_MATCHER=brisk`, `ipa`, or
`dual` selects the verification strategy in the process loading libfprint.

The IPA adapter accepts the registered FT9361 and FW9369 (raw ID `0x9362`)
profiles, both with native 64 × 80 grayscale images. It does not infer support
from image dimensions alone. Other profiles continue to use their own BRISK
policy in dual mode; explicitly selecting IPA alone reports an unsupported
policy for those profiles. No sensor commands, initialization, or image
calibration steps change when opting into a different host matcher.

Templates retain the sensor identity, geometry, and processing revision.
FT9361 dual templates preserve wire v3 and its existing policy; FW9369 dual
templates use wire v4 with the FW9369 BRISK policy. A template from one sensor
profile cannot authenticate another, including these two equal-size profiles.
BRISK-only enrollment keeps wire v2. Existing BRISK templates remain usable
with BRISK or dual verification in an IPA-enabled build, but adding IPA
features requires new enrollment. Selecting IPA alone cannot use features
missing from an old BRISK template.

FW9369 IPA and fusion are experimental. Synthetic lifecycle and template tests
exercise extraction, persistence, policy selection, and sensor isolation;
accuracy and thresholds still require real GPD Pocket 3 data and testing.

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
