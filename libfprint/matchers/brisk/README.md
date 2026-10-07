# Reusing the BRISK-style core

[Matcher architecture](../../../docs/fte3600/architecture.md) ·
[Documentation index](../../../docs/fte3600/README.md)

`brisk.c` and `brisk.h` implement image normalization, feature extraction and
rigid-match evidence. They depend only on GLib, the C library and libm. They
do not include a device driver, GPIO/SPI API, firmware command, generated
`config.h`, template codec or authentication build option.

Pass original, single-channel 8-bit pixels through `FpiBriskImage`:

```c
FpiBriskImage image = {
  .data = pixels,
  .length = available_bytes,
  .width = width,
  .height = height,
  .stride = bytes_per_row,
};
FpiBriskFeatureSet features;
FpiBriskStatus status = fpi_brisk_extract (&image, &features);
```

The extractor performs one local contrast normalization pass. Do not normalize
the image before extraction. `fpi_brisk_normalize()` is available separately
for image-processing experiments; it also handles overlapping input/output.
Input storage remains owned by the caller, and row padding is neither sampled
nor modified. The supplied length must cover the last row's pixels; trailing
padding on that row is optional.

Extraction and description accept **32–256 pixels per axis**. Normalization
accepts **1–256**. The upper bound limits memory/work and prevents overflow in
the frozen integral-image representation. Invalid dimensions, short buffers,
short strides and overflowing stride arithmetic are rejected before pixels
are read. Features are capped at 160. The core follows GLib's allocation
convention: allocation failure is fatal, rather than a recoverable status.

Compare feature sets by supplying each set's coordinate domain:

```c
FpiBriskMatchEvidence evidence;
FpiBriskStatus status = fpi_brisk_match (
  &query, query_width, query_height,
  &reference, reference_width, reference_height,
  &evidence);
```

Match/validation canvases are limited to **1–1024 pixels per axis**. Larger
canvases support feature mosaics without allocating image buffers. Results
contain correspondence counts, fitted translation/rotation, residuals and
spatial statistics. `FPI_BRISK_OK` indicates a fitted model, **not an identity
or authentication decision**. The frozen correspondence filters and five-match
consensus minimum belong to this algorithm profile; applications must assess
their own decision policy and calibration.

Pixel coordinates, sampling radii and residuals use image pixels. The core
does not infer DPI, rescale sensors or establish that different sensors produce
comparable images. Reuse on other geometries is supported by the interface;
its accuracy on another sensor still needs validation.

Feature sets are in-memory structures. A caller's persistent format must record
the schema and coordinate domain and encode fields explicitly. Schema 3 and
the existing 64×80 extractor output remain unchanged. The driver adapter now
records each supported chip's geometry and processing revision in its template
format and owns the separate decision policy. Other geometries must not be
stored as if they were the original FT9361 profile.

Operations are reentrant with separate output storage. Concurrent calls may
share immutable inputs. Floating-point operations temporarily select
`FE_TONEAREST` in the calling thread and restore its previous rounding mode.
Feature/evidence output storage must not overlap an input; callers must not
modify buffers while an operation is using them.

From the repository root, build and run the independent mathematical tests:

```sh
cc -std=gnu99 -O2 -Wall -Wextra -Werror \
  tests/test-brisk-core.c libfprint/matchers/brisk/brisk.c \
  $(pkg-config --cflags --libs glib-2.0) -lm -o /tmp/test-brisk-core
/tmp/test-brisk-core
```

These tests need no hardware, driver sources, proprietary data or authentication
macro. They cover the legacy extractor golden vector, different dimensions,
padding, overflow rejection, maximum normalization size, overlap, coordinate
validation, matching evidence and concurrent rounding-mode restoration.
