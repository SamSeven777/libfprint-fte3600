# Medion E3224: source-backed legacy identity comparisons

These tests compare two bounded bridge candidates with protocol recovered from
an archived userspace library. The primary comparison follows the public
ctfdavis alternative named in tuxman2's successful Mint report. The secondary
comparison follows a different 4 MHz/GPIO patch posted later for validation.
Neither proves the exact files installed on Mint, and neither is a complete
initialization, capture, enrollment, or authentication test.
These diagnostics do not enable or claim production support for the Medion.

## Provenance boundary

The issue chronology distinguishes two different bridge sources:

- tuxman2's successful Mint report says that ctfdavis's alternative bridge
  resolved `init sensor error`:
  <https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3288100306>
- vobademi later posted a one-line patch and explicitly asked for it to be
  validated:
  <https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3402710422>

The closest public ctfdavis source now available is pinned for review, but it
is not proof of the historical Mint file:

- source: <https://github.com/vobademi/FTEXX00-Ubuntu/blob/a0a35d3e47873a1712e02e3210f0e6e4fe75eeee/alt/focal_spi.c>
- commit: `a0a35d3e47873a1712e02e3210f0e6e4fe75eeee`
- SHA-256: `fdcf6e583291ab719007896b38742a0ff2cfbdc26e5f9de10d22959932902b2c`

The secondary 4 MHz test is based on the attachment from the later validation
request:

- source: <https://github.com/user-attachments/files/22909015/focal_spi.c>
- SHA-256: `ed1c273d4988f8d490b7a81fd76e47927ab24785f0f400691f34cf3ba137b2f8`
- size: 13,915 bytes

This attachment is not the ctfdavis optional-named-GPIO/1 MHz alternative
cited by the successful report. Its presence in the issue is evidence for a
proposed experiment, not evidence that it ever worked on the Medion.

The protocol bytes below were recovered from an archived userspace library
with SHA-256
`4ee33eb988a62413698d1761b9c9baf09faca6a645f82ac19db6f33d04c8cb1b`.
It was inspected statically and was never installed or executed. No available
evidence proves that this exact library and either reviewed bridge source were
the files installed together on the old Mint system.

## Primary ctfdavis comparison

The pinned alternative bridge:

1. selects Mode 0, 8 bits, and MSB first;
2. uses 1 MHz when the kernel-side maximum is zero or above 1 MHz, otherwise
   keeps the existing non-zero maximum;
3. looks only for optional named `reset`, `power`, then `enable` GPIOs;
4. skips its probe reset when none of those named mappings exists.

The reported Medion ACPI fragment contains an unnamed GPIO resource, not one
of those named mappings. The diagnostic therefore models this source path
without requesting or changing any GPIO. The bridge's reset-ioctl handler
still calls a routine with an unconditional 10 ms sleep between its GPIO
setters. With no optional GPIO descriptor, the pin operations have no
electrical effect, but that 10 ms delay remains. The diagnostic models that
delay before sending the C6 and identity transactions below.

Static inspection of the archived userspace library shows no additional 2 ms
delay after the reset ioctl: its reset call is followed immediately by the C6
helper. The diagnostic therefore does not add one.

The generic-spidev diagnostic intentionally has a stricter speed precondition
than the kernel bridge. Before changing any SPI setting or sending traffic, it
requires spidev to report a non-zero original maximum speed. A zero value
fails closed; it is not converted to 1 MHz and later "restored" by writing
zero. This preserves the diagnostic's recoverable-state promise. The public
bridge's zero-to-1-MHz fallback occurs during kernel-driver probe and is not
evidence that writing zero back through spidev is a valid restoration.

This is a source-backed comparison with the code family named in the success
report. It is not a claim that the pinned revision is byte-identical to the old
Mint installation.

## Secondary 4 MHz attachment comparison

The later 4 MHz attachment:

1. set the SPI device to Mode 0, 8 bits, MSB first, and a 4 MHz maximum;
2. obtained unnamed ACPI GPIO resource index 0 with `GPIOD_OUT_LOW`;
3. drove descriptor value `0`, waited 10 ms, then drove `1`;
4. required a level-high IRQ and initially disabled it.

The attachment does not register an ACPI GPIO polarity mapping. Its unnamed
`devm_gpiod_get_index(..., NULL, 0, ...)` lookup uses the `_CRS` resource, and
ACPI `GpioIo()` itself has no polarity field. Consequently its sequence is raw
low, wait 10 ms, raw high. It must not be inverted using the unproven FT9361
reset interpretation.

The diagnostic models both the bridge probe cycle and the library's reset
ioctl cycle: each is raw low, waits 10 ms, then raw high. It does not add an
unsupported 2 ms delay before C6.

## Common bounded protocol

The recovered userspace library contains this FW9362 branch:

```text
up to four times:
  TX 09 f6 c6 01
  wait 4 ms
  one SPI message, CS held:
    TX 08 f7 c6 00
    RX 1 byte, expect 01

one SPI message, CS held:
  TX 04 fb 9a 8b 00 00
  RX 4 bytes, first two bytes are the big-endian identity
```

Both comparisons run the C6 loop and one FW9362 identity read. C6 is a sensor
register write, so these are bounded identity checkpoints, not read-only
probes. They deliberately stop before `1a84`, shifted-ID recovery, the
other-93xx `FD/FE` electrical configuration, FDT/calibration, IRQ capture,
image data, or either firmware path.

## Safety and interpretation

Both comparisons change the SPI device's maximum speed temporarily and write
C6. The ctfdavis comparison does not request a GPIO. The later-attachment
comparison additionally changes Pin 39 and leaves it raw high before release.
The wrapper restricts both runs to the exact `MEDION / E3224 / FT / YS13G`
profile, keeps the resolved SPI controller runtime-active, isolates `fprintd`,
and restores the SPI settings, power policy, and service state on handled exits.

- Exit `0`: C6 read back `01` and the FW9362 branch returned identity `9362`.
- Exit `2`: traffic completed but C6 or identity was inconclusive. Raw bytes
  remain useful evidence; this is not proof that the sensor is absent or dead.
- Exit `1`: setup, transfer, controller-state, or cleanup failed.
- Exit `128 + signal`: the run was interrupted and cleanup was attempted.

The archived FW9362 branch does not validate a CRC on this response. “Accepted
identity” therefore means that it matches the old branch's exact value, not
that authenticity or physical chip marking has been independently verified.

## Preflight and run

Collect the bounded host snapshot first:

```sh
bash scripts/collect-medion-spi-host-state.sh
```

Compile without hardware or service access:

```sh
bash scripts/test-medion-legacy-id.sh --check-only
```

Run the primary ctfdavis comparison first and preserve the complete output:

```sh
set -o pipefail
sudo bash scripts/test-medion-legacy-id.sh --ctfdavis-run 2>&1 | tee ../medion-legacy-ctfdavis-id.log
printf 'Test/logging exit status: %s\n' "$?"
```

Only if a separately reviewed comparison is still useful, run the later
unverified 4 MHz/GPIO attachment candidate. `--historical-run` is retained as
a compatibility name; it does not mean this attachment was the working Mint
bridge:

```sh
set -o pipefail
sudo bash scripts/test-medion-legacy-id.sh --historical-run 2>&1 | tee ../medion-legacy-historical-id.log
printf 'Test/logging exit status: %s\n' "$?"
```

The earlier single identity read without GPIO or C6 is retained only as a
limited framing comparison:

```sh
sudo bash scripts/test-medion-legacy-id.sh --run
```

It does not reproduce either bridge candidate's initialization prefix.
