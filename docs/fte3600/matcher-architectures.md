# Experimental FTE3600 matcher modes and audit guide

This branch adds an experimental geometric point/descriptor matcher (called
2D-IPA here) to the BRISK path. It is separate from the BRISK implementation
proposed upstream. Neither method has a deployment-level population accuracy
evaluation in this repository.

## Algorithms and decision boundaries

BRISK uses DoG keypoints, orientation-normalized binary descriptors and geometric
consensus. IPA first applies local contrast normalization, then finds
gradient/structure-based points with sub-pixel peak interpolation and builds
continuous descriptors. It checks rigid geometric consistency using local
orientations and two-point spatial rotation hypotheses. Descriptor context uses
a fixed normalized Hadamard basis, not trained parameters. The name does not
establish equivalence to a published learned model, general affine invariance
or superior biometric accuracy. Follow the implementation and its documented
parameter provenance.

Runtime modes are BRISK, IPA and OR fusion. In dual mode either permitted engine
can accept, so adding IPA is a change to the authentication boundary, not a
cross-validation requirement that both engines agree. Single-pair metrics do
not establish the behavior of the eight-subtemplate gallery and retries.

## Explicit build gates

- `fte3600_personal_auth=false` (default): no authentication.
- `fte3600_personal_auth=true`, `fte3600_ipa_auth=false` (default):
  experimental BRISK authentication only; IPA diagnostics remain available.
- Both options true: explicitly opt into experimental IPA and dual decisions.
  Setting IPA authentication true without personal authentication is rejected.

The same flags apply to all driver, template, matcher and test translation
units. A source-local macro must not bypass these gates. The default runtime
mode is BRISK without IPA authorization and dual only with both opt-ins.
Explicit IPA/dual authorization requests are rejected when their build gate is
closed; unknown mode names are errors, not a silent fallback.

These flags do not certify the selected policy. Keep experiments out of
system-wide sudo/root authentication and preserve a password fallback.

## Templates

BRISK-only Wire V1 remains version checked. The old experimental Wire V2 format
is rejected; it did not carry a complete IPA compatibility contract.
IPA-containing records use Wire V3 with a 48-byte header and independent IPA
extractor, diagnostic, authentication and fusion policy versions. The current
contrast-normalized/sub-pixel extractor schema and diagnostic policy are both
version 2. The IPA authentication policy is version 2 only with its explicit
build opt-in, otherwise 0; fusion policy is version 1. The earlier experimental
IPA schema is not interchangeable with these descriptors. Upgrade tests must check
mismatches and mixed BRISK-only/IPA records. Re-enroll when the driver reports
an incompatible experimental template; do not relabel old records to bypass
version checks. Inspect the current header constants rather than hard-coding
serialized offsets in applications.

## Running tests without changing a login service

Configure and test each legal option pair in its own build directory:

```sh
meson setup build-capture -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=false -Dfte3600_ipa_auth=false \
  -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson test -C build-capture --suite=unit-tests --print-errorlogs

meson setup build-brisk -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=true -Dfte3600_ipa_auth=false \
  -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson test -C build-brisk --suite=unit-tests --print-errorlogs

meson setup build-dual -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=true -Dfte3600_ipa_auth=true \
  -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson test -C build-dual --suite=unit-tests --print-errorlogs
```

The IPA, template and lifecycle tests use synthetic inputs. They do not require
hardware or real fingerprints. All registered unit tests, including
`fte3600-ipa`, belong in the corresponding CI runs.

`FP_FTE3600_MATCHER` is read by the process loading this driver. Prefixing
`fprintd-verify` with that variable affects only the D-Bus client, **not** the
already running or D-Bus-activated fprintd service; it does not select the
service's matcher mode. Use the in-process tests/benchmark for controlled mode
comparisons. Service experiments require setting the environment of the actual
daemon, recording and restoring its prior configuration, and confirming its
effective mode. Do not change a login service just to run this benchmark.

## Synthetic performance benchmark

Only personal-auth builds register the real Meson benchmark:

```sh
meson test -C build-brisk --benchmark --logbase=benchmark --print-errorlogs benchmark-fte3600
meson test -C build-dual --benchmark --logbase=benchmark --print-errorlogs benchmark-fte3600
```

The program times extraction and complete gallery comparisons, prints the
actual encoded size and observed decisions, and fails on API errors or its
explicit self/different-pattern smoke expectations. Transformed-pattern
decisions are descriptive observations, not guaranteed successful matches.
Capture-only builds have no authentication benchmark. No hard latency threshold
is asserted, so sanitizer and slower machines may legitimately be slower.

The output reports a small fixed synthetic fixture set, not real population
FAR/FRR. It does not measure heap allocation counts or justify a general
rotation/accuracy guarantee. Earlier fixed zero-FAR and sub-2-ms summaries must
not be used as evidence. Reproduce timing on a recorded CPU, compiler, commit
and options; evaluate real biometric accuracy separately with a documented
independent dataset and the complete deployment decision process.

