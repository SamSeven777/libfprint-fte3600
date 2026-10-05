# Dynamic ACPI and sensor discovery

[Documentation index](README.md)

Current discovery combines ACPI resource validation with chip-specific runtime
and ROM identity protocols. It has no DMI model/controller/pin table and does
not assume a chip after failed reads. Unknown replies never authorize firmware
upload. For installation, use [the build guide](install.md) and
[system integration](acpi-spidev.md); this page describes the runtime behavior.

## Two independent decisions

### Resource association

Linux resolves the physical SPI device from ACPI, and that device binds to
stock `spidev`. The `fte3600` module creates a separate `fte3600-glue` platform
child. It validates one SPI connection, one single-pin reset GpioIo and one
edge-sensitive interrupt: either a single-pin GpioInt or one ordinary ACPI
IRQ/Interrupt resource. GPIO resources may use different controllers and
appear in either order. An ordinary interrupt uses the IRQ resolved by SPI
core and does not consume a GPIO resource index.

The glue exposes one reset GPIO line and a separate IRQ-only UIO device.
libfprint verifies all three character devices belong to the same physical
SPI/glue instance, checks metadata ABI 2 and a stable ready generation, and
acquires an exclusive reset/UIO lease. Missing or ambiguous resources,
unsupported trigger modes and contradictory reset properties fail explicitly.
There is no computer-model bypass. See the
[kernel contract](../../kernel/fte3600/README.md#resources-and-device-pairing).

### Application identity

Discovery follows the normal SPI factory's first two rounds in Windows
`EvtDevicePrepareHardware`. It no longer tries direct legacy geometry or
special-family IDs before the factory wake sequence. At each available
connection it performs:

1. FT9368 `ff00` wake, **5 ms**, then 32-byte `9180` application information
   (39-byte total transaction). A negative first attempt repeats this complete
   sequence once. Positive metadata must also pass independent validation and
   identity confirmation.
2. Special-family wake, then **two** C6 configuration calls. Each call writes
   `c6=01`, waits 4 ms, and reads back, up to **31 attempts**. Exhausting a
   readback mismatch still proceeds to the next call and ultimately the ID
   read, as the reference does. Actual transfer errors remain fatal.
3. If special identification returns no known identity, H10/L20/H reset plus
   **10 ms**, then repeat the whole special sequence once on the same CS.
   A second negative result returns without another reset. Repeated positive
   IDs and the FT9391 variant check are mandatory; known unsupported IDs,
   conflicting observations and I/O errors cannot authorize another family.

Where `fte3600_cs_control=1`, Linux also tries the alternate native CS
connection before finishing a factory round. Fixed-CS transports retain their
effective connection without mode writes. Successful discovery keeps its
selected connection; failed discovery attempts to restore the baseline.
These connection trials are Linux integration, not observed Windows behavior.

When every connection returns negative, factory round zero repeats the
FT9368/special sequence as round one. Only after round one also fails does
legacy application detection begin. This preserves the normal reference
ordering instead of collapsing its repetitions into an unverified fast path.
See [special-family transactions and reference RVAs](special-probe.md).

<a id="legacy-wake-and-factory-negotiation"></a>

### Factory negotiation and legacy wake

Legacy detection sends `70`, waits **5 ms**, sends `70` again, and immediately
reads MCU status `20/21`. There is no added 2 ms wait or early geometry shortcut.
Non-idle status permits six rounds total per available CS, with 5 ms between
failed rounds. Only `a5 5a` permits the **350 ms** settle followed by geometry
reads. Positive geometry must repeat unchanged: `5858`, `6060`, `4050`, `4080`
select FT9338, FT9348, FT9361 and FT9536 respectively. A started command pair
finishes before cancellation is observed; transfer errors stop immediately.

Unknown nonempty geometry prevents unidentified ROM recovery on that
connection. Changing identity, failed variant validation, transfer errors and
cancellation stop discovery rather than becoming permission to upload.
Explicit boot-only discovery retains its separate ROM entry sequence.

The earlier A1 hardware results covered a different discovery order. The
reference-based sequence now performs special-family reset/retry before legacy
status and geometry, even for a running legacy application. It needs A1
revalidation; neither preservation nor erasure of runtime RAM across reset is
assumed from software tests. Corrected GPD reopen latency also needs measurement.

The later Windows outer rounds include a 300 ms FT9368 recovery and eventually
force an unconfirmed family into a firmware loader. Linux stops at reliable ROM
identification instead of reproducing those guesses, defaulting an unknown
boot-A reply to FT9338, or retaining an old global identity after unknown
geometry. This is the explicit limit of the reproduced factory path.

### ROM identity and upload authorization

Only an eligible empty legacy response reaches unidentified ROM discovery.
The independently implemented sequences are described in
[Windows runtime adaptation](windows-runtime-adaptation.md):

- `90 00 00` distinguishes boot-A (`ef` response). After the documented reset,
  synchronization and register preparation, `fe=02` positively identifies
  FT9536. Other boot-A responses do not default to FT9338.
- A8 uses `06 f9 00`, a four-byte register query staged at mailbox `85c0`, a
  2 ms settle, and mailbox trigger/read to obtain the family.
- Accepted A8 families are `2b50`, `95a8`, `23dd`. OTP is selected through
  `c8/f1/f4/f3`. SPI low nibble `1..3` identifies FT9348; `4/e/f` identifies
  FT9361. USB's different OTP adjustment is not used. Family and OTP must agree.
- Boot-B38 requires runtime `5858`/`4080` observed in the current open and
  corresponding OTP upper nibble `1`/`2`. A cached enumeration identity or
  unknown A8 family does not authorize this path. First unidentified blank
  FT9338 discovery remains unsupported.
- OTP access is cleared and bounded reset cleanup returns from discovery mode,
  including error/cancellation paths. Cleanup failure invalidates the session.

Every RAM upload requires its own accepted ROM check and the selected sensor's
exact external firmware size/SHA-256. There is no fallback to FT9361 or trial
of unrelated firmware. FT9338/FT9536 use distinct four-byte boot-register
framing and compare the entire RAM readback before restarting. See
[legacy38 recovery](legacy38-recovery.md).

Runtime `0x14/0x15` values describe sensor geometry in the Windows driver; they
are not claimed to be immutable silicon IDs. Geometry, application and AGC
versions are checked again after initialization. FT9368 persistent flash update
is a separate explicit operation requiring a positive application identity;
blank replies do not authorize it. See the [support matrix](status.md#implemented-functions-and-test-limits)
for each chip's boundary.

Wire encoding was checked at DLL RVAs `22c78`, `2f1b0`, `2fc30`, `2fab0`,
`2f4c0`, with identity flow at `28114`, `2819c`, `278f4`. These are independently
implemented technical observations, not copied vendor code or a claim of
formal clean-room separation. Static evidence and software tests do not
establish cold-probe success on a physical device.

## Electrical and protocol limits

Windows sends physical GPIO-controller pin levels `1,0,1`. Userspace requests
the glue's reset line with ACTIVE_LOW and uses logical values `0,1,0` for the
same high/low/high sequence. The glue forwards raw physical levels, avoiding a
second inversion. GpioIo itself has no polarity field. The fallback mapping is
active-low; existing `reset-gpios`/`reset-gpio` properties must reference the
validated resource, pin zero, active-low, with exactly one entry. Pull-down,
unknown bias and contradictory mappings fail before GPIO acquisition because
ACPI can override the initial output value. See [GPIO polarity evidence](gpio-polarity.md).
Physical voltage, board inverters and waveform timing still require measurement.
IRQ polarity/trigger comes from the validated GpioInt or ordinary ACPI interrupt.

The backends require mode-0 clock flags, eight-bit words and a configured
maximum speed no greater than 1 MHz, the saved spidev speed or the ACPI speed.
This is a configured limit, not a clock measurement. Native CS negotiation is
available only when metadata permits it; fixed CS is never rewritten. Neither
mode can isolate other devices on a shared SPI controller during a polarity
trial. See [CS and failure limits](acpi-spidev.md#failure-and-power-management-boundaries).

Userspace bounds transfers by `min(spidev.bufsiz, 32768)`, and each backend
checks its largest individual transaction before starting. Setup requires
`spidev.bufsiz >= 32768`. Stock spidev does not expose a general query for all
controller transfer/message limits; the controller can still reject an actual
transaction. These are protocol sizes, not promises about a given controller:

| Protocol operation | Required single transaction (bytes) |
| --- | ---: |
| FT9338 / FT9348 / FT9361 / FT9536 image | 7752 / 9224 / 5128 / 8200 |
| FT9348 / FT9361 RAM upload | 10319 / 10403 |
| FT9338 / FT9536 full RAM readback | 14192 / 11942 |
| FW9369 image / FT9368 image | 10246 / 5127 |
| FT9365 / FT9769 FIFO chunk maximum | 1798 |

The documented FT9365/9769 FIFO chunks do not require the whole raw image in
one transfer. Legacy images and full RAM readbacks remain continuous. The
driver does not split an oversized transaction across chip selects to bypass
a controller error.

## Kernel/userspace boundary

The kernel glue owns reset/IRQ resources, publishes metadata ABI 2 and
invalidates leases on suspend/removal. It sends no sensor SPI command and
never changes SPI mode. Userspace uses standard spidev, GPIO v2 and UIO
interfaces, with SPI `flock` for cooperating clients. The three nodes are
validated together; there is no private `/dev/fte3600-*` SPI ioctl endpoint.
See the [complete kernel ABI](../../kernel/fte3600/README.md#metadata-abi-2).

Normal close releases UIO before reset and restores saved word size/speed.
For controllable native CS it also restores the ACPI mode; fixed-CS sessions
never write mode. SIGKILL can leave native CS in its trial state until a new
open restores the baseline. The glue cannot atomically revoke an in-flight
stock-spidev transaction or prevent another root process ignoring the lock.
Status/generation checks bracket SPI transfers and IRQ waits. Suspend or
rebind ends the old session; resume requires close/reopen, not reuse of stale
file descriptors. These are deliberate differences from the former private
SPI bridge.

## Build and migrate

Use [installation](install.md) for dependencies, library build, firmware and
optional authentication. Use [system integration](acpi-spidev.md) for matching
kernel glue, migration from the old bridge, permissions and rollback. Do not
reuse historical ABI 1 misc-node commands or old bridge service policies.

## Validation scope

The [release validation record](validation-release-2026-10-05.md) records the
executed builds, mock suites, sanitizer scope and skipped fixtures. Kernel
policy tests and Linux-header compilation do not exercise real ACPI providers,
IRQs or suspend. The proprietary-firmware fixture remains optional and is
skipped unless supplied externally.

Enumeration opens a temporary transport session to establish per-device
features. It never loads firmware or starts capture; ROM probing can change
boot state and therefore has reset cleanup. Every open repeats identification
and rejects a sensor different from the one enumerated. Default builds expose
capture; opt-in builds advertise matching features only for supported profiles.
Templates from a different sensor profile are rejected before capture, including
profiles with equal dimensions.

Capture completes only after the backend's bounded return-to-idle sequence.
Unverified cleanup invalidates the session and blocks later captures without
further sensor I/O; close releases resources and a new open reinitializes them.
See [architecture](architecture.md) and [protocol evidence](sensor-protocol-evidence.md).
Implemented capture for eight chips does not establish all-OEM compatibility,
cold-boot reliability, suspend behavior, image quality or biometric accuracy.
