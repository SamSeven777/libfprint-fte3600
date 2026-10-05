# Local validation — 2026-10-04

[Documentation index](README.md)

**Historical checkpoint.** This record covers the sensor-family implementation
at the source revision below, using the former custom kernel SPI bridge. Its
counts and transport observations are not results for current main. See the
[later release validation](validation-release-2026-10-05.md) for the ACPI-glue /
stock-spidev implementation. The original results are preserved here.

Source: implementation commit `019161d3ad8c93562fc99e84d87fa3ca9691a2a5`, based on
`22c51b2f5cf47546b01241af01d7a9b32c02a835`. The checks ran on the matching working
tree before commit; committing introduced no runtime changes. These are local
results, not a remote CI run.

Environment: Ubuntu 24.04 under WSL, GCC 13.3, Meson/Ninja. No target sensor
was present and no kernel module was loaded.

| Build | Test suites | Passed TAP cases | Skipped TAP cases | Failures |
| --- | ---: | ---: | ---: | ---: |
| Personal authentication disabled | 22 | 391 | 1 | 0 |
| Personal authentication enabled | 22 | 412 | 1 | 0 |
| Enabled, ASan + UBSan | 22 | 412 | 1 | 0 |

Each build also passes 16 Python installer cases in the
`fte3600-install-firmware` suite. These are separate from the TAP counts above.
They use generated payloads, temporary directories and simulated archive tools;
no downloads or system firmware installation are performed. Tests cover all
six catalog payloads, hash/size rejection, file races, staging failures,
per-file replacement, and real child-process output limits and timeout cleanup.

The skipped case in each build is `/fte3600-driver/firmware/valid`, which requires
an optional, exact external FT9361 firmware image. The generic firmware loader
has 15 passing generated-data tests per build; no proprietary firmware was
added to the repository. Lifecycle suites pass 91 cases with authentication
disabled and 94 with it enabled, including the complete enrollment/verification
flow using generated mathematical images.

All builds selected `drivers=fte3600`, `werror=true`, `gtk-examples=false`,
`doc=false`, `introspection=false`, `installed-tests=false`. The sanitizer build
also selected `b_sanitize=address,undefined`, `b_lundef=false`; its effective
test environment used Meson's `ASAN_OPTIONS=verify_asan_link_order=0` and
`UBSAN_OPTIONS=halt_on_error=1`. Meson's test setup overrides the calling
shell's ASan options. The selected libfprint library, protocol/backend sources,
matcher and test executables were instrumented, rather than linking new tests
to an uninstrumented libfprint. The recorded claim is address/undefined-behavior
checking; it is not a complete leak audit or hardware correctness proof.

The 22 suites are `fpi-device-cancel`, `fpi-spi-transfer`, `fte3600-context`,
`fte3600-driver`, `fte3600-sensor`, `fte3600-firmware`, `fte3600-protocol`,
`fte3600-lifecycle`, `brisk-core`, `fte3600-brisk`, `fte3600-template`,
`fw9369-protocol`, `ft93xx-protocol`, `ft9368-protocol`, `ft9368-backend`,
`fw9369-backend`, `ft93xx-backend`, `legacy38-recovery`,
`fte3600-special-probe`, `fte3600-family-template`, `fte3600-auth-lifecycle`,
and `fte3600-install-firmware`.
The normal two-build invocation is `scripts/check-fte3600.sh`; after a sanitizer
setup/build, the same test names can be passed to `meson test --print-errorlogs`.
Local results are recorded in `build-fte3600-ci-false/meson-logs/`,
`build-fte3600-ci-true/meson-logs/` and
`build-fte3600-sanitize/meson-logs/`. These generated logs are not source files.

The kernel bridge separately passed `make -C kernel/fte3600 check` and a
`W=1` module build against Ubuntu Linux `6.8.0-146-generic` headers. The build
reported the normal compiler-name difference (both GCC 13.3) and skipped BTF
without `vmlinux`; neither is a hardware result.

The eight protocol/backend suites contribute 133 passing cases in each build: 17 pure
protocol cases, 78 backend cases, 20 legacy RAM recovery cases, and 18 shared
factory negotiation cases. They exercise golden packets, raw image decoding,
chunked transfer limits, bounded calibration, idle verification, RAM/PRAM
readback, explicit flash-update gating, and transfer/cancellation faults.
Negotiation cases include multiple internal failure and cancellation points;
these are not counted as separate TAP cases.

Public lifecycle coverage includes all new chip identities, FT9395 rejection,
changed IDs, CRC failures, FT9368 metadata rejection, positive FT9536 cold
discovery, insufficient transfer limits, alternate CS selection, and chips that
answer ID reads only after C6 negotiation. It also covers backend initialization
failure through the public open/reset/close path. All images and uploaded test
buffers in these new suites are synthetic.

The family-template suite adds 57 passing cases in each configuration. They
cover all eight native image settings, packed/strided equivalence, full-height
40×196 images, principal-axis shape evidence checked against closed-form values,
collinear/near-collinear and low-variance rejection, all 56 directed cross-model
pairs, v1/v2 isolation, and modern matching under non-default rounding modes.
Every profile is swept in five-degree rotation increments for native, reversed
and mosaic comparisons, including minimum-spread and shape boundaries. The
FT9769 counterexample now passes consistently. A weak 12-inlier alignment with
mean descriptor distance above the quality limit cannot introduce its unique
feature into the modern mosaic. Previous v2 policies are explicitly rejected;
v1 bytes and historical behavior remain isolated.
FT9361 is an ordinary v2 geometry setting; only the legacy v1 compatibility
path uses the historical decision rule and format.

The separate public authentication suite passes 26 cases when enabled and
eight disabled-policy cases otherwise. It replaces the discovery/capture
boundary while running the actual libfprint APIs, driver, asynchronous workers,
BRISK core and template codec. Enabled cases enroll eight native images and
verify for every chip, check backend retries and session reuse, verify a real
legacy-v1 template, reject same-sized foreign-chip templates before capture,
and exercise geometry errors, cleanup failure and cancellation. Release-gate
tests cover held contact, a rejected duplicate, recoverable retries, cancellation,
failed cleanup and incorrect success state, with no premature next capture.
An obsolete v2 policy is rejected before capture with a re-enrollment message. The original
wire-level lifecycle suite also runs full enrollment/verification for FT9361,
FT9348, FT9338 and FT9536. These layers do not simulate actual touch waveforms.

Backend release tests cover FW9369 UP/DOWN ambiguity, pending UP retained across
matching, prearm cancellation/failure and terminal-stage idle; FT93xx requires
three consecutive empty-image observations and correctly cleans up even when
cancelled before its first scan. These observations do not prove physical
removal or the behavior of an already-empty sensor when FDT is armed.

Code was formatted using the repository's Uncrustify configuration and
native Git `diff --check` passed. Review corrected the reset wrapper overwriting a
backend state machine's private data, calibration data surviving transport
release, and reopening after a failed kernel SPI-configuration rollback.
The earlier enrollment rearm/session invalidation and capture-only enrollment
rejection regressions remain covered. Firmware read fault injection covers
the fortified `__read_chk` path as well as `read`, preserving Fortify checks.

Still unverified: board GPIO voltages/inverters, every OEM ACPI description,
physical cold boot/upload, new-sensor image quality, touch/removal cadence,
real suspend/resume, thermal behavior and population FAR/FRR.
The support boundaries are listed in [architecture.md](architecture.md).
