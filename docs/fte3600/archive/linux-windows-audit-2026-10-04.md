<a id="linux--windows-行为核对--2026-10-04"></a>

# Linux/Windows behavior audit — 2026-10-04

[Documentation index](README.md)

**Historical record of the custom SPI bridge.** The original Linux baseline
was main `e8ab02e`, later incorporating the A1 fixes from host-matcher
`eacf7bc` and the CS-cleanup changes described below. Windows evidence came
from the package identified as 2.0.3.102. The DLL's actual file version is
clarified in [lifecycle coverage](windows-lifecycle-coverage.md).

The observations and test counts below describe those successive historical
checkpoints, not current main. In particular, the old kernel bridge's
last-reference CS restoration and GpioInt-only restriction do **not** describe
the current stock-spidev/ACPI-glue transport. Use
[the current transport contract](acpi-spidev.md) for those guarantees and limits.

The merged discovery flow retained the hardware-tested A1 fast path and added
bounded slow-start fallback. Code review and simulated tests did not establish
successful operation on Medion or GPD hardware.

A subsequent comparison with original Windows disassembly found and fixed
startup after failed legacy38 recovery and continued probing after conflicting
special-family identity. See the
[follow-up audit and validation](windows-recheck-2026-10-04.md).

<a id="已确认并修复legacy-识别前遗漏唤醒"></a>

## Confirmed and fixed: missing wake before legacy identification

The earlier flow performed the legacy software reset only after selecting a
backend. A sensor that needed wake before reporting runtime geometry could
therefore be rejected before backend selection or sent into ROM probing.

The Windows running-firmware check follows this order:

`one-byte 70 → 5 ms → one-byte 70 → read 20/21 → A5 5A → 350 ms → read 14/15`

Windows makes at most six attempts, waiting 5 ms between unsuccessful rounds.
The merged Linux flow sends one pair of `70` commands after normal application
probes fail and before C6 factory negotiation. It retains the hardware-tested
A1 path's final 2 ms delay and first repeats the geometry read. A positive
result selects the backend; only blank geometry enters the MCU-status check,
350 ms wait and bounded fallback of at most six rounds. An unknown nonempty
result still blocks ROM firmware fallback, but stale pre-wake data does not
prevent trying wake. Conflicting positive identity or a transfer failure ends
discovery immediately. Cancellation completes an already-started command pair,
then sends no more register reads or protocol probes. There is no second
legacy-wake sequence after C6 negotiation.

Windows Detect does not wait 2 ms after its second `70`. That delay also exists
in its A8 post-download wrapper, but Linux retains it here because of the
validated A1 path, not because Windows Detect requires it. See the
[Windows call chain](windows-runtime-adaptation.md) and
[Linux discovery design](dynamic-discovery.md).

<a id="对外部审计各项说法的核对"></a>

## Review of external audit claims at that checkpoint

| Claim | Historical finding |
| --- | --- |
| Strict ACPI resource counts and bias checks can reject OEM layouts | True as a compatibility boundary. There was insufficient evidence to attribute historical Medion zero responses to it, and no statistics supporting a “most frequent cause” claim. |
| Interleaved reset/IRQ resources or different controllers make IRQ index 0 select the wrong resource | Not established. That index counts GpioInt resources, each of which carries its controller reference. The old bridge accepted one GpioInt; a layout using only ordinary Interrupt was unsupported at this checkpoint. |
| Linux active-low produces the opposite waveform from Windows physical H/L/H | False for the requested levels: logical 0/1/0 through active-low produces physical H/L/H. A fixed board inverter would affect both implementations. Pin identity, supply and actual waveform still required measurement. |
| Windows always performs a double hardware reset before identifying any chip | Not supported by the checked paths. Windows first probes special families; failed retries and post-download startup have their own resets. |
| CS switching depends on bridge capability | True for the old bridge. It advertised the capability and normal legacy probing tried both polarities; userspace did not force switching when an older bridge lacked it. |
| Reading special IDs before configuring C6 necessarily hangs the chip | C6 negotiation was implemented; the initial direct read was an optimization. No evidence established that those reads hung the sensor. |
| A8 upload lacks the 2 ms delay, double hardware reset and 160 ms wait | Those states and timing constants existed. The A1 fixes separately found that cached main-loop time could shorten real waits; delays were changed to use monotonic time at the request. A delay call alone was not proof of actual duration. |
| FT9338/FT9536 have no cold recovery at all | Already outdated: independent upload, full readback and startup existed, subject to the identity gates below. |
| The unrelated-IRQ limit or not using continuous mode 2 on legacy38 necessarily causes failure | These were bounded-retry and single-capture policy differences, without proof of a hardware failure. A8 itself has a fast-mode branch. |
| A finger present during FW9369 open can contaminate the baseline | A known limitation. Open requires an uncovered sensor; stable samples do not prove that a stationary finger is absent. |

