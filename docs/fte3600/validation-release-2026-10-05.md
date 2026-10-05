# Release preparation validation — 2026-10-05

[Documentation index](README.md)

**Dated validation record.** Counts below are results for the recorded
implementation and runs, not a promise that an arbitrary later checkout passed
the same checks.

This record covers the ACPI reset/IRQ glue and stock-spidev implementation in
`6e25b20`, based on `1ce4c74`, and the subsequent CI dependency fix in
`732425e`. It follows the
[independent audit](audit-acpi-spidev-2026-10-05.md); it does not replace the
earlier hardware reports with simulated results.

## Fixes included

- FW9369 records when communication recovery stops finger detection. An
  unrelated or empty event restarts the existing mode with C2 before waiting
  again, without clearing newly latched events or replacing the baseline.
- The kernel publishes mandatory `fte3600_cs_control` metadata. GPIO-controlled
  and other fixed CS paths retain the existing mode, never write MODE32 and
  continue discovery/wake without advertising a polarity switch they cannot
  perform. Normal close also preserves this distinction.
- Both initial and delayed udev events explicitly label a verified, pinned
  device inode. Installation invokes the same helper synchronously and verifies
  the actual result; it no longer depends on SECLABEL being applied on CHANGE.
- Removal restores surviving, positively identified spidev nodes even without
  a complete GPIO/UIO pair. It drains queued udev workers and checks for module
  reload before restoring labels and removing policy. A failure preserves the
  policy and helper for retry rather than silently reporting complete cleanup.

The current README, install entry points and hardware-report template describe
the new interface. [Upstream preparation](upstream-preparation.md) separates
kernel, generic libfprint, sensor and matcher review concerns. No kernel or
libfprint upstream submission was made by this work.

## Software checks

GCC 13.3, `-Dwerror=true`, WSL Ubuntu; no sensor devices were opened.

| Check | Result |
| --- | --- |
| Capture-only build and unrestricted Meson tests | 32 passed, 33 skipped, no failures/timeouts |
| Personal + IPA authentication build and unrestricted Meson tests | 30 passed, 33 skipped, no failures/timeouts |
| Resource resolver / GUdev enumeration | 37 / 28 cases passed |
| Capture-only / authentication lifecycle | 141 / 144 cases passed |
| Public action lifecycle, capture-only / authentication | 14 / 33 cases passed |
| FW9369 / FT9368 backends | 55 / 44 cases passed |
| Pairing and label helper | 35 cases passed |
| Installation/removal helper after final queue-ordering fix | 39 cases passed |
| Address + undefined-behavior sanitizers | Seven suites passed: generic SPI, context, resources, lifecycle, public actions, FW9369, FT9368 |
| Linux 6.8.0-146 headers, matching GCC, `W=1` | Module built; BTF skipped because no vmlinux was available |
| Kernel resource/lease/CS policy | Nine host-test groups passed |
| Shell syntax and udev label rule validation | Passed |

The unrestricted-run skips have the same meaning as the
[October 4 record](validation-acpi-spidev-2026-10-04.md): 32 unavailable
introspection replay tasks plus the USB hwdb comparison for a non-default
driver selection. The separate optional FT9361 firmware fixture remains unset;
its case is skipped inside an otherwise passing test task. Sanitizer checks
used `detect_leaks=0`; they do not establish absence of leaks.

The new FW9369 simulator requires detection to be running whenever the backend
waits for an IRQ. Its cases cover unrelated/empty events, release waits, a newly
latched event and C2 failure/cancellation. GPIO-CS fixtures reproduce stock
spidev's asymmetric mode read/write behavior and cover open failure, wake,
capture, close and reopen. Label tests exercise inode pinning, metadata loss,
incomplete pairing, queued label writers and module reload during uninstall.
O_PATH/proc-fd xattr behavior was additionally checked on an ordinary temporary
file; that is not a real SELinux device-policy test.

Local full-build and sanitizer logs are under
`work/publish-validation-2026-10-05/`; these are development records, not shipped
runtime dependencies. The final queue-ordering change was followed by another
complete run of the installation/removal and pairing helper suites; both passed.

## Published CI results

The final CI runs below completed successfully after adding the `systemd`
runtime package, which provides `systemd-escape`, to the Ubuntu dependencies.
These are remote software checks and are distinct from the local matrix above.

