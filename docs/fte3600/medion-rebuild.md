# Medion E3224: evidence for an independent backend

Updated 2026-10-01. The previous Medion implementation has never worked on the
reported machine. Its commands, FT9361 identity assumption, reset polarity,
firmware requirement and image geometry are not inputs to the new design.
A1 code may illustrate libfprint APIs; A1 hardware results establish nothing
about Medion protocol compatibility.

## Starting evidence

The same Medion E3224 worked on Mint 22.2 with vobademi's userspace package,
ctfdavis's SPI module and fingwit. This is an accepted hardware success report;
the reporter does not need to prove that history again.

- [Original success report](https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3288100306):
  replacing the SPI module with ctfdavis's code resolved `init sensor error!`.
- [Same-machine confirmation](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5830566555):
  Mint 22.2 worked; the old stack had not been tried on Fedora 44.
- [Reported topology](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5559055022):
  DMI `MEDION / E3224 / FT / YS13G`, SPI device `FTE3600:00`, host
  `0000:00:19.0`, GPO1 pin 39 and GPO2 pin 0. These are resource identifiers,
  not proof of a sensor model or the electrical purpose of pin 39.
- [Later checked-PM test](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5813414994):
  PCI host reached D0/active while the attempted FT9361 protocol still returned
  zeros. This neither proves nor disproves power at the sensor.

## Do not conflate the two surviving SPI sources

| Source | Behavior | What is established |
| --- | --- | --- |
| [ctfdavis alternative, pinned source](https://github.com/vobademi/FTEXX00-Ubuntu/blob/a0a35d3e47873a1712e02e3210f0e6e4fe75eeee/alt/focal_spi.c) | Mode 0, 8 bits, maximum speed capped at 1 MHz; optional named `reset`, `power`, then `enable` GPIO lookup | The project attributes it to the author named in the success report. This surviving copy was committed later; it is not a proven byte-for-byte copy of the installed Mint module. |
| [Later proposed attachment](https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3402710422) | Mode 0, 4 MHz, mandatory unnamed GPIO resource 0, raw low/10 ms/high reset | The author explicitly asked tuxman2 to validate this simplified patch after the success report. It is not the confirmed successful module. |

SHA256 of the surviving alternative is
`fdcf6e583291ab719007896b38742a0ff2cfbdc26e5f9de10d22959932902b2c`.
The later attachment is
`ed1c273d4988f8d490b7a81fd76e47927ab24785f0f400691f34cf3ba137b2f8`.

The named optional lookups do not imply a claim on unnamed GPO1 pin 39.
[Linux documents the distinction](https://docs.kernel.org/firmware-guide/acpi/gpio-properties.html#using-the-crs-fallback).
The supplied FP05 fragment has no named GPIO mapping. Therefore the first
candidate models absent mappings and makes no GPIO requests. A mapping elsewhere
in the complete firmware namespace remains possible; this is an explicit
assumption, not a measured fact from the old Mint boot.

## Independently recovered userspace protocol

The archived Ubuntu package is a reference artifact, not a firmware image.
Its host library opens `/dev/focal_moh_spi`; that kernel ABI uses a five-byte
header which is not transmitted to the sensor. Reads send a command and then
receive data in a single SPI message, keeping chip select asserted between
the phases. They are not the A1 full-duplex register transactions.

Static analysis of library SHA256
`4ee33eb988a62413698d1761b9c9baf09faca6a645f82ac19db6f33d04c8cb1b`
identifies a FW9362 probe followed by a separate other-93xx family path. That
library has not been executed during development, and its hash has not been
bound to tuxman2's installed copy. No vendor code or binary is imported into
the host implementation.

The implemented prefix is deliberately finite:

1. Require a non-zero original spidev maximum so it can be restored, then
   configure Mode 0 / 8-bit / MSB first and cap that maximum at 1 MHz.
2. With no mapped optional GPIO, retain the bridge reset ioctl's unconditional
   10 ms wait. The archived userspace adds no delay between reset and C6.
3. Send `09 f6 c6 01`, wait 4 ms, then TX `08 f7 c6 00` / RX 1.
   Stop retrying on `01`, with at most four attempts.
4. TX `04 fb 9a 8b 00 00` / RX 4, with TX and RX in one message.
5. Record the response and restore SPI configuration and temporary host PM
   settings. Do not continue into voltage setup, calibration or capture.

Both C6 acknowledgement and the expected `9362` candidate are required for
exit 0. It means only that this prefix matched. Unknown IDs, zero-filled
responses and the shifted-ID condition return 2; failed/short I/O is an error.
Even successful identification does not prove working enrollment.

## Implementation boundary and next hardware checkpoint

`fte3600-legacy-proto.[ch]` contains new protocol primitives independent of
FT9361/A1. The SPI transport can require one unsplit sequential message and
rejects short transfers. The standalone diagnostic uses this protocol without
entering any FT9361 state machine. The ordinary driver rejects Medion before
opening SPI or claiming GPIO, so fprintd does not run the obsolete A1 path.
This is a bring-up foundation, not a finished Medion capture backend.

The single next checkpoint is described in the
[test instructions](../medion-legacy-id-test.md): `--ctfdavis-run`.
The output already contains revision, actual requested speed, C6 results,
identity response and cleanup status. No fingerprint data is collected. The
previously reported kernel version and Mint success history need not be
requested again. No issue reply has been posted automatically.

After a reproducible response, select and implement the matching family's
complete calibration, finger detection and image pipeline. Static recovery
still has unresolved dynamic calibration/filter details; neither a hard-coded
capture transcript nor an A1 64x80 assumption closes those gaps. Then validate
repeated open/close, cancellation, finger removal, cold boot and resume on
Medion before enabling capture/enrollment or requesting upstream inclusion.
