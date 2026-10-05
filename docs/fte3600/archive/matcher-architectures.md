# Host matcher architecture

[Documentation index](README.md)

The current implementation separates reusable feature extraction and matching
from sensor communication, stored-template formats and authentication policy.
Both cores depend only on GLib, the C library and libm. Their source files do
not include driver headers, transport APIs, firmware sequences or generated
build-policy headers.

<a id="design-philosophy"></a>

## Core and adapter responsibilities

The cores in `libfprint/matchers/` extract features and return numerical match
evidence. The driver adapter owns captured images, sensor profiles, template
serialization, enrollment and the final authentication decision. A successful
core match is not by itself an identity decision.

The FTE3600 BRISK adapter passes each sensor's native dimensions and row stride.
FT9361 is one setting among eight profiles. The optional 2D-IPA core currently
has a fixed 64 × 80 input interface. Its current driver integration is restricted
to FT9361; the other 64 × 80 profiles do not enable IPA merely because their
dimensions agree. See [family authentication](family-authentication.md)
for the BRISK adapter's profile and template compatibility rules.

<a id="available-host-matchers"></a>

## Available cores

| Core | Input and implementation | API and reuse |
| --- | --- | --- |
| BRISK-style | Variable-size grayscale images; Gaussian/DoG keypoints, orientation-normalized binary descriptors and rigid geometric consensus | [BRISK guide](../../libfprint/matchers/brisk/README.md) |
| 2D-IPA | Fixed 64 × 80 grayscale images; Harris keypoints, continuous Hadamard descriptors, invariant point comparisons and rigid cluster verification | [2D-IPA guide](../../libfprint/matchers/ipa/README.md) |

Meson builds them as the independent `fprint-brisk-core` and `fprint-ipa-core`
static targets. Authentication options belong to the driver build and adapter;
neither core accesses hardware. No comparative accuracy or performance
qualification is implied by the algorithms in this table.

<a id="mathematical-verification"></a>

## Standalone checks

Run from the repository root with a C compiler and GLib development files:

```sh
cc -std=gnu99 -O2 -Wall -Wextra -Werror \
  tests/test-brisk-core.c libfprint/matchers/brisk/brisk.c \
  $(pkg-config --cflags --libs glib-2.0) -lm -o /tmp/test-brisk-core
/tmp/test-brisk-core

cc -std=gnu99 -O2 -Wall -Wextra -Werror \
  tests/test-ipa-core.c libfprint/matchers/ipa/ipa.c \
  $(pkg-config --cflags --libs glib-2.0) -lm -o /tmp/test-ipa-core
/tmp/test-ipa-core
```

These suites use synthetic data to check bounds, deterministic extraction,
transforms, descriptor behavior and concurrency. They require no sensor or
driver mocks. For the integrated driver, run `./scripts/check-fte3600.sh`.
Synthetic checks are regressions, not population FAR/FRR measurements.
