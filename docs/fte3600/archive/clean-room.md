# Implementation, provenance and architecture

[Documentation index](README.md)

The FTE3600 host driver and matcher are independently written
LGPL-2.1-or-later code. Windows transport/binary analysis and A1 hardware
experiments informed register framing and RAM firmware startup. The host code
does not execute a vendor DLL/ELF, use a proprietary host matching library,
or import the vendor's descriptor table or biometric template format.

These statements describe the implementation boundary. They do not claim that
protocol knowledge came solely from bus captures, provide a legal assessment,
or establish a formally documented clean-room separation. Contributions must
identify their sources and must not copy vendor code, decompiler listings,
firmware, or private biometric material into the repository.

## Resource and chip discovery

The independent GPL-2.0-only kernel glue resolves ACPI resources and exposes
one reset GPIO and an IRQ-only UIO device. The LGPL host driver uses standard
spidev, GPIO and UIO interfaces with the glue's pairing metadata; it does not
use the former custom SPI-bridge ioctl ABI. There are no machine/controller/pin
profiles in normal runtime discovery. Technical sources and the supported
subset are documented in [dynamic discovery](dynamic-discovery.md).
This does not assert a formally separated clean-room process.

## Why a host matcher

The FT9361 provides small 64 × 80 images. The author reports that the existing
NBIS path did not give usable matching in A1 experiments. No reproducible
comparative dataset establishes a general minimum sensor size, average
minutiae count, or numerical NBIS false-rejection rate here.

This driver consequently derives from `FpDevice` and implements host-side
enrollment, verification and versioned templates itself; it is not a
match-on-chip driver. The common `FpImageDevice`/NBIS route and this proposed
alternative need upstream architecture review. The rationale does not establish
population-level authentication accuracy.

The BRISK-style implementation combines Gaussian/DoG features,
orientation-normalized descriptors and geometric consensus. Its implementation
and sampling parameters live in the [reusable core](../../libfprint/matchers/brisk/README.md).
References:

- Lowe, [SIFT](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf), 2004.
- Leutenegger et al., [BRISK](https://doi.org/10.1109/ICCV.2011.6126542), 2011.
- Fischler and Bolles, [RANSAC](https://doi.org/10.1145/358669.358692), 1981.

## External device firmware

Each firmware-dependent chip requires its own externally supplied payload.
The [firmware installation guide](install.md#2-install-the-firmware-for-the-identified-chip)
and [hardware inventory](windows-hardware-inventory.md) record the accepted
files, sizes and hashes. FT9361 is one profile, not a firmware default for the
other chips. The sensor executes that vendor firmware; saying that the Linux
host uses no proprietary matching library does not make the device firmware
open source.

Size/hash checks establish that the input equals the expected image, not a
right to redistribute it, a signature-verification claim, or compatibility
with every device sharing the ACPI ID. The repository excludes firmware and
vendor binaries. Acquisition/distribution terms must be checked separately.
Legacy FT9338/FT9536 and FT9348/FT9361 recovery targets volatile RAM.
FT9368 has a separate, explicitly requested update path that writes application
flash after identifying a running application; see [FT9368 protocol](ft9368-protocol.md).
These are different operations. No OTP programming is implemented.

## Privacy and validation limits

Major owned capture/template buffers are explicitly cleared on release.
That is not comprehensive erasure: temporary feature copies, worker stacks,
GLib/GBytes serialization copies, process dumps and framework-managed storage
may retain biometric-derived data. Do not publish real images, templates,
descriptors or memory dumps. Existing upstream fixtures retain their own
provenance. See [hardware and validation status](status.md) for evidence limits.
