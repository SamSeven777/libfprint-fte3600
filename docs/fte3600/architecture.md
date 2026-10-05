# FTE3600 implementation boundaries

[Documentation index](README.md)

This document describes the current module boundaries and runtime contracts.
For installation use [install.md](install.md); for implemented chips and
hardware evidence use [status.md](status.md). The implementation does not
reproduce the full Windows lifecycle; the
[coverage record](windows-lifecycle-coverage.md) distinguishes confirmed fixes
from independent Linux strategies and remaining evidence gaps.

The implementation selects an established protocol from observed sensor
responses. It does not use a computer-model whitelist or try different firmware
images until one responds. The independent host implementation follows the
documented technical observations; it does not include vendor code, firmware,
or a vendor matching library.

## Physical modules

| Boundary | Files | Responsibility |
| --- | --- | --- |
| Kernel resource driver | `kernel/fte3600/` | ACPI resources, one reset GPIO, IRQ-only UIO, exclusive leases and suspend/removal; no sensor SPI commands |
| libfprint integration | `drivers/fte3600.c` | Probe/open/close, action completion, per-device features and host matching jobs |
| Transport | `drivers/fte3600-transport.c`, `fte3600-resources.{c,h}` | Verified SPI/GPIO/UIO pairing, metadata ABI 2, standard spidev transfers, IRQ notifications, logical reset and buffer lifetime |
| Identification | `drivers/fte3600-discovery.c`, `fte3600-special-probe.{c,h}`, `fte3600-sensor.{c,h}` | Runtime/ROM identity evidence, bounded factory mode negotiation, immutable geometry, protocol and firmware metadata |
| Protocol execution | `drivers/fte3600-backends.c`, `fte3600-legacy.c`, `fte3600-legacy-recovery.c`, `fte3600-fw9369.c`, `fte3600-ft93xx.c`, `fte3600-ft9368.c`, `fte3600-ft9368-update.c` | Backend routing, initialization, calibration, capture and bounded cleanup |
| Wire definitions | `drivers/fte3600-protocol.{c,h}`, `fte3600-legacy-recovery-protocol.{c,h}`, `fte3600-fw9369-protocol.{c,h}`, `fte3600-ft93xx-protocol.{c,h}`, `fte3600-ft9368-protocol.{c,h}`, timing headers | Opcodes, registers, framing, sample decoding and pure image transforms; no device access or matcher dependency |
| Reusable algorithms | `matchers/brisk/brisk.{c,h}`, `matchers/ipa/ipa.{c,h}` | Standalone image normalization, features, descriptors, correspondences and numerical rigid-match evidence |
| FTE3600 matching adapter | `drivers/fte3600-match-profile.{c,h}`, `fte3600-brisk.{c,h}`, `fte3600-template.{c,h}` | Sensor image identities, versioned template codec, variable-size mosaics and authentication policy |
| Build definitions | `drivers/fte3600/meson.build`, `fte3600-build-config.h`, `matchers/brisk/meson.build`, `matchers/ipa/meson.build` | Driver source composition, authentication policy switch, independent core targets |

Paths in this table are relative to `libfprint/` except `kernel/`. The parent
Meson files select the driver and register tests. `fte3600-private.h` defines the
private device state/backend contract; it stores opaque template pointers and
does not import the matching implementation into transport or protocol code.

```text
ACPI resources -> reset GPIO + IRQ-only UIO -> transport -> discovery + sensor catalog
physical SPI -> stock spidev --------------/         \-> selected protocol state machine
                                                       |
                                                image (size/stride)
                                                       |
                                  libfprint action / FTE3600 template adapter
                                                       |
                                             generic BRISK core
                                                       |
                                        numerical evidence -> caller policy
```

The wire module and matcher core can be built/tested without a sensor. The
matcher core depends only on GLib and libm and does not include libfprint,
driver headers, GPIO, SPI, firmware, or authentication build switches. Its
[reuse guide](../../libfprint/matchers/brisk/README.md) describes ownership,
supported sizes, concurrency and a standalone compile command.

## API and compatibility

`FpiBriskImage` supplies bytes, buffer length, width, height and row stride.
Feature coordinates are validated against explicit image/canvas dimensions.
`FpiBriskMatchEvidence` reports correspondences, consensus, residuals, spatial
coverage and transform estimates; success is not an identity decision.
Extraction supports 32–256 pixels per axis and matching coordinates up to
1024 per axis. Pixel scale must agree; no sensor DPI is inferred.

