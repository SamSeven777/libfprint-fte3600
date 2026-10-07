# Vendor Sequence Corrections — 2026-10-06

[Documentation Index](README.md) · [Wire Protocols](protocols.md)

This change set follows an audit of main `4dae0af`. The comparisons below refer
to the inspected Windows 2.0.3.102 DLL identified in the protocol reference.
They establish the corrected command order and software error handling, not a
new physical-device test result.

## Reset Release and ROM Sync

The A8 RAM-download entry and both legacy-38 identification/recovery paths now
finish the reset pulse with one explicit compound operation:

1. Prepare the complete `55 AA` frame and queue its worker while reset is asserted.
2. Validate the transfer, SPI configuration, and resource generation.
3. Release reset through GPIO and immediately issue the prepared SPI message.
4. Check that exactly two bytes transferred and validate the generation again.
5. Return to the main context and advance the state machine.

Steps 3's GPIO and SPI ioctls run in the same worker. There is no sleep, log,
SSM transition, thread dispatch, or resource query between them. This follows
the vendor release/sync ordering at `2EAD0` / `2F5E0`, while retaining the normal
Linux transfer validation and callback lifetime. It is not an atomic operation
across the two kernel devices or a measured maximum release-to-clock interval.

The generic transfer function no longer intercepts packets beginning with `55`.
Ordinary full-duplex reads still use one complete SPI message. No extra dummy
clocks or message segments were introduced.

Cancellation does not split a reset pulse already in progress: the bounded
release/sync finishes, then the protocol state machine observes cancellation.
GPIO failures suppress the SPI message; generation and configuration failures
before preparation suppress both operations. Short transfers are errors.

The unused pulse helper and unused `restart_wait_ms` metadata were removed.
Active per-protocol waits remain 80 ms for FT9338, 180 ms for FT9536, and 160 ms
for FT9348/FT9361 after their application-start sequences. None is inserted
between ROM-entry reset release and `55 AA`.

## Identity and C6 Negotiation

| Path | Corrected behavior | Reference |
| --- | --- | --- |
| ROM family `1534` | Enter B38 identification explicitly; ignore unrelated header receive bytes when selecting boot edition | Family classification `2831B–28335`, B38 OTP classification `27807–27889` |
| B38 firmware authorization | Accept a matching positive OTP with the current-session `1534` context consistently in the permission helper and loader | The observed B38 route; model remains OTP-derived |
| FW9369 initialization | Two C6 helper passes, each up to 31 write/4 ms/read attempts, followed by silicon ID validation | `FD94`, called at `100D7` and `10D79` |
| FT9365/FT9769 initialization | Up to four C6 attempts, ID validation, then another up-to-four-attempt helper pass | `195D0`, called at `199CA` and `19717` |

A failed C6 acknowledgement alone does not veto a valid chip ID. An actual
transport error still aborts initialization. The regression fixtures exercise
successful acknowledgement, one failed read followed by success, exhaustion
with a valid ID, and an I/O error for all five modern silicon/process variants.

The pre-existing conservative unknown-identity policy remains distinct from
Windows' fallback defaults. In particular, main does not select FT9338 merely
because OTP is all ones, and `1534` is not a unique model number. This change
does not claim equivalence for every vendor fallback or host calibration policy.

## Regression Coverage

New tests cover:

- Reset release and SPI in the same worker, without an intervening resource check.
- Stale generation before sync, generation changes during sync, invalid SPI
  configuration, short results of zero/one byte, SPI errors, and GPIO errors.
- Invalid transfer preparation and failed preparation without issuing SPI.
- A `1534` response with an unrelated header byte of either `00` or `EF`:
  both retain the FT9338 identity established by OTP `10`.
- C6 acknowledgement and transport failures across FT9365, FT9769/9391,
  FT9769/9392, FW9369 DB, and FW9369 SMIC, including subsequent image capture.

Validation results:

| Configuration | Result |
| --- | --- |
| Authentication disabled, FTE3600 CI suite | 28/28 groups passed |
| Personal authentication and IPA enabled, FTE3600 CI suite | 28/28 groups passed |
| AddressSanitizer and UndefinedBehaviorSanitizer | 16/16 selected groups passed; leak detection disabled |
| All drivers plus FTE3600, personal authentication enabled with IPA disabled | Build passed; 33 groups passed, 32 optional emulation groups skipped |

The all-driver hwdb comparison was run with LF line endings matching the
committed file. The earlier failure came from the Windows checkout's CRLF
conversion; the generated and committed contents are identical. The skipped
groups require optional emulation/introspection support absent from this build.

The installation guide now uses the existing setup/firmware tools and documents
the spidev buffer, pairing, module-signing, and SELinux prerequisites. The wire
reference was corrected against the packet builders and vendor evidence,
including the separate 10-byte Ubuntu and 12-byte Windows FW9369 word reads.

Physical Medion/GPD validation remains the next check for hardware response and
capture quality. In particular, automated call-order tests do not measure GPIO
release-to-first-clock time on the target SPI controller.
