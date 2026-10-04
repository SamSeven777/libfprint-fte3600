# Reusing the 2D-IPA (2D Invariant Point Attention) core

`ipa.c` and `ipa.h` implement an independent, host-based biometric feature
extractor and rigid point-set matcher. They depend only on GLib, the C library,
and libm. They do not include device drivers, GPIO/SPI/USB transport code,
firmware command sequences, generated `config.h`, serialized template containers,
or hardware authentication gates.

## Architectural Overview

1. **Local Contrast Normalization (LCN)**:
   Applies zero-mean integral-image local contrast normalization with regularized
   noise floors, ensuring robustness against ambient temperature and skin contact variations.

2. **Structure Tensor & Sub-Pixel Harris Corner Detection**:
   Computes spatial image gradients ($I_x, I_y$) and builds the structure tensor
   matrix $\begin{bmatrix} S_{xx} & S_{xy} \\ S_{xy} & S_{yy} \end{bmatrix}$ over Gaussian windows.
   Harris corner responses are evaluated with sub-pixel quadratic peak interpolation.
   Duplicate refined locations within 1.5 pixels are deterministically suppressed.

3. **Rotation-Invariant Sylvester-Hadamard 32D Descriptors**:
   Local patch context is projected onto a normalized Sylvester-Hadamard $H_{32}$
   basis ($H[i, j] = (-1)^{\text{popcount}(i \land j)} / \sqrt{32}$). Descriptors are
   steered according to the dominant orientation $\theta \in [-\pi, \pi]$.

4. **2D Invariant Point Attention & SE(2) Bearing Consistency**:
   Minutiae pairs are compared using cross-descriptor cosine similarities and
   spatial relative vector bearing checks invariant under $SE(2)$ rigid transformations
   (translation + rotation).

5. **Global Rigid Cluster Verification**:
   Hypothesized spatial rotation and translation transforms are clustered to find
   the maximum mutually consistent inlier set.

## API Usage

Pass original single-channel 8-bit pixels (64×80 resolution):

```c
#include "matchers/ipa/ipa.h"

FpiIpaFeatureSet features;
FpiIpaStatus status = fpi_ipa_extract (pixels, length, &features);
if (status == FPI_IPA_OK)
  {
    /* Extracted features.n_minutiae points with 32D descriptors */
  }
```

Compare two feature sets:

```c
FpiIpaMatchEvidence evidence;
FpiIpaStatus status = fpi_ipa_match (&query_features, &ref_features, &evidence);
if (status == FPI_IPA_OK)
  {
    gboolean pass = fpi_ipa_result_meets_policy (&evidence);
    /* evidence.n_supported_inliers, evidence.consensus_score, evidence.x_span, evidence.y_span */
  }
```

## Standalone Mathematical Tests

From the repository root, build and run the standalone mathematical test suite:

```sh
cc -std=gnu99 -O2 -Wall -Wextra -Werror \
  tests/test-ipa-core.c libfprint/matchers/ipa/ipa.c \
  $(pkg-config --cflags --libs glib-2.0) -lm -o /tmp/test-ipa-core
/tmp/test-ipa-core
```

These tests require no hardware, driver sources, or proprietary libraries.
They verify bit-exact determinism, parameter validation, tied-peak deduplication,
translation and rotation invariance, orthonormal basis projection, and multi-threaded reentrancy.
