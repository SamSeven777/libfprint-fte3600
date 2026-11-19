# FTE3600 tools

Use `scripts/fte3600-tool.sh` from a checkout. It separates connection metadata,
chip identification through the production driver, and startup timing. It does
not install packages, load modules or change services automatically.

| Command | Purpose | Requirements |
| --- | --- | --- |
| `info` | Read-only ACPI, SPI and GPIO topology report; does not identify the chip | Bash; optional Python 3 and ACPICA tools for ACPI decoding; root for protected tables |
| `check` | Check installed kernel and service prerequisites | See [installation](install.md); no chip identification |
| `probe /dev/spidevB.C [firmware-directory]` | Report driver profile, chip IDs and startup result for one exact SPI device | Production build, root, matching ABI 2 bridge and spidev |
| `bench [cycles]` | Measure normal driver open/close time | Built libfprint and tool, configured device permissions; defaults to three cycles |

## Identify a chip

Build on the target distribution using the dependencies in
[installation](install.md). There is no need to install the built libfprint or
replace the system library to run these tools:

```sh
meson setup build -Ddrivers=fte3600 -Dfte3600-test-backend=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson compile -C build fte3600-probe fte3600-open-bench
./scripts/fte3600-tool.sh info
```

The ACPI resource bridge must be installed and loaded before chip probing.
Use the **actual** spidev character device associated with the FTE3600 sensor,
as shown by the topology report; `spi-FTE3600:01` is not a `/dev/spidevB.C` path.
Keep the sensor untouched and stop other sensor clients such as fprintd before
running a probe. On systems using systemd, for example:

```sh
sudo systemctl stop fprintd.service
sudo ./scripts/fte3600-tool.sh probe /dev/spidev2.0
sudo systemctl start fprintd.service
```

`/dev/spidev2.0` is an example, not a default or a guessed bus assignment. If the
service was previously disabled or stopped, restore that state instead. The
probe never starts or stops services itself.

The probe reuses the production runtime, including validated device pairing,
chip detection, firmware loading and full startup. **It performs hardware
writes and resets, and may upload firmware and run calibration.** It does not
request enrollment or verification, or save images or fingerprint templates.
It is not a passive, identification-only register reader.

Output distinguishes the selected driver profile, raw ID, chip ID, startup and
calibration. A raw ID and the selected profile may differ (for example,
FW9369 uses raw `0x9362`). Zero IDs in a failed startup may mean the ID was not
available, rather than a measured chip ID of zero. An unconfirmed-identity
message means the reported profile must not be treated as confirmed.

Missing firmware or failed calibration is not proof that a chip is unsupported.
The failure report retains the runtime's available identity and diagnostics.
Even a successful startup does not establish enrollment, verification or
suspend/resume compatibility. Share the full text output with a hardware report.

An optional firmware directory must contain the raw files described in the
installation guide. Otherwise the tool uses the driver's build-time firmware
directory (`-Dfte3600-firmware-dir=...`), including on NixOS. The imperative
kernel installation helpers are not NixOS packaging instructions.

For a different build directory:

```sh
sudo env FTE3600_BUILD_DIR=/absolute/path/to/build \
  ./scripts/fte3600-tool.sh probe /dev/spidev2.0 /path/to/firmware/fte3600
```

Exit status is `0` after successful startup, status retrieval and cleanup;
`1` for runtime/status/cleanup failure; `2` for incorrect arguments or a missing
built tool. Build the real backend: the fake test backend cannot identify hardware.

## Developer tools

| Tool | Scope |
| --- | --- |
| `tools/fte3600-probe.c` | Implementation of the production chip/startup probe; built as `build/tools/fte3600-probe` |
| `tools/fte3600-open-bench.c` | Public libfprint API startup benchmark; built as `build/tools/fte3600-open-bench`; supports `--cancel-open-ms=1..1000` |
| `tools/ft93xx-lab.py` | Experimental direct-register lab for raw IDs `0x9391`, `0x9392`, `0x9395`, `0x9397`; requires Python spidev and explicitly configured device paths; does not cover every driver profile |
| `tools/fte3600-match-lab.c` | Matcher development source; not a chip identification tool |
| `scripts/fte3600-pair.py` | Resource pairing, service permissions and labeling integration; not chip identification |
| `scripts/check-fte3600.sh` | Developer builds and software regression checks; not a hardware diagnostic command |

The legacy `scripts/probe-fte3600.sh` and `scripts/setup-fte3600.sh check`
entry points remain available for existing users. Their implementations are
reused by `info` and `check`. The FT93xx lab remains separate because it exposes
experimental register writes and capture operations. Its default device paths
are not portable; do not use it as the general hardware-report probe.
