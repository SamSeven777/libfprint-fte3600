# ACPI glue / stock spidev validation — 2026-10-04

Historical checkpoint. The [October 5 release validation](validation-release-2026-10-05.md)
records subsequent audit fixes and regression coverage; the counts below
describe the earlier working tree.

Branch: `acpi-spidev`, based on `1ce4c740ac759b69537312f46ac956e39c4a9b53`.
The implementation remains uncommitted. No module was loaded, system policy
installed, hardware accessed, or upstream submission made during this work.

## Implemented boundary

- The physical SPI device binds to the distribution's unmodified `spidev`.
- The `fte3600` module creates a separate platform child and resolves one reset
  GpioIo plus one edge-sensitive GpioInt or ordinary ACPI IRQ/Interrupt resource.
  ABI 2 exposes one reset GPIO and a separate IRQ-only UIO device, without maps.
- libfprint validates the actual SPI/GPIO/UIO device relationship, uses standard
  SPI/GPIO v2 ioctls and UIO event reads, and retains sensor protocols, firmware
  checks, image processing and host matcher.
- Reset forwarding preserves physical levels; userspace applies ACTIVE_LOW
  once. CS probing remains in userspace. Normal close restores the ACPI CS
  baseline, and a new session recovers from a previous process's trial mode.
- Session checks bracket SPI messages and monitor IRQ waits across system
  suspend. Failed acquisition, unverified SPI configuration and stale sessions
  close their descriptors. IRQ handlers exist only during valid UIO leases;
  close, PM and removal free them. Kernel teardown retains reset resources
  until outstanding GPIO requests have released them.
- FW9369 final close is separate from reusable per-action idle. It masks and
  acknowledges known event bits, then sends C1; completion is not a measured
  sleep-state result. Events get a bounded communication/wake check first.
- FT9368 initialization/capture use the confirmed four-byte wake response and
  three-attempt limit, followed by full identity/geometry verification.
- Positive conflicting identities invalidate FW9369/FT9368 sessions before
  further protocol cleanup. Blank responses are not evidence of another chip.

## Checks completed

| Check | Result |
| --- | --- |
| GCC 13.3 / Meson, `-Dwerror=true`, capture-only plus virtual drivers | Build passed; 31/31 unit-test tasks passed |
| GCC 13.3 / Meson, `-Dwerror=true`, personal/IPA authentication enabled | Build passed; 29/29 unit-test tasks passed |
| Actual resource resolver against synthetic sysfs | 34 cases passed, including IRQ metadata, three-node ancestry, UIO identity/no mappings and stable generation |
| Actual device enumeration against synthetic GUdev | 26 cases passed |
| Capture-only lifecycle | 138 cases passed |
| Authentication-enabled lifecycle | 141 cases passed |
| Public action lifecycle | Capture-only 14 / authentication-enabled 33 cases passed; includes shutdown dispatch, repeated close/reopen and first-error preservation |
| FW9369 backend, including shutdown/discovery/reopen | 42 cases passed |
| FT9368 backend, including wake and identity-conflict handling | 44 cases passed |
| Pairing helper | 22 cases passed |
| Installation / removal helper | 33 cases passed |
| Address + undefined-behavior sanitizers: context, resources, generic SPI | Passed |
| Address + undefined-behavior sanitizers: capture-only lifecycle and authentication-policy lifecycle | Passed after fixing a test-only fortified-read interception gap |
| Address + undefined-behavior sanitizers: FW9369 and FT9368 backends | Both complete suites passed |
| Linux 6.8.0-146 headers, matching GCC 13, `W=1` | Module compiled without compiler warnings; BTF skipped because no `vmlinux` was available |
| Kernel resource and lease policy | Eight host-test groups passed |
| Shell and udev rules | Syntax checks passed, including systemd-udev 255 |
| SELinux CIL | Compiled; resulting policy grants UIO only getattr/open/read, with no generic device-node grants |
| AppStream metadata | Validation passed after omitting an empty USB provides element for a non-USB-only build |
| Patch whitespace | `git diff --check` passed |

The full unit-test runs had no failing or timed-out tasks. Each included one
optional test case skipped because `FTE3600_TEST_FIRMWARE` was unset; no valid
local FT9361 firmware fixture was supplied to that test. This skip is distinct
from the successful unit-test task containing it. Introspection-based driver
replay tests were not enabled in these builds. The unrestricted Meson runs
finished with 32 passed / 33 skipped tasks for capture-only and 30 passed /
33 skipped tasks for authentication. Each skip total consists of 32 replay
tasks unavailable without introspection and the USB hwdb content comparison,
which is deliberately skipped for a non-default driver selection. There were
no failed or timed-out tasks. The separate optional firmware case is inside a
passing unit-test task and is not counted as a skipped Meson task.

The first unrestricted run exposed a CRLF shebang in `test-generated-hwdb.sh`
and the non-USB metadata generator's empty `provides` element. The script was
normalized to the repository's LF rule and the generator now omits that empty
element; the final unrestricted runs above include both corrections.

The sanitizer build used `__read_chk` where the normal build used `read`.
The lifecycle fixture now intercepts both, preserves the checked buffer size,
and delegates non-fixture descriptors to the real checked function. After this
test-only correction, the affected sanitizer and normal lifecycle runs passed;
the production transport did not need a change.

The regression tests include wrong-parent and ambiguous triple-node pairing, partial
resource acquisition, CS rollback failure, restoration failure followed by
reopen, counter coalescence/wrap/short reads, PM notifications, and an expired
session while waiting without an IRQ. New parser and
transport checks exercise production code rather than a second implementation
of the protocol.

The FW9369 electrical model makes the ID blank after C1 until a wake command.
Tests run production discovery and special-probe after actual backend shutdown,
then initialize and capture again; wake failures and conflicting responses also
stop correctly. Separate public-action tests exercise final-close dispatch and
resource release. FT9368 tests cover wake exhaustion/cancellation, positive
identity conflicts in initialization/capture/cleanup, and ambiguous responses.
These composed tests are not a physical-device or complete end-to-end OS test.

## What these checks do not establish

No A1, GPD Pocket 3 or Medion device was exercised. Electrical reset waveforms,
actual IRQ delivery, cold boot, suspend/resume, forced unbinding and capture
quality need hardware testing. The kernel policy tests do not emulate gpiolib
or its teardown concurrency. Only Linux 6.8 headers were compiled; later API
compatibility guards have source review but still need a build on each target
kernel.

Secure Boot still requires a trusted signature for the out-of-tree glue.
The synthetic SELinux build does not prove Fedora's real fprintd domain can
use anonymous GPIO line-request descriptors and read UIO events. Actual enforcing-system testing
is required. Stock spidev also retains cooperative locking and lacks immediate
CS rollback after SIGKILL; it does not provide bus-wide isolation for opposite
CS-polarity trials.

The tested module declares `depends=uio`; its 6.8 headers enable `CONFIG_UIO=m`.
The target kernel's UIO module is not installed in this WSL environment, so a
complete module dependency-load run was not performed. The installer uses
`modprobe`, and the target distribution must provide its configured UIO support.
No claim is made that FT9365/9769 native sleep/FDT, FW9369 background baseline
maintenance/automatic ESD recovery, FT9368 POA, or all Windows system-power
paths have been ported. See the [lifecycle coverage](windows-lifecycle-coverage.md).

See [architecture, installation and migration](acpi-spidev.md) and the
[kernel ABI and lifecycle contract](../../kernel/fte3600/README.md).
