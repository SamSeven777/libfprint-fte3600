# Implementation, provenance and architecture

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
orientation-normalized descriptors and geometric consensus. The sampling-pair
seed is recorded in `fte3600-brisk.h`. References:

- Lowe, [SIFT](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf), 2004.
- Leutenegger et al., [BRISK](https://doi.org/10.1109/ICCV.2011.6126542), 2011.
- Fischler and Bolles, [RANSAC](https://doi.org/10.1145/358669.358692), 1981.

## External device firmware

Cold-boot recovery expects an external image at
`/usr/lib/firmware/fte3600/ft9361.bin`, exactly 10,396 bytes, SHA256
`027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.
The sensor executes that vendor firmware; saying that the Linux host uses no
proprietary matching library does not make the device firmware open source.

Size/hash checks establish that the input equals the expected image, not a
right to redistribute it, a signature-verification claim, or compatibility
with every device sharing the ACPI ID. The repository excludes firmware and
vendor binaries. Acquisition/distribution terms must be checked separately.
The implemented recovery targets volatile RAM, not persistent flash/OTP.

## Privacy and validation limits

Major owned capture/template buffers are explicitly cleared on release.
That is not comprehensive erasure: temporary feature copies, worker stacks,
GLib/GBytes serialization copies, process dumps and framework-managed storage
may retain biometric-derived data. Do not publish real images, templates,
descriptors or memory dumps. Existing upstream fixtures retain their own
provenance. See [hardware and validation status](status.md) for evidence limits.
