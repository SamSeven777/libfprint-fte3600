<a id="windows-生命周期覆盖与未完成项"></a>

# Windows lifecycle coverage and remaining work

[Documentation index](README.md)

**Coverage checkpoint: 2026-10-04 (America/Chicago).** The reviewed Linux
implementation was the then-uncommitted `acpi-spidev` tree based on
`1ce4c740ac759b69537312f46ac956e39c4a9b53`. Those changes were subsequently
published in `6e25b20`, with additional fixes described in the
[October 5 audit](audit-acpi-spidev-2026-10-05.md) and
[release validation](validation-release-2026-10-05.md). The current transport
contract is in [ACPI glue and stock spidev](acpi-spidev.md).

This record corrects the earlier inference that checking major capture paths
established a complete Windows-lifecycle port. The investigation traced the
call chains below and implemented ordinary-IRQ support, a separate final-close
interface, FW9369 pre-event communication recovery/C1 shutdown, and bounded
FT9368 wake checks. **Not every Windows path has been ported or tested on
hardware.** The table records this checkpoint's scope; historical test results
do not validate untested hardware behavior.

<a id="证据基线与判定方法"></a>

## Evidence baseline and classification

The Windows package INF version is **2.0.3.102**. The contained
`ftWbioUmdfDriverV2.dll` has PE FileVersion/ProductVersion **1.0.0.3188**,
not 2.0.3.102. Version resources and SHA-256 were checked again:
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.

The AMD64 DLL is 1,515,192 bytes, with image base `0x180000000`; all addresses
below are RVAs. Older references to “DLL 2.0.3.102” mean this package/hash, not
the DLL's file version. Matching catalogs, constructors or firmware across
package versions do not establish equivalent lifecycle behavior.

Evidence came from the original local binary, disassembly, call records and
function tables; the vendor binary was not executed. The repository records
interface, command and control-flow facts, not disassembly, vendor
implementation or firmware. Production Linux code and its simulators were
also examined. The classifications mean:

- **Checked:** evidence covers only the listed condition, message and branch,
  not an entire chip or computer.
- **Implementation gap:** a Windows function or branch has not been ported;
  that alone does not establish the cause of a reported hardware failure.
- **Independent policy:** Linux intentionally differs, with consequences and
  validation scope that must remain explicit.
- **Untraced:** the entry point, condition, postcondition or hardware behavior
  is insufficiently established for a positive claim.

A function's existence, a reachable call to it and a particular machine
actually taking that path are different levels of evidence. Application close,
capture cancellation, chip idle, device D0Exit and system suspend must be
traced separately.

<a id="资源irq-与框架生命周期"></a>

## Resources, IRQ and framework lifecycle

| Item | Windows evidence | Linux behavior at the checkpoint | Finding |
| --- | --- | --- | --- |
| Resource preparation | `2EB20` enumerates translated resources; `2EC38` accepts generic interrupt Type=2; `301E8` gives same-index raw/translated descriptors to WDF | ABI 2 accepts one edge-sensitive GpioInt, IRQ or EXTENDED_IRQ, using the real GPIO descriptor or SPI-core IRQ respectively | Implemented and covered by builds/policy tests; this branch still needed GPD hardware validation |
| IRQ polarity | `301E8` preserves system descriptors, without changing flags or hardcoding an edge | Both IRQ sources use ACPI polarity; ordinary IRQ is not fabricated as GPIO | No evidence that Windows corrects ACPI polarity; the cited GPD edge/active-low configuration worked in its reported test |
| IRQ-object lifetime | Prepare creates passive ISR `2DC20`; SPI destructor `2DBA5–2DBC9` deletes the IRQ object | UIO open within a valid reset lease requests the IRQ; close, PM and removal release it; no user means no sensor-event processing | Independent policy; releasing the host IRQ does not prove the sensor stopped signaling |
| Physical reset | `2F5E0 → 31414 → 3106C` passes raw 1/0/1 to GPIO writes with 10/20 ms waits | An active-low request's logical 0/1/0 corresponds to physical H/L/H | Requested levels/timing checked; actual waveform still unmeasured |
| CS | `303BC` opens SPI using a Resource Hub connection ID | ACPI baseline and polarity probing; normal close restores the baseline | Independent policy, not a proven Windows dual-CS sequence; current switching is additionally gated by reported CS-control capability |
| Framework registration | `24916–24948` registers Prepare `23D80`, Release `24760`, D0Entry `238C0`, D0Exit `23B70` | Resource preparation/release exist; kernel PM invalidates sessions and reopening identifies the sensor again | Chip power lifecycles are not equivalent; invalidation does not implement low-power entry/wake |
| D0 dispatch | D0Entry/Exit dispatch through vtable `+90/+88`; FW9369 targets `39130/39280` | Per-frame reset and final shutdown are distinct; system PM still invalidates the old session and requires reopen | Not automatic system-power recovery; all chip-specific power conditions still need tracing |
| S0/Sx policy | `30EF0` contains a 5000 ms idle setting, but `2EF30–2EF3F` calls it only for scene=2; SxWake `2D958` is conditional too | No corresponding runtime-idle/wakeup policy | GPD taking these branches was not established; Windows cannot be claimed to sleep it after a fixed five seconds |

