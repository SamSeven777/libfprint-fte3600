# Hardware tools

Use `scripts/fte3600-tool.sh` from the repository root:

| Command | Purpose |
| --- | --- |
| `info` | Collect ACPI, SPI and GPIO connection information |
| `check` | Check installed kernel and service prerequisites |
| `probe /dev/spidevB.C [firmware-directory]` | Identify the chip and report its driver profile and initialization result |
| `bench [cycles]` | Measure driver open/close time; defaults to three cycles |

## Build

Install the [build dependencies](install.md), then build the production tool:

```sh
meson setup build -Ddrivers=fte3600 -Dfte3600-test-backend=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson compile -C build fte3600-tool
```

The tool runs against the built library directly. `probe` and `bench` require
the ABI 2 ACPI resource module and root access. `info` uses Bash, with Python 3
and ACPICA tools for ACPI decoding; run it as root to read protected tables.

## Identify a chip

```sh
sudo ./scripts/fte3600-tool.sh info
```

Find the sensor's `/dev/spidevB.C` path in the report. Substitute that path in
the command below; `/dev/spidev2.0` is an example. Close other sensor clients
and keep the sensor untouched during initialization. With fprintd running:

```sh
sudo systemctl stop fprintd.service
sudo ./scripts/fte3600-tool.sh probe /dev/spidev2.0
sudo systemctl start fprintd.service
```

`probe` uses the driver's chip detection and startup sequence, including reset,
firmware loading and calibration. It reports the driver profile, raw ID, chip
ID, geometry and initialization status. It produces text output without saving
images or templates.

Raw IDs and profiles can differ: FW9369 uses raw ID `0x9362`. On failed startup,
zero IDs mean the runtime may not have reached identification; `Identity
unconfirmed` marks an unresolved identity. Firmware and calibration errors are
reported separately. Share the full output when reporting a device, followed
by enrollment and verification results once initialization succeeds.

The optional firmware directory defaults to the driver's build-time
`fte3600-firmware-dir`. See [firmware setup](install.md#firmware).
For a custom build directory and firmware location:

```sh
sudo env FTE3600_BUILD_DIR=/absolute/path/to/build \
  ./scripts/fte3600-tool.sh probe /dev/spidev2.0 /path/to/firmware/fte3600
```

Exit codes: `0` for success, `1` for runtime/status/cleanup failure, and `2` for
invalid arguments or a missing executable. `bench` also accepts
`--cancel-open-ms=1..1000` for cancellation testing.

## Development

`tools/fte3600-tool.c` implements both compiled commands in
`build/tools/fte3600-tool`. The shell entry point contains the topology report
and delegates installation checks to `setup-fte3600.sh`.

`tools/fte3600-match-lab.c` is for matcher experiments;
`scripts/fte3600-pair.py` handles resource pairing and service permissions.
Run `scripts/check-fte3600.sh` for the software regression checks.