See [physical/logical polarity](gpio-polarity.md) and
[Linux 6.8 IRQ indexing](https://github.com/torvalds/linux/blob/v6.8/drivers/gpio/gpiolib-acpi.c#L1035).
Protocol details are in [legacy recovery](legacy38-recovery.md),
[special-family negotiation](special-probe.md), [FW9369 baseline](fw9369-protocol.md)
and [sensor evidence](sensor-protocol-evidence.md).

<a id="已确认并修复进程退出后-cs-遗留"></a>

## Confirmed and fixed in the old bridge: CS retained after process exit

Previously, only userspace discovery failures restored CS; kernel close
released reset without restoring mode. The old bridge was changed to record
CS polarity at successful open and restore it when the last file reference was
released, covering normal close and process termination. A surviving
`dup`/`fork` reference delayed cleanup until its last release. Only
`SPI_CS_HIGH` was restored; CS was not forced low and other SPI parameters
were preserved.

Before suspend, the bridge attempted restoration while the controller remained
available. Close during suspend did not access GPIO or SPI. An incomplete
restore was retried at resume and the next open; failures retained the original
target and invalid-configuration state. Even when the software mode already
contained the original bit, setup had to succeed before opening again.
Suspend still invalidated old sessions. Removal attempted restoration only
while the controller remained accessible; later closes did not touch removed
resources.

This restored the session's original configuration, not a proven correct ACPI
polarity, and did not isolate a shared bus during probing. Production helper
tests covered 49 cases: original high/low CS, other mode bits, failed changes
and rollbacks, and retry after failed restoration. A Linux 6.8 `W=1` module
build passed. Lifecycle fixtures added opposite-polarity reopen/discover/capture
cases while retaining userspace failure-cleanup checks. This later checkpoint
passed 116 lifecycle cases with authentication/IPA off and 119 with them on,
with no failures or skips. Actual process termination and suspend/resume still
needed target-hardware verification.

**This mechanism belonged to the custom bridge.** Current stock spidev cannot
guarantee immediate native-CS restoration after abnormal process exit; see the
[current contract](acpi-spidev.md).

<a id="仍然存在的边界"></a>

## Boundaries recorded at the time

- Main supported positively identified boot-A cold discovery for FT9536.
  Boot-B recovery required runtime FT9338/FT9536 geometry already observed in
  the same open and consistent OTP. An unidentified, entirely blank FT9338 did
  not authorize automatic firmware selection. Explicit candidate boot in the
  separate Medion diagnostic was another experiment.
- The old bridge required a single SPI connection, a single reset GpioIo and
  a single edge-sensitive GpioInt. Additional resources could not be accepted
  by choosing arbitrary pins. Ordinary ACPI IRQ support arrived later.
- Discovery did not isolate the shared SPI bus. Restoring CS at session end
  did not fix that; there was no evidence identifying it as the cause of
  historical Medion zero responses.
- FW9369 initialization required an uncovered reference baseline. Stable
  samples could not rule out a stationary finger. Contamination could affect
  later images, but did not prove that every attempt would be blank or fail.
- Missing wake was a software defect. Explaining a particular machine's
  historical `00 00` still required its pre/post-wake MCU state, runtime
  geometry and actual-CS logs.

<a id="合并范围与本地验证"></a>

## Merge scope and local validation

The merge retained main's installer and SELinux fixes and host-matcher's
algorithm separation, IPA integration, A1 wake, monotonic timing, V3 enrollment
size limit and `.llseek = NULL` compatibility change. Wake used strictly
length-checked, one-byte full-duplex transactions, without adding clocks;
short transfers failed immediately.

V3 remained restricted to the IPA adapter's FT9361 scope and rejected other
chips being decoded as FT9361. It preserved original processing-version
metadata so old and new encodings round-tripped byte-for-byte. Other chips
retained their own BRISK/V2 settings.

Authentication followed the host-matcher branch's policy. Its practical BRISK
rules differed from the preceding main policy; this merge did not retune
thresholds, change policy versions or perform FAR/FRR calibration.

The earlier merge-validation checkpoint used Ubuntu 24.04/WSL, GCC 13.3 and
`werror=true`:

| Configuration | Passing suites | Complete lifecycle cases |
| --- | ---: | ---: |
| Personal authentication off, IPA off | 26 | 114 |
| Personal authentication on, IPA on | 26 | 117 |
| Personal authentication on, IPA off; template/family/authentication targets | 3 | Not rerun in this configuration |
| Dual algorithm; ASan + UBSan core/template/authentication/lifecycle targets | 10 | 117 |

All selected suites passed. Each normal matrix skipped one optional case
requiring an external FT9361 firmware file; lifecycle, BRISK and sanitizer
targets had no skips. The 26 suites covered the core device/state-machine,
protocol, backend, matcher, template and installer tests selected by
`scripts/check-fte3600.sh`; lifecycle and the other suites were run separately.
The ten sanitizer suites covered core device, state machine, cancellation,
SPI, BRISK, IPA, template, family template, authentication lifecycle and device
lifecycle.

Lifecycle retained six A1 sleep/stale-data/reopen cases and 17 multi-chip wake
cases covering fast/slow paths, dual CS, real delays, attempt limits, identity
conflicts, short transfers and cancellation. Immediately identifiable special
families skipped legacy wake; C6-dependent families had separate ordering
checks. A timer regression performed synchronous work inside a callback before
requesting another wait, checking both the deadline and actual callback so
machine load could not conceal shortened waits.

Kernel policy tests and a Linux 6.8.0-146 `W=1` build passed; the module was not
loaded. Formatting, shell syntax and diff checks passed. Lifecycle retained
actual protocol waits and used a 60-second suite timeout. No new A1, Medion or
other target-hardware test was performed. The previous A1 hardware evidence
came from `eacf7bc`, not from the merged implementation.
