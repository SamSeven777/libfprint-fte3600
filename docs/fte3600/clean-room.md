# Implementation and provenance

The FTE3600 driver and host matcher are independently written LGPL-2.1-or-later
code. Windows transport analysis and A1 hardware experiments informed register
framing and the RAM firmware startup sequence. Host code does not execute a
vendor DLL/ELF or use the vendor's descriptor table or template format.

The matcher uses Gaussian/DoG features, orientation-normalized binary
descriptors, and geometric consensus. Its sampling-pair seed is documented in
`fte3600-brisk.h`; persisted templates have explicit format/policy versions.
Algorithm references:

- Lowe, [SIFT](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf), 2004.
- Leutenegger et al., [BRISK](https://doi.org/10.1109/ICCV.2011.6126542), 2011.
- Fischler and Bolles, [RANSAC](https://doi.org/10.1145/358669.358692), 1981.

A1 recovery loads a separately supplied, pinned firmware image onto the sensor;
see [installation](install.md#firmware-for-cold-boot-recovery). The repository
and packages exclude that firmware, vendor binaries/decompiler output, and
private FTE3600 images/templates. Existing upstream test fixtures retain their
own provenance. See [SECURITY](../../SECURITY.md) for reporting boundaries.
