# Implementation and provenance

The FTE3600 driver and host matcher are independently written LGPL-2.1-or-later
code. Windows transport analysis and A1 hardware experiments informed register
framing and the RAM firmware startup sequence. Host code does not execute a
vendor DLL/ELF or use the vendor's descriptor table or template format.

## Why the driver uses a host matcher

The FT9361 supplies only a small 64 × 80 pixel fingerprint image and has no
on-chip matching capability. In this implementation's hardware experiments,
the existing NBIS pipeline did not provide usable matching for that small
capture area. The driver therefore uses a host-side BRISK matcher and derives
from `FpDevice` to implement enrollment, verification and its versioned raw
templates directly. This is a deliberate departure from libfprint's usual
`FpImageDevice` path for image sensors, not a match-on-chip implementation.
The rationale should accompany any upstream architecture review; it does not
establish population-level authentication accuracy.

## Algorithm and firmware provenance

The matcher uses Gaussian/DoG features, orientation-normalized binary
descriptors, and geometric consensus. Its sampling-pair seed is documented in
`fte3600-brisk.h`; persisted templates have explicit format/policy versions.
Algorithm references:

- Lowe, [SIFT](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf), 2004.
- Leutenegger et al., [BRISK](https://doi.org/10.1109/ICCV.2011.6126542), 2011.
- Fischler and Bolles, [RANSAC](https://doi.org/10.1145/358669.358692), 1981.

Cold-boot recovery loads a pinned, SHA256-verified firmware image onto the sensor
from `/usr/lib/firmware/fte3600/ft9361.bin`. The repository
excludes that firmware, vendor binaries/decompiler output, and
private FTE3600 images/templates. Existing upstream test fixtures retain their
own provenance.
