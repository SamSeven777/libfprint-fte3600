# Sensor-parameterized BRISK enrollment and verification

[Documentation index](README.md)

All eight implemented capture profiles feed their native 8-bit images to the
same BRISK core. The image interface passes data length, width, height and row
stride. There is no FT9361 reference-image conversion, resampling to 64×80,
chip-specific extractor, or hardware access inside the matcher.

## Image settings and ownership

`fte3600-match-profile.{c,h}` records each chip's persistent model identifier,
native dimensions and image-processing revision. FT9361 is one ordinary entry.
The driver checks the captured image against the selected sensor's dimensions
and gives each asynchronous worker an exact-sized owned copy plus an immutable
profile. Workers do not read live device state. Image copies are cleared when
consumed or cancelled; this is not a complete memory-erasure claim.

The generic core accepts arbitrary supported dimensions independently of this
registry. The registry is the driver's template-compatibility boundary, not a
computer-model admission table. It distinguishes equal-sized chips such as
FT9361, FT9365, FT9368 and FW9369. Raw IDs 9391/9392 share the FT9769 setting
after the capture backend converts their sample ordering to the same image
layout. Model 9369 identifies the FW9369 profile, whose raw silicon ID is 9362.

## Enrollment and matching

Enrollment still collects eight distinct samples; every later sample must
pass against at least one already accepted sample. The template constructs
its mosaic in a canvas three times the native width and height, anchored by
one native width/height. Matching compares the query with
the eight samples and the canonical mosaic. Both query and reference coordinate
domains are passed explicitly to the core. No sensor DPI or cross-device pixel
scale equivalence is inferred.

New templates use diagnostic policy 7 / optional authentication policy 8.
Descriptor-distance, correspondence, residual and competing-fit gates retain
their numeric thresholds. Spatial quality uses the inlier covariance's
principal variances `minor <= major` and the sensor's own short and long sides
`S <= L`. For both query and reference, the shape score is
`min(1, (minor / major) * (L / S)^2)` and must be at least 0.03. This compares
minor variance per short-side squared with major variance per long-side
squared. The cap preserves monotonicity: adding transverse spread must not
reduce the shape score. Covariance eigenvalues are unchanged by rigid
rotation, so rotating a rectangular sensor's feature set does not change
these gates. No image resampling is involved.

Both sides must also have `minor >= 3` and `major >= 64/12` in pixel-squared
units. These correspond to principal equivalent spreads `sqrt(12*variance)`
of at least six and eight pixels, using the variance of a uniform interval.
They replace the old axis-span and occupied-grid decision gates; the raw spans
and grid counts remain diagnostic evidence. Exactly/near-collinear and
insufficiently spread matches remain rejected. A mosaic uses the same sensor
aspect ratio: its larger storage canvas does not change pixel scale. The
generic core provides covariance as numerical evidence; the adapter owns the
principal-axis normalization and decision.

Modern mosaic construction requires every connecting match to pass the full
diagnostic quality gate, even in a build with authentication disabled. An OK
rigid fit and sufficient inlier count alone cannot add another sample's
coordinates or descriptors. Samples without an accepted path from the
canonical anchor remain in the individual gallery but do not enter its mosaic.

These are experimental settings with synthetic regression coverage. They are
not fitted to a real multi-person data set and do not establish FAR/FRR or
liveness. The build option `fte3600_personal_auth=true` enables enrollment and
verification for all eight positive sensor identities. With the option off,
all profiles remain capture-only.

## Persistent compatibility

New enrollments use wire version 2. Its canonical little-endian 40-byte header
and bounded eight-record payload retain the existing framing:

| Offset | Field |
| --- | --- |
| 0 | Eight-byte magic |
| 8, 10, 12 | u16 wire version, u16 header length, u32 total length |
| 16, 18, 20, 22 | u16 model, width, height, feature-record length |
| 24, 26, 28 | u16 extractor, diagnostic-policy, authentication-policy versions |
| 30 | u16 sample count, exactly eight |
| 32 | u32 flags, zero |
| 36 | u32 processing revision, currently one |

Extractor schema 3 and the 44-byte feature encoding remain unchanged.
Unknown model, wrong dimensions, unsupported processing revision/policy,
noncanonical records and malformed lengths fail decoding. The driver additionally
compares the decoded profile with the current sensor **before capture**. A
same-sized template from another chip is not accepted. The comparison API
also requires an explicit query profile.

Wire version 1 remains an isolated compatibility path for existing FT9361
templates with diagnostic policy 5 / authentication policy 6 and reserved
processing metadata zero. It preserves the old pixel-space shape test,
historical mosaic reconstruction and canonical bytes. This legacy format does not define the settings used by new
templates; version-2 FT9361 enrollment uses the same parameterized path as the
other seven chips. Older extractor/policy revisions still require re-enrollment.
Wire-v2 templates carrying diagnostic policy 6 / authentication policy 7
(or diagnostic-only authentication version zero) are rejected with an
unsupported-policy result and require re-enrollment. Their axis-scaled shape
test and permissive mosaic connections are not silently reinterpreted under
the new rules. Changing header version fields is not a template migration.

## Retry and failure behavior

A backend may report that a frame is unsuitable after restoring verified idle.
During enrollment this reports a retry at the current stage and retains prior
samples. During verification it reports a retry through libfprint's verification
report API. Cancellation stops the action; failed cleanup invalidates the
session and cannot be retried as if the sensor were ready. Reopening must
establish a new hardware state.

Software checks cover native/strided extraction, all profile dimensions,
version-1 compatibility, version-2 canonical round trips, cross-chip rejection,
degenerate geometry, principal-spread and shape-score boundaries, native/mosaic
rotation sweeps, rejection of weak mosaic connections, worker image bounds,
asynchronous cancellation, and public
enroll/verify/retry flows. Real images, electrical behavior, cold recovery and
population accuracy remain separate hardware-evaluation work.