Extractor schema 3, the old 64×80 descriptor output, template wire version 1
and existing FT9361 authentication policy remain compatible. Golden vectors
and both policy builds test that contract. All eight identified chips now use
the existing BRISK core for enrollment and verification. New enrollments use
wire version 2, recording model, geometry and processing revision; wire version
1 remains readable only as the original FT9361 profile. A different sensor's
template is rejected before capture, including sensors with equal dimensions.
Workers own exact-sized image buffers and immutable profile snapshots.

The modern policy uses covariance principal variances scaled by each sensor's
long/short-side ratio, so rotating an unchanged correspondence set does not
change the spatial quality decision. Every chip, including FT9361, is a
geometry setting for the same matcher; images are not resized or cropped to
an FT9361 reference. Minimum principal spread, residual and descriptor gates
remain in force, including when joining samples into a mosaic. Diagnostic
policy 7 / optional authentication policy 8 replaces the earlier axis-dependent
v2 policy; those earlier v2 templates require re-enrollment. Only the wire-v1
compatibility path retains the historical decision and reconstruction policy.
This is an explicit adapter policy, not a core authentication decision or
population calibration. See [family authentication](family-authentication.md).

## Implemented support and authorization

The [support matrix](status.md#implemented-functions-and-test-limits) records
each sensor's image size, capture path and recovery boundary. Implemented
behavior and software coverage do not establish hardware compatibility.
The eight catalog entries represent six observed Windows protocol
families. Linux executes all six through four backend modules. Legacy shares
three immutable parameter sets (9338, 9536, A8); 9365/9769 share a configurable
host scanner. The A8 set selects geometry and firmware separately for 9348 and
9361. A backend declares raw bytes per sample, frame storage overhead, and
largest individual SPI transaction separately from final 8-bit image geometry.
Backend preparation/destroy only allocate or clear host state and perform no I/O.

Enumeration identifies the chip before publishing features; open rechecks the
identity. A8 firmware writes require a matching ROM+SPI-OTP identity. FT9368
persistent updates require explicit `FTE3600_FT9368_UPDATE=1`, a positive 9368
application identity, and both exact size/hash-validated firmware files.
Runtime geometry alone is insufficient. Unknown identities, conflicting responses, unimplemented
backends and undersized transport limits fail before capture/upload.

Legacy firmware defaults are `/usr/lib/firmware/fte3600/ft9338.bin`,
`ft9348.bin`, `ft9361.bin` and `ft9536.bin`. The existing
`FTE3600_FIRMWARE_PATH` override is still checked against the selected sensor's
metadata. FT9368's explicit updater instead reads `fte3600/ft9368-app.bin` and
`fte3600/ft9368-pramboot.bin` under `FTE3600_FIRMWARE_DIR` (default
`/usr/lib/firmware`). The installation helper accepts explicit chip selection
and validates all six catalog payloads, including the FT9368 pair, before
installing externally obtained files. Each file replacement is atomic; the
pair is not a filesystem transaction. Firmware is not redistributed here.

Discovery follows the normal Windows SPI factory rounds zero and one.
For each available CS, it tries FT9368 wake/full information twice on a negative
reply, using the factory's 5 ms wake delay. Then it runs special-family wake,
two independent C6 configuration calls (31 attempts each), and confirmed ID.
A negative special result causes H10/L20/H plus 10 ms and one same-CS retry;
a second negative does not add another reset. FT9391 also needs variant
validation. Unknown-family FD/FE pad-voltage registers are never written.

After both factory rounds return negative, legacy detection sends
`70` / 5 ms / `70`, then reads MCU status immediately. Non-idle status retries
up to six rounds, 5 ms apart; idle status waits 350 ms before confirmed geometry.
The direct identity and immediate-geometry shortcuts are removed. A started
pair finishes before observing cancellation; transfer errors stop immediately.
See [dynamic discovery](dynamic-discovery.md) and
[special-family evidence](special-probe.md).

Linux retains capability-gated alternate CS, repeated identity validation and
explicit I/O/cancellation handling. Later reference factory branches that guess
a chip and load firmware without positive identity are excluded. Eligible
unknown devices instead reach the existing reliable ROM checks. Explicit
boot-only discovery retains its ROM sequence. The earlier A1 and GPD hardware
reports do not establish the new sequence's behavior or latency.

FT9338/FT9536 RAM recovery has separate four-byte boot-register framing and
compares every uploaded byte against a complete readback before restarting.
Boot-B authorization preserves the current session's positive runtime pair and
checks the matching SPI OTP high nibble. A cached device name from enumeration
does not satisfy that gate. No unknown A8 family is reclassified as boot-B38.
See [legacy-family recovery](legacy38-recovery.md) for exact boundaries.

FW9369 establishes its empty-sensor baseline on open; fingers must be removed
during calibration. FT9365/9769 instead tune ADC exposure without retaining an
opening image, then use independently designed image contrast/gradient checks
to wait for a finger. This replaces the factory's FDT branch and still needs
real-device calibration of image quality and finger detection. Their capture
loops are cancellable; individual scans and calibration searches are bounded.
Empty-image polling backs off from 100 ms to at most 500 ms; this reduces scan
frequency without claiming hardware sleep or a calibrated thermal model.
FT9368's updater verifies PRAM by readback and the application checksum/version.
It is never invoked automatically just because a device failed to answer or
reports a different firmware version. See the three chip protocol documents.

The core calls the optional backend release observer between enrollment
frames, including matcher-rejected frames. FW9369 requires an unambiguous UP
event in release mode. FT9365/9769 require three consecutive raw frames whose
contrast and gradient are both below their presence thresholds. This image
heuristic is not proof of physical removal or liveness. Other legacy chips and
FT9368 retain their existing rearm/capture behavior until a positive release
indication is established; clearing a latched event is not such evidence.
Cancellation and failures in release observers must clean up before completing
the action; an unverified state invalidates the session.

Physical image resolution is catalog metadata, independent of the matcher.
Only FT9361 currently specifies 20 pixels/mm; other chips retain the framework's
unknown value rather than inheriting FT9361's pixel pitch.

## Electrical and failure contracts

Windows `IOCTL_GPIO_WRITE_PINS` uses physical controller pin levels. Userspace
requests the glue's reset GPIO with ACTIVE_LOW and uses logical `0/1/0` for
high/low/high. The glue forwards raw levels to avoid applying polarity twice.
The acquisition constraints and binary/API evidence are in
[gpio-polarity.md](gpio-polarity.md).

Each reset pulse explicitly establishes high for 10 ms, low for 20 ms and then
high. Firmware entry immediately sends the boot synchronization after that
pulse. A8 post-upload reset runs two pulses separated by 10 ms (20 ms total
high between the two low pulses), waits 160 ms, then performs software reset.
The FT9338-family reset and firmware readback have their own 80/180 ms startup
timing. Calibration-only backends do not substitute an A8 reset/upload sequence.

Terminal capture cleanup uses each protocol's acknowledgement and status:
legacy MCU `a5/5a`, FW9369/FT93xx verified awake-idle `50`, or FT9368 capture-clear
plus a valid application response. The latter is not a proof of sleep/power-off.
Failed cleanup
invalidates the session for further capture. Error/cancellation still finishes
through one action completion, attempts necessary cleanup, and clears image
buffers. Reopen establishes a fresh hardware state. Removal and suspend are
also terminal for an open transport session.

Final close uses optional `create_shutdown()` while transport/reset/IRQ resources
are still held. FW9369 implements C0/verified awake idle, known-event mask/ack
and C1 plus 1 ms. This clears `idle_verified`: delivery of C1 is not an observed
sleep-state or power measurement. Other backends retain their documented reset
cleanup. Per-action reset never invokes final shutdown. Close always releases
resources and preserves the earliest error; a failed session receives no more
sensor commands. Reopen repeats discovery and calibration as required.

The transport restores saved word size/speed on normal close. Native CS with
advertised polarity control also restores the ACPI mode on close and at the
next open. Fixed-CS sessions never write mode; reported GPIO-CS bits cannot
establish the physical polarity. Stock spidev cannot restore native CS on
process death. Kernel PM frees the IRQ,
invalidates the lease and wakes UIO readers, but does not send SPI commands or
atomically revoke stock spidev transactions. The transport therefore retains
cooperative locking and requires new sessions after suspend. Actual GPIO, IRQ,
process-exit and power-management behavior still need hardware validation.

## Validation

`scripts/check-fte3600.sh` compiles with warnings as errors and runs both
authentication configurations. Suites cover protocol golden packets/bounds,
identity domains, firmware file races, metadata enumeration, eight chips with
five image geometries, factory mode negotiation and CS polarity selection,
session failure/reopen, transfer/cancellation, matcher golden vectors,
strided images, concurrent extraction and template compatibility. Kernel policy
host tests and a Linux-header `W=1` build check the glue without loading it.
The [release validation record](validation-release-2026-10-05.md) gives executed
counts, build options, skipped fixtures and the exact ASan/UBSan scope.

No FTE sensor is attached in the build environment. Board wiring, cold boot,
real firmware upload, capture quality, resume behavior and biometric accuracy
still require hardware evaluation. The optional proprietary-firmware test
remains skipped unless its exact external fixture is provided.