| Branch / source | Workflow run | Fedora 43 job | Ubuntu job |
| --- | --- | --- | --- |
| Main `732425e` | [37270794630](https://github.com/SamSeven777/libfprint-fte3600/actions/runs/37270794630) | [111637148615 — passed](https://github.com/SamSeven777/libfprint-fte3600/actions/runs/37270794630/job/111637148615) | [111637148724 — passed](https://github.com/SamSeven777/libfprint-fte3600/actions/runs/37270794630/job/111637148724) |
| Separate Medion branch `c71dfa4` | [37270794636](https://github.com/SamSeven777/libfprint-fte3600/actions/runs/37270794636) | [111637148598 — passed](https://github.com/SamSeven777/libfprint-fte3600/actions/runs/37270794636/job/111637148598) | [111637148863 — passed](https://github.com/SamSeven777/libfprint-fte3600/actions/runs/37270794636/job/111637148863) |

The Medion result belongs to its separate diagnostic branch; it does not make
that workflow part of main or establish successful operation on the reported
Medion machine. Neither run is evidence of physical sensor behavior or an
enforcing-system installation.

## Hardware and integration limits

No A1, GPD Pocket 3 or Medion E3224 hardware was exercised for this release.
There is no new claim of Fedora enforcing access, Secure Boot module trust,
IRQ/physical polarity correctness, measured sleep, suspend/resume recovery or
biometric accuracy. The normal driver still requires the external glue module
and a trusted module signature where the kernel requires one. The separate
Medion diagnostic uses distribution spidev and physical GPIO v2 directly.

Stock spidev still provides cooperative locking rather than enforced
cross-client leases or atomic PM revocation. An abnormal process exit cannot
guarantee immediate native-CS rollback; the next controlled open restores its
baseline. These limits and the incomplete Windows power/background-calibration
coverage remain documented, rather than being treated as fixed by unit tests.

## Vendor factory restoration on the c87992a working tree

This later local run covers the normal Windows SPI factory round-zero/one
restoration based on `c87992a`. It supersedes the earlier discovery-order
prototype, which retained direct identity shortcuts. It is not a published CI
result or a new hardware report.

The corrected sequence uses FT9368 factory wake with 5 ms, two 31-attempt C6
configuration calls per special attempt, one same-CS H10/L20/H + 10 ms retry,
and legacy MCU status before the 350 ms geometry settle. Direct-ID and early
geometry shortcuts and the extra 2 ms discovery wait are removed. A8 firmware
startup remains unchanged. Identity confirmation, cancellation, transfer-error
handling and reliable firmware authorization remain explicit Linux safeguards.
See [discovery behavior](dynamic-discovery.md) and [wire evidence](special-probe.md).

GCC 13.3, warnings as errors, WSL Ubuntu:

| Check | Result |
| --- | --- |
| Capture-only full Meson run | 32 suites passed, 33 skipped |
| Personal + IPA authentication full Meson run | 30 suites passed, 33 skipped |
| Device lifecycle, capture-only / authentication | 146 / 149 cases passed |
| Public action lifecycle, capture-only / authentication | 14 / 33 cases passed |
| Special-family factory / FW9369 backend | 27 / 57 cases passed |
| General SSM timer and state tests | 42 cases passed |
| ASan + UBSan | Four suites passed: special factory, FW9369 backend, device lifecycle, public action lifecycle |

Skips retain the meaning documented above; leak detection remains disabled in
the sanitizer environment. Local logs are in
`work/vendor-factory-validation-2026-10-05/`, separate from prototype runs.

Lifecycle protocol waits use an explicit virtual clock with asynchronous
dispatch, so tests check requested intervals and bus ordering without waiting
through every real-time 31-attempt loop. SPI workers, cancellation and public
device actions still execute. The general SSM timer tests retain real-time
scheduling checks. These results do not measure device latency.

The legacy simulator explicitly models retained application RAM: reset hides
runtime geometry until the complete wake/status/settle sequence. It does not
assert that every board retains RAM. FW9369 tests preserve C1 sleep and chip
registers while destroying and recreating the host device, and cover correct,
alternate and fixed CS, wake errors and identity conflicts after reset. No
real A1 or GPD device was available for this correction; both need revalidation.
