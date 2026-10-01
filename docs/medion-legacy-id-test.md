# Medion E3224: bounded legacy-protocol identity test

This test isolates one material difference between the stack that reportedly
worked on this same machine under Mint and the current FTE3600 diagnostic. It is
not an enrollment test and does not identify the cause by itself.

## Why this test exists

Static analysis of the archived 2025 Ubuntu userspace library and the public
`focal_spi` bridge recovered this transaction for logical address `0x1a8b`:

```text
one SPI message, chip select held between its two transfers
TX: 04 fb 9a 8b 00 00
RX: four bytes
```

The analyzed `libfprint-2.so.2.0.0` has SHA-256
`4ee33eb988a62413698d1761b9c9baf09faca6a645f82ac19db6f33d04c8cb1b`.
It was inspected statically and was never installed or executed. The public
bridge corroborates the transfer framing, but may postdate the reported Mint
success; neither artifact alone proves the exact historical installation.

The old bridge used `spi_write_then_read()`. The earlier Medion diagnostic used
a different command family and one six-byte full-duplex transfer. Therefore the
all-zero result from that earlier diagnostic did not test this legacy framing.

The full archived library did more before and after this read. It requested a
reset, wrote and read back `C6`, and one fallback path wrote `FD/FE` in a routine
described by the binary as I/O-voltage configuration. It also retried and could
enter recovery paths. Those operations are intentionally **not** copied here.

## Exact experiment boundary

`--legacy-id-no-init`:

- accepts only MEDION / E3224 / FT / YS13G;
- resolves the character device through `spi-FTE3600:00` and verifies the
  opened node against the same sysfs object;
- holds only the resolved SPI/controller runtime-PM ancestors active;
- uses Mode 0, 8 bits, MSB first and 1 MHz;
- sends exactly one `SPI_IOC_MESSAGE(2)`: TX6 followed by RX4;
- does not request a GPIO, reset the sensor, write `C6`, `FD`, `FE` or `1a84`,
  query ROM, write scratch RAM, retry, upload firmware or run recovery;
- restores the prior SPI settings and runtime-PM controls on every handled exit.

This is active bus traffic, not a passive read. The RX-only transfer still
generates clocks and dummy MOSI bits. The command is documented as a read only
by the archived implementation; that is not proof that it is side-effect-free
on every unknown device.

## Collect host evidence first

Run the collector without `sudo` and share its complete output:

```sh
bash scripts/collect-medion-spi-host-state.sh
```

It reads a bounded set of existing sysfs/debugfs metadata. It does not mount
debugfs, open SPI/GPIO device nodes, change power management or services, or
execute an ACPI method. Missing debugfs data is reported as a gap. A later
snapshot may show autosuspend state, and `MUX UNCLAIMED` does not by itself mean
that firmware left a pin unconfigured.

## Build-only preflight

```sh
bash scripts/test-medion-legacy-id.sh --check-only
```

This compiles the current diagnostic in a private temporary directory. It does
not access the sensor or change `fprintd`.

## Active test

Only run this after the host snapshot has been reviewed:

```sh
set -o pipefail
sudo bash scripts/test-medion-legacy-id.sh --run 2>&1 | tee ../medion-legacy-id.log
printf 'Test/logging exit status: %s\n' "$?"
```

The wrapper temporarily masks and stops `fprintd`, runs only the bounded mode,
then restores the prior service state. A pre-existing mask is preserved. The
output stays local unless the user shares the log.

## Reading the result

- Exit `0`: the first two bytes are in the archived library's accepted ID set,
  and the non-zero trailer matches that library's other-93xx CRC convention.
  This is a protocol candidate, not proof of the physical chip model or working
  enrollment.
- Exit `2`: the transaction completed but returned all zero, all `ff`, an
  unknown identity, a zero trailer or a CRC mismatch. All four raw bytes remain
  useful evidence.
- Exit `1`: setup, SPI transfer, controller-state or cleanup failed.
- Exit `128 + signal`: the run was interrupted; cleanup is still attempted.

An all-zero or otherwise inconclusive result cannot rule out this protocol,
prove that the sensor is unpowered, or distinguish FT9361 from FT9362. The test
deliberately omits the archived stack's reset and configuration writes.
