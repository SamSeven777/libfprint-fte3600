# Clean-room and vendor-code boundary

The runtime driver is newly written LGPL-2.1-or-later code. It does not load a
vendor DLL/ELF, upload vendor firmware, embed a proprietary descriptor table,
or serialize a vendor template.

## Runtime pipeline

```text
FT9361 64x80 frame
  -> clean-room two-octave Gaussian/DoG feature detection
  -> orientation assignment and 256-bit BRISK-style descriptors
  -> binary candidate matching
  -> rigid rotation/translation consensus and coverage checks
  -> versioned eight-subtemplate enrollment/verification policy
```

The 45-point sampling layout uses independently documented geometry. Its fixed
256-pair table was generated from the public seed recorded in
`fte3600-brisk.h`; it was not copied from vendor code. Persisted templates
carry extractor and policy versions and are validated with strict bounds,
finite-number, canonical-order, and little-endian checks.

## What static interoperability research established

Static analysis of the matching Windows package established that the sensor
returns a host-processed 64x80 image and that enrollment/verification happen
in a separate WinBio engine adapter. It also established a high-level
DoG/orientation/binary-descriptor/geometric-consensus architecture. Those
observations informed interoperability requirements, not copied source.

Exact proprietary descriptor-pair ordering, template serialization, score
weights, and the mapping from the vendor's verification level to a decision
threshold are not part of this implementation.

## Public algorithm references

The implementation is independently written, but its general computer-vision
building blocks follow these public papers:

- David G. Lowe, [*Distinctive Image Features from Scale-Invariant
  Keypoints*](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf), IJCV 2004 —
  scale space, Difference of Gaussians, and orientation-normalized local
  features;
- Stefan Leutenegger, Margarita Chli, and Roland Y. Siegwart,
  [*BRISK: Binary Robust Invariant Scalable
  Keypoints*](https://doi.org/10.1109/ICCV.2011.6126542), ICCV 2011 — local
  sampling patterns and binary intensity-comparison descriptors;
- Martin A. Fischler and Robert C. Bolles,
  [*Random Sample Consensus*](https://doi.org/10.1145/358669.358692), CACM
  1981 — robust geometric-consensus concepts. This implementation enumerates
  bounded deterministic hypotheses rather than copying a RANSAC routine.

No source code, trained data, thresholds, or descriptor tables were copied
from those publications' reference implementations.

## Material deliberately excluded

- Windows DLL/CAT/INF files and extracted firmware;
- vendor Linux binaries and kernel modules;
- Ghidra projects, decompiler output, and full machine DSDTs;
- real FTE3600 fingerprint frames, templates, descriptors, or image-bearing
  SPI captures;
- the unused vendor-engine loader and older correlation-matcher prototype.

Public issues and pull requests must preserve this boundary.
