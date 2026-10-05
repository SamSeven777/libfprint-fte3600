# Host Matcher Architecture: Algorithm and Driver Decoupling

The `host-matcher` branch establishes a clean architectural separation between
host-based fingerprint biometric matching algorithms and hardware sensor drivers.

## Design Philosophy

Traditionally in match-on-host drivers, matching logic, image processing,
wire protocols, and hardware abstraction were tightly coupled within monolithic
driver sources. In this architecture:

1. **Algorithm Independence (`libfprint/matchers/`)**:
   - Biometric feature extraction and matching algorithms live in independent
     subdirectories under `libfprint/matchers/`.
   - Each matcher (`matchers/brisk/`, `matchers/ipa/`) compiles as an isolated
     static library (`fprint-brisk-core`, `fprint-ipa-core`).
   - Matchers depend **only on GLib, standard C, and libm**.
   - They contain **zero** device driver code, GPIO/SPI/USB APIs, hardware registers,
     firmware sequences, or platform-specific authentication policy definitions.
   - Algorithms can be audited, benchmarked, and mathematically verified in pure
     isolation without any hardware or mock driver wrappers.

2. **Driver Adapters (`libfprint/drivers/fte3600-*`)**:
   - The hardware driver acts solely as a consumer of matcher cores.
   - The driver layer owns hardware communication, frame capture, sensor geometry
     profiling, template container serialization, and authentication decision policy gates.

## Available Host Matchers

| Matcher Core | Directory | Key Techniques | Primary Strengths |
| :--- | :--- | :--- | :--- |
| **BRISK Core** | `libfprint/matchers/brisk/` | Difference-of-Gaussians (DoG) keypoints, circular concentric pattern sampling, orientation-normalized binary Hamming descriptors, RANSAC rigid consensus | Extremely fast execution, lightweight memory footprint, robust across standard touch geometries |
| **2D-IPA Core** | `libfprint/matchers/ipa/` | Local Contrast Normalization (LCN), Structure Tensor image gradients, Harris corner response with sub-pixel quadratic peak interpolation, normalized Sylvester-Hadamard 32D continuous descriptors, Invariant Point Attention with SE(2) vector bearing checks, rigid cluster verification | Sub-pixel spatial resolution, continuous patch representation, enhanced discriminative power on low-ridge-count sensors |

## Mathematical Verification

Each host matcher includes an independent test suite in `tests/`:

- `tests/test-brisk-core.c`: Verifies BRISK contrast normalization, golden extraction vectors, deterministic hashing, coordinate bounds, and affine consensus.
- `tests/test-ipa-core.c`: Verifies 2D-IPA parameter validation, bit-exact determinism, tied-peak sub-pixel deduplication, translation and rotation invariance, Hadamard basis orthonormality, and multi-threaded reentrancy.

Neither test suite requires a physical sensor or driver mocks. Both can be compiled
and executed standalone:

```sh
# Standalone BRISK verification
cc -std=gnu99 -O2 -Wall -Wextra -Werror tests/test-brisk-core.c libfprint/matchers/brisk/brisk.c $(pkg-config --cflags --libs glib-2.0) -lm -o /tmp/test-brisk-core
/tmp/test-brisk-core

# Standalone 2D-IPA verification
cc -std=gnu99 -O2 -Wall -Wextra -Werror tests/test-ipa-core.c libfprint/matchers/ipa/ipa.c $(pkg-config --cflags --libs glib-2.0) -lm -o /tmp/test-ipa-core
/tmp/test-ipa-core
```