Windows also abstracts GPIO-originated interrupts as
`CmResourceTypeInterrupt`; see
[Microsoft's resource documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/gpio-based-interrupt-resources).

Supporting ordinary IRQ on Linux required more than relaxing resource counts.
Reset remains one real GPIO; both interrupt sources use the standard read-only
UIO event interface. Kernel, pairing helper, userspace, permissions and tests
moved together to ABI 2. Event counts can coalesce or wrap. A PM wake must
validate generation before treating a notification as a sensor event. See
[the kernel contract](../../kernel/fte3600/README.md).

## FW9369 / raw ID 9362

| Stage | Windows evidence | Linux behavior and coverage |
| --- | --- | --- |
| Identity | `10088/10CD8` configures C6, reads 1A8B and checks 9362 | Discovery, special probing and initialization check positive identity; repeated confirmation is an independent protection |
| End of initialization | `FED8` completes wake, process selection, parameters and baseline, then enters WAIT_TOUCH | Linux initialization ends in awake idle and waits for an explicit action; the ending state differs |
| Calibration | `157C0 → 11C64` updates FDT baseline; `16294` updates image baseline | Bounded host DAC search and stability checks are independent algorithms; not all background baseline/interference handling is ported |
| Finger down/up | `F8C4(1/2) → 11800` configures FDT, baseline and 1881, clears 002F and sends C2 | Main command structure checked; Linux prearms release between enrollment samples and rejects ambiguous UP/DOWN |
| Image | `39050 → F8B4 → 15938 → 16898`; `17000` enters C4/state 54 and triggers scan; `18208` reads FIFO | Six-byte header plus 10,240 bytes of 16-bit pixels and main ordering checked; image conversion is independent |
| Pre-event communication recovery | `101EC → 10CD8` checks communication; failure wakes through `102B3` and retries | C6=01/4 ms and consecutive identity confirmation implemented; blank replies allow at most ten wake rounds, positive conflicting IDs stop further communication |
| Event control | `11008` reads 1A82; `11014/10FD0` sets/clears 1A83; `10FC0` writes 1A84 acknowledgement | Final close masks known 07FF bits by read-modify-write and separately acknowledges 07FF with W1C; unknown bits are preserved, not guaranteed cleared |
| IRQ without a capture request | ISR `2DDEC` checks events before checking the request after TOUCH; no request still enters WAIT_LEAVE through `2DEFA → F8C4(2)` | Linux handles events only during an action and releases transport at close; the background lifecycle differs |
| Device-idle capability | `FB2B → FB5A → 10BD0` sends C0, waits 1 ms, sends C1, waits 1 ms | Per-frame reset still verifies awake idle; final shutdown adds C1/1 ms after masking/acknowledging known events |
| Deep-sleep capability | `FBA0(3)` explicitly identifies deep sleep; `FC3F → 11048` wakes then sends C1 | Known C1 command implemented without inventing a post-C1 state value or claiming measured power/IRQ effects; reopen tested with production discovery/special-probe simulators |
| D0Entry | `238C0 → 39130` checks events and restores waiting; some failures reset/reinitialize | Linux invalidates the old session and requires reopen, rather than providing the same automatic recovery |
| D0Exit | `23B70 → 39280` enters WAIT_TOUCH when flag `1A4400==0` | This does not mean Windows always enters deep sleep at D0Exit |
| ESD/reset | `101EC` classifies events; ISR `2DE7C–2DF4A` has reset/reinitialize paths | Linux invalidates calibration, reports failure and requires reopen: intentionally less automatic recovery |
| Request cancellation | `28EE0` clears the request, conditionally calls ResumeIdle, notifies waiters and completes cancellation | Linux performs best-effort cleanup after cancellation; a direct C0/C1 from the Windows cancellation callback was not established |
| Public application close | No complete call chain from public handle close to chip commands has been established | C1's existence must not be described as proof that Windows sends it on every close |

The traced direct call into `FBA0` used wake. Idle/deep-sleep branches alone
do not establish their use on a particular Windows application close.
Background finger detection can itself be a valid Windows policy. Linux must
define its own no-user state and event ownership instead of assuming that
copying a background service is necessary.

The subsequent [October 5 audit](audit-acpi-spidev-2026-10-05.md) found that
communication recovery could stop detection and then wait without rearming.
That defect was fixed separately; the existence of the pre-event recovery
path in this checkpoint was not proof of complete recovery behavior.

<a id="其他芯片的关键覆盖与缺口"></a>

## Key checks and gaps in other families

| Family/stage | Windows evidence | Finding |
| --- | --- | --- |
| Wake before legacy identification | `24221 → 28344 → 270E4 → 28C54`: two 70 commands, 5 ms, MCU check, at most six rounds and 350 ms after success | Slow-start fallback exists; the A1 fast path and 2 ms after the second 70 are retained independent optimizations |
| FT9338/FT9536 RAM recovery | `365C0` uploads; failed comparison at `368DA` returns; only after `3691B` does startup occur | Double reset follows complete matching readback only; failure releases reset and invalidates the session. Refusing to guess an initially unidentified FT9338 is independent policy |
| FT9348/FT9361 A8 upload | `397EA` enters download, `3982D` uploads, `3983A` waits 2 ms, `39866` starts | Order checked; Windows also lacks legacy38-style complete RAM readback in this path |
| Legacy stop | `28668 → 28C54`: two 70 commands, mode-dependent clearing of 1E/1F and 10 ms wait | Main stop rule checked; bounded exit after unrelated IRQs is independent policy |
| FT9365/FT9769 down/up/sleep | `1928C` mode 1/2 uses FDT in `1B3B4`; mode 0 calls `1AC14` | Native FDT/sleep lifecycle not implemented; Linux actively scans and uses independent image thresholds for contact/release |
| FT9365/FT9769 idle finalization | `1A804` verifies 80=50; `1A880 → 225DC` sends A5 5A 00 | Linux remains awake idle and omits this final command; standby equivalence is not claimed |
| FT9368 wake recovery | `38E90` sends FF00, waits 10 ms, reads four bytes at 9180 through `38DEC`; equal nonzero bytes trigger retry | Implemented in initialization and both capture wake points, with at most three total attempts; complete INFO must still validate identity/geometry |
| FT9368 image/acknowledgement | `384F0` reads image; `38610` acknowledges with 9080; `38B60` reads information | Native 8-bit image, offset 7 and acknowledgement paths implemented; no new hardware validation |
| FT9368 POA | `38AF0` sends F080/reads four bytes in a particular global mode | Not implemented; calling conditions not fully traced. Do not add it unconditionally to close |

This investigation did not recheck every AFE field, FT9368 PRAM/flash error
branch, per-family cancellation path or D0/Modern Standby condition. Those
remain within the scope of a complete-port audit.

<a id="为什么此前测试没有发现"></a>

## Why earlier tests missed these gaps

The shared `Fte3600Backend` interface previously exposed only
`create_reset()`, without distinguishing final close, low power and wake.
Earlier `fte3600_close()` released transport immediately when
`idle_verified` was true. That flag proved reusable backend idle, not sleep
or suppression of every IRQ. The same reset served initialization completion,
per-frame cleanup, cancellation and error recovery. Adding C1 there would
change all those paths, not only close.

The previous `test-fw9369-backend.c` model made C0 produce state 50 and gave
5A/A5 no state effects. It did not model inaccessible registers during sleep,
background IRQs without a request or rediscovery after C1. Tests can check the
contract encoded in their model; they cannot independently prove it covers
every Windows state. Earlier builds, unit tests and sanitizer results remain
valid within scope, not as complete-lifecycle equivalence or evidence of
unexecuted paths.

An optional `create_shutdown()` was added while resources are still held.
FW9369 final close invokes it even from awake idle. Errors retain the first
failure and still release resources; failed sessions send no further commands.
Protocol tests model C1 sleep and blank identity before wake, then connect
production discovery/special-probe with actual backend initialization/capture.
Separate public-action tests check that per-frame cleanup does not shut down
and that repeated close/reopen works. These expand regression coverage without
providing independent evidence of physical sleep.

<a id="本轮实现与后续边界"></a>

## Implemented scope and outstanding work

1. Ordinary IRQ and GpioInt both have explicit delivery, acquisition/release,
   suspend and removal contracts. Reset remains a real GPIO. Both layouts and
   missing/multiple-interrupt errors have tests.
2. The backend interface distinguishes capture stop from final close; FW9369
   has shutdown/wake. Other families retain awake-idle cleanup without claiming
   equivalent sleep. Each family still needs explicit preconditions, commands,
   waits, observable postconditions, failure and cancellation behavior.
3. Software regressions cover FW9369's blank initial ID after sleep, dedicated
   wake/identification, recalibration/capture, repeated close/reopen, C1/wake
   errors and short transfers.
4. FT9368's confirmed bounded wake retry is implemented and tested. All-zero
   replies must not be assigned the same retry condition without evidence;
   three total attempts is not one initial attempt plus three retries.
5. Independent Linux policies remain: FT9365/FT9769 use active scans and host
   image thresholds instead of native FDT/sleep; FW9369 omits background
   baseline updates and invalidates calibration after ESD, requiring reopen
   rather than recalibrating automatically over a possible finger. Power and
   detection quality need hardware measurement. FT9368 POA conditions remain
   untraced and are not added unconditionally to close.
6. Trace public close, every chip's power slots, WDF request/power references,
   global-policy flag origins and necessary SensorAdapter entry points.
   Function names cannot substitute for missing call-chain evidence.
7. Record hardware chip state/events, IRQ counts, reopen and suspend/resume.
   Power and image quality need separate validation. Fewer interrupts, one
   successful frame or software tests cannot close every outstanding item.

<a id="最新gpd证据能证明什么"></a>

## Scope of the cited GPD evidence

The [resource correction report](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-5987576100)
and [capture report](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-5987625692)
used main `1ce4c74` plus the tester's local ordinary-IRQ patch, **not this
ACPI/spidev implementation**. They reported raw ID 9362, active-high CS, an ACPI
edge/active-low IRQ, initialization, capture and two close/reopen cycles.
Images were discarded; image quality, enrollment/matching and system
suspend/resume were not verified. About ten IRQs per second persisted after
close. Without corresponding 1A82/1A83/state observations, the cause remained
unknown.

At this checkpoint the changes were uncommitted and no issue update or
hardware access was performed. That is historical provenance, not the present
publication state. See the [checkpoint validation](validation-acpi-spidev-2026-10-04.md)
and [later release validation](validation-release-2026-10-05.md) for executed
checks and skips.
