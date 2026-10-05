# Medion E3224 spidev diagnostics

The `medion-spidev` branch provides an experimental, standalone diagnostic path
for the Medion E3224 wiring under investigation. Its recommended first hardware
stage, `--identify-legacy`, checks the FT9338 and FT9348 identification paths
without uploading firmware, initializing a capture backend or acquiring an
image. The explicit `--boot ft9338` and `--boot ft9348` stages can then test
the selected candidate's catalogued RAM firmware. Separate stages reuse this
repository's general sensor discovery,
initialization and capture state machines. The tool does not
establish that the Medion sensor works, or that every laptop sold as an E3224
has the same sensor and wiring.

This is a board-specific tool, not the normal FTE3600 installation procedure.
The normal driver continues to require the ACPI resource bridge. The standalone
tool does not enumerate devices through fprintd or install a replacement system
libfprint.

## What is known about the Medion sensor

Tuxman2's [E3224 hardware report](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5559055022)
identifies an SPI device with ACPI ID `FTE3600`, product version `FT` and board
`YS13G`. It does not contain a confirmed sensor model.

The [FocalTech repository's issue #9](https://github.com/focaltech-systems/drivers-input-touchscreen-FTS_driver/issues/9)
lists `FTE3600` and `2808:9338`, explicitly obtained from a Windows INF.
[Tuxman2's reply](https://github.com/focaltech-systems/drivers-input-touchscreen-FTS_driver/issues/9#issuecomment-2726462940)
says his laptop has the same reader, but supplies no chip-register response.
In the audited Windows package 2.0.3.102, `ACPI\FTE3600` selects the SPI install
section and `USB\VID_2808&PID_9338` selects the USB install section. These are
separate device matches in one package. FT9338 is the leading candidate for
this experiment, with FT9348 as the other identification target. This candidate
choice does not force a backend or replace a response from the sensor.

Linux Hardware's [published index](https://github.com/linuxhw/TestDays/blob/main/Location/Belgium/README.md)
contains E3224 probe [`9def2aed31`](https://linux-hardware.org/?probe=9def2aed31).
Its separate [E3224 DMI record](https://github.com/linuxhw/DMI/blob/master/Convertible/Medion/E/E3224/4DDCFDF3E5EE)
records the Akoya family, YS13G board and SKU `30033784`. These establish a
matching computer model, not a fingerprint chip identity or ownership by
Tuxman2. The original probe's device logs could not be retrieved during this
investigation.

Medion's [official driver download 18518](https://www1.medion.de/downloads/index.pl?id=18518&lang=uk&op=detail&type=treiber)
lists `drve322x_w10.exe`, dated 2018-07-24, for E3221 through E3228. Its archive
could not be downloaded during this check, so no claim about its fingerprint
driver contents follows from the download page.

## Requirements and build

Use the `medion-spidev` branch and run commands from the repository root. The
tool requires the distribution's `spidev` driver and the
[GPIO character-device v2 interface](https://docs.kernel.org/userspace-api/gpio/chardev.html),
available since Linux 5.10. The GPIO interface is used directly; libgpiod is not
required.

For Fedora, the compiler and library dependencies follow the existing
[build guide](install.md#1-prerequisites--dependencies):

```sh
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel systemd-devel \
  systemd cairo-devel python3 cabextract kmod
```

Build the standalone target:

```sh
meson setup build-medion -Ddrivers=fte3600 \
  -Dfte3600_medion_spidev=true -Dfte3600_personal_auth=false \
  -Dfte3600_ipa_auth=false -Dintrospection=false -Ddoc=false \
  -Dgtk-examples=false -Dinstalled-tests=false -Dwerror=true --prefix=/usr
ninja -C build-medion examples/fte3600-medion
```

Do not run `ninja install` or `setup-fte3600.sh` for this diagnostic path. The
`--prefix=/usr` option does not install anything when building this target.
No custom kernel module, DKMS setup, module signing or service installation is
needed. Keep Secure Boot and SELinux enabled; this tool does not change either
policy. Distribution module loading and access to SPI/GPIO nodes must still be
permitted by the running system.

Secure Boot does not require signing this userspace executable. The distribution's
kernel and any loaded modules must still be trusted. Running the diagnostic from
an administrator's terminal also avoids the `fprintd` service's confinement, but
`sudo` does not bypass SELinux: a confined terminal or local policy can still
deny the required device access. No Fedora E3224 hardware success is claimed.

## Inspect before operating the sensor

The launcher is `scripts/medion-spidev.py`. Its default stage, `--inspect`, only
checks the host configuration. It must find an unambiguous ACPI association
between the FTE3600 SPI device and the expected GPIO controllers. A matching
ACPI name alone is not a sensor identity. This checks the known board's ACPI
namespace paths and hardware IDs; it does not parse or validate the running
firmware's `_CRS` resource descriptors.

Inspection also reports the kernel release, firmware Secure Boot state, kernel
lockdown state, SELinux enforcement and the current process security context,
when readable. Unavailable values are reported as unknown. Firmware Secure Boot
state and effective kernel lockdown are separate observations. Inspection does
not load `spidev`, open SPI/GPIO nodes or request GPIO lines; a successful
`--inspect` is not proof of device-access permissions or working SPI.

The board configuration is deliberately fixed:

| Resource | Configuration |
| --- | --- |
| SPI | Existing mode 0 and active-low chip select; at most 1 MHz, preserving an existing lower speed limit |
| Reset | ACPI controller `GPO1`, line offset 39; physical high/low/high |
| Finger interrupt | ACPI controller `GPO2`, line offset 0; rising edge |

These are controller-relative line offsets, not global GPIO numbers. The
launcher resolves the controllers from their ACPI associations instead of
assuming `/dev/gpiochip0` and `/dev/gpiochip1`. It refuses ambiguous or missing
resources, and a SPI controller with another attached device. It does not switch
chip-select polarity: a device initially configured with active-high CS is
rejected. This configuration is an explicit Medion experiment, not a claim
that these offsets apply to other FTE3600 devices.

The CS choice follows the archived ctfdavis implementation's
[`spi->mode = SPI_MODE_0`](https://github.com/vobademi/FTEXX00-Ubuntu/blob/a0a35d3e47873a1712e02e3210f0e6e4fe75eeee/alt/focal_spi.c#L181)
and [Tuxman2's report that ctfdavis's code worked](https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3288100306).
It is not a controlled demonstration that changing CS alone fixed the reader:
the earlier stack also had different GPIO and initialization behavior. CS
selection and reset GPIO polarity are distinct settings.

Only an unbound SPI device or one already using `spidev` is eligible. A device
owned by the custom `fte3600` bridge or another driver is left alone. The
launcher may load the distribution's `spidev` module with a 32768-byte buffer
request; it does not unload an already loaded module to enlarge a smaller
buffer. An existing buffer of at least 64 bytes allows execution to proceed,
with its actual capacity reported. The backend checks each chip's transaction
requirements and may reject an identified chip when the buffer is too small;
protocol frames are not split to make them fit. A temporary
binding and driver override are restored on normal completion and handled
failures; a module loaded for the experiment remains loaded.

An explicit FT9338 boot needs at least 14192 bytes; FT9348 needs 10319 bytes.
If `spidev` is already loaded with a smaller buffer, the launcher refuses that
boot before pausing `fprintd` or changing the device binding. A newly loaded
module's actual buffer is checked again. These are minimum complete-frame
sizes, not proof that the SPI controller accepts the transaction. An existing
4 KiB or 8 KiB buffer can still run the short identification queries.

## Stages and side effects

| Stage | What it establishes | Sensor operations |
| --- | --- | --- |
| `--inspect` | Reports the known ACPI associations, candidate nodes and current driver binding | None |
| `--identify-legacy` | Application and ROM/OTP evidence for FT9338 or FT9348, preserving candidate versus confirmed-profile distinctions | Application register queries, bounded software wake and reset/ROM/OTP negotiation; no firmware upload, capture initialization or image acquisition |
| `--boot ft9338` or `--boot ft9348` | Whether the explicitly selected firmware starts with its expected runtime parameters | Validate the selected payload, reset, upload to RAM, verify transfer and application status; no capture or IRQ request |
| `--probe` | Whether the existing protocol discovery can establish a supported identity | SPI queries, wake commands and, when needed, reset/ROM negotiation |
| `--init` | Whether that identified chip completes its existing initialization and cleanup | Probe plus chip initialization; eligible recovery may load matching firmware |
| `--capture OUTPUT` | Whether initialization and a finger-triggered acquisition produce an image | Initialization, capture and cleanup; writes the requested local PGM |

Start with host inspection, then run the focused identification stage:

```sh
python3 scripts/medion-spidev.py --inspect
sudo python3 scripts/medion-spidev.py --identify-legacy
```

`--identify-legacy` uses native SPI/GPIO access directly, without first requiring
the general device initialization (`GInitable`) to identify the chip. It uses
SPI and reset GPIO synchronously and does not request or wait for the IRQ line;
the launcher still checks the board's GPIO controller mappings. The later
probe, initialization and capture stages retain their IRQ setup. It reads
the application registers `0x14` and `0x15` and can use the documented
ROM-A8 or boot-B38 OTP identification paths. After stable empty application
responses it first tries the bounded legacy software wake sequence: single-byte
`70`, 5 ms, single-byte `70`, 2 ms. A responsive runtime can be identified without
toggling reset. The bounded MCU-status retry and settling delays follow the
shared legacy discovery behavior. Only persistent empty responses reach the
existing ROM experiment; conflicting responses or I/O failures stop the test.
A prior `0x58/0x58` application
response is not an entry requirement: a previous all-zero generic probe can
still be followed by this explicit identification experiment. If all paths
return only zeroes, or the responses remain ambiguous or unrecognized, the
result stays unknown; it does not select FT9338 or FT9348 by elimination. Read
the reported evidence and stage result together.

This action does not change the normal driver's discovery rules. It does not
upload firmware, run automatic chip initialization or fall through to capture.
This action rejects `--firmware`; no environment variable enables a RAM boot.
It is mutually exclusive with all other stages.

## Boot the selected candidate's RAM firmware

FT9338 is the first candidate for this experiment. `--boot ft9338` explicitly
selects its RAM startup sequence; an initial `0x0000` response is allowed and
successful prior identification is not required. The tool rejects an observed,
nonempty identity that contradicts the selected profile. A failed FT9338 boot
does not automatically retry FT9348. `--identify-legacy` remains available when
only identification is wanted.

Download and extract the selected payload from the installer's fixed Microsoft
Update Catalog package, without installing it system-wide:

```sh
sh scripts/install-firmware.sh --chip ft9338 --download \
  --destdir "$PWD/medion-firmware"
sudo python3 scripts/medion-spidev.py --boot ft9338 \
  --firmware "$PWD/medion-firmware/fte3600/ft9338.bin"
```

The download needs Python 3 and either `cabextract` or `7z`. Use the explicit
`--chip` argument; the installer's old no-argument entry point selects FT9361.
The installer verifies the selected payload's exact size and SHA-256. No Windows
code is executed and no vendor binary is distributed in this repository.

Alternatively, use a local Windows driver DLL or an already extracted raw
firmware file. These commands do not download a package:

```sh
sh scripts/install-firmware.sh --chip ft9338 \
  --input /path/to/ftWbioUmdfDriverV2.dll --destdir "$PWD/medion-firmware"
sudo python3 scripts/medion-spidev.py --boot ft9338 \
  --firmware "$PWD/medion-firmware/fte3600/ft9338.bin"
```

For a raw file, replace the installer's `--input` value with its path, such as
`/path/to/ft9338.bin`. The installer also accepts a local CAB; use
`sh scripts/install-firmware.sh --help` for input options. No firmware payload
is distributed in this repository.

FT9348 is a separate, explicit choice, using its own payload and startup
sequence. Choose these commands only when running that candidate's experiment:

```sh
sh scripts/install-firmware.sh --chip ft9348 \
  --download --destdir "$PWD/medion-firmware"
sudo python3 scripts/medion-spidev.py --boot ft9348 \
  --firmware "$PWD/medion-firmware/fte3600/ft9348.bin"
```

For a local FT9348 input, replace `--download` with `--input /path/to/file`.

`--firmware PATH` is optional and valid only with `--boot`. Without it, the C
tool uses `/usr/lib/firmware/fte3600/ft9338.bin` or
`/usr/lib/firmware/fte3600/ft9348.bin`, according to the selected chip. A custom
file must also match that chip's catalog entry; its name does not select a
profile. The launcher passes an absolute path, and the C loader verifies a
bounded regular-file snapshot before opening or configuring SPI:

| Selected chip | Payload size | SHA-256 |
| --- | --- | --- |
| FT9338 | 14184 bytes | `ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46` |
| FT9348 | 10312 bytes | `48d658d588c297a5d749c1f4bd6a0f5bd3d6ede59040e1674f95fe9db08eede2` |

Each boot uses its own upload and start protocol. FT9338 additionally reads
back the complete uploaded payload before starting it. The tool then checks
application status, geometry, firmware version and AGC version against the
selected profile. It uses synchronous SPI and reset GPIO without requesting or
waiting for IRQ; the launcher still validates the fixed GPIO controller
mappings. This uses the distribution's `spidev` module and needs no custom
kernel module. It exits after the boot checks without acquiring an image or
entering an authentication flow.

A successful boot means the selected firmware started and its runtime
parameters matched. It does not independently identify the underlying silicon
or demonstrate capture. On failure, the tool releases reset and restores host
transport settings; the sensor's ROM/application state is unknown, and a
working application-idle state is not promised. Also check the launcher's
binding and service restoration result. Neither candidate has a confirmed
Medion hardware success from these synthetic tests.

## General driver stages

The general stages remain available for follow-up after reviewing the
identification result. Their normal discovery and recovery rules are unchanged:

```sh
sudo python3 scripts/medion-spidev.py --probe
sudo python3 scripts/medion-spidev.py --init
sudo python3 scripts/medion-spidev.py --capture "$PWD/medion-capture.pgm"
```

The launcher uses `build-medion/examples/fte3600-medion` by default. Use
`--tool /absolute/path/to/examples/fte3600-medion` only when using a different
build directory. Keep any stage's exact failure and determine whether it came
from host access, identification or sensor initialization. Host-access failures
must be resolved before another hardware stage. Candidate RAM boot requires an
explicit `--boot` choice; a general-stage failure never selects it automatically.

Inspection needs no root privileges. Operating the sensor requires root. The
diagnostic requests cancellation after 60 seconds per protocol stage; Ctrl+C
also requests cancellation and cleanup. A running kernel SPI ioctl must still
complete before its resources can be released, so this is not a guarantee that
every stalled controller returns within 60 seconds.
If the launcher must forcibly kill an unresponsive diagnostic after an
interrupt, the diagnostic cannot run its SPI/GPIO cleanup. That outcome is
reported as a failure; restoration of SPI parameters and reset state is then
not guaranteed.

`--identify-legacy` and `--probe` are **not read-only hardware inspection**.
The focused action can reset the device and enter ROM to obtain OTP evidence.
General discovery includes the
bounded wake and ROM negotiation paths documented in
[dynamic discovery](dynamic-discovery.md) and [special probing](special-probe.md).
It does not upload firmware. Initialization can use firmware only after the
existing positive identity checks authorize the specific recovery path; an
all-zero response never selects an FT9361 fallback.
The standalone diagnostic clears an inherited `FTE3600_FT9368_UPDATE` setting,
so this tool does not opt into the separate FT9368 persistent-flash update path.

Operational stages temporarily mask and stop fprintd to prevent concurrent
access. On normal completion and handled failures, the launcher restores its
prior state: it removes only a runtime mask that it added and restarts the
service only if it was previously active.
It does not install a permanent service override or change authentication
configuration. Restoration failures must be reported as failures, not hidden
behind a successful probe or capture result.

The standalone adapter owns each SPI/GPIO session from open through close.
It takes a cooperative exclusive lock on the spidev descriptor, retains the
fixed existing mode without writing CS polarity, and verifies the configured
mode, word size and speed before and after SPI transfers and during IRQ waits.
Stock spidev can hide the internal CS_HIGH flag on GPIO-controlled CS; the mode
readback is therefore not a measurement of the physical chip-select level.
The synchronous identification and boot callbacks use the same checks. A
configuration mismatch or failed configuration read invalidates the session
until close; later matching settings cannot revive it. Close releases IRQ
before reset, restores and reads back the original word size and
speed, and reports restoration failures even after otherwise successful sensor
operations. These checks do not provide the ACPI glue's suspend generation:
keep the system awake during this standalone experiment and restart the tool
after any suspend or device rebind.

The sensor should be uncovered before initialization. In particular, the
FW9369 backend measures an uncovered baseline during initialization; placing a
finger on the sensor at that point can spoil calibration. Touch the sensor
when capture is waiting for a finger.

## Reading results

Keep host-access failures separate from sensor responses. Failure to load the
distribution module, claim a GPIO line, configure SPI or open a node is not an
unsupported-chip result. Likewise, successful host inspection does not show
that the sensor is powered, reset correctly or answering SPI commands. For
`--identify-legacy`, distinguish an application register signature from ROM
family plus OTP evidence. The application signatures `0x58/0x58` and
`0x60/0x60` correspond to the FT9338 and FT9348 runtime profiles respectively;
they are runtime geometry signatures, not USB IDs. Without that runtime
evidence, a boot-B38 OTP classification is reported as an FT9338 candidate
according to the Windows identification rule, not as confirmation of an
immutable silicon chip ID. Raw observations can appear before cleanup finishes;
`IDENTIFY PASS` is emitted only after the C diagnostic restores its transport
state. Also check the launcher's final restoration result. Identification does
not establish successful initialization or capture. No Medion hardware
success is claimed for this new identification stage.

Reports should identify the stage, the confirmed identity and the error, without
printing fingerprint pixels, feature descriptors or templates. A requested PGM
is biometric data: the tool writes it only to an explicit local output file with
mode `0600`. Do not add that image or image-bearing SPI dumps to this repository
or attach them to an issue. The output path must not already exist, and a failed
save is removed when cleanup can do so. With the commands above, the file is
owned by root.

The name **FW9369** denotes the audited Windows backend/profile. Its confirmed
raw silicon response is **`0x9362`**; a report must preserve both facts rather
than rename a raw `9362` response to `9369`. Other legacy responses, such as the
application's width/height register pair, describe runtime geometry and are not
automatically a silicon product number. See the
[hardware inventory](windows-hardware-inventory.md) and
[FW9369 protocol](fw9369-protocol.md).

A successful `--probe` does not prove initialization, IRQ delivery or a complete
image transfer. The `IDENTITY` line can precede a failed probe: backend selection
also checks whether a complete transaction fits the transport. Read the stage
result and its error as well as the identity. A successful capture does not
validate enrollment, matching or
population FAR/FRR. Authentication is disabled in the build above. Record the
source commit, kernel, stage and sanitized result when reporting this Medion
experiment; no Medion hardware success is claimed by the existence of this
branch or its mock tests.

## Local validation, 2026-10-05

This update merges common-driver commit `6e25b20` and migrates the standalone
adapter to the shared open/close/session-check contract. It preserves direct
stock-spidev operation without the custom ACPI glue. The identification and
RAM-boot wire engines were not changed in this migration.

The merged source was built with GCC 13.3, warnings as errors, on Ubuntu 24.04
under WSL. Both `fte3600_personal_auth` and `fte3600_ipa_auth` were tested as
`false` and as `true`, with `fte3600_medion_spidev=true` and only the FTE3600
driver enabled. In each configuration, the complete Meson run finished with
**36 passing suites, 33 skipped suites, zero failures and zero timeouts**.
The skips were 32 unrelated recorded-driver replay suites (introspection was
disabled) and the generic generated-hwdb check. The passing general-driver
suite also skipped its optional external FT9361 firmware-file subcase.

| Regression | Cases in each configuration |
| --- | ---: |
| Medion launcher, including active ACPI-glue exclusion | 62 |
| Medion transport ownership, restoration and sticky session guard | 23 |
| Legacy identification engine | 18 |
| Native identification I/O, including pre/post-transfer guards | 14 |
| FT9338/FT9348 RAM boot engine | 13 |
| Standalone `--help` | 1 invocation |
| Shared public lifecycle | 144 with authentication disabled; 147 enabled |

No Medion case was skipped. The lifecycle run includes a separate-IRQ
standalone session through probe, initialization, capture, cancellation and
reopen, plus probe-close and final-close failure propagation. Test logs are in
`build-medion-sync/meson-logs/testlog.{txt,json}` and
`build-medion-sync-true/meson-logs/testlog.{txt,json}` in the validation worktree.

An independent AddressSanitizer/UndefinedBehaviorSanitizer run of the adapter
and syscall fixture passed all 23 transport cases. Those two objects were
instrumented; the linked shared libraries were not rebuilt with sanitizers,
and leak detection was disabled. This is targeted adapter evidence, not a
claim of a fully instrumented library run.

The branch's `scripts/check-fte3600.sh` now enables and runs all six standalone
suites alongside the shared regression suites in both policy configurations;
the GitHub workflow calls that script. No sensor was accessed and no kernel
module or policy was installed during these checks. Physical Medion operation,
Fedora Secure Boot/SELinux access and suspend behavior remain hardware checks.

## Historical validation, 2026-10-04

The standalone branch incorporates shared-driver fixes through main `1ce4c74`.
The Medion software-wake path and launcher checks were then verified with:

- A GCC 13.3 warnings-as-errors build on Ubuntu 24.04 under WSL.
- 32 passing selected suites, including Medion transport (17 cases),
  identification (18), native identification I/O (12), RAM boot (13), launcher
  (60), and the merged shared-driver lifecycle (121). The general driver suite
  skipped its optional FT9361 external-firmware and disabled udev-generator
  subcases; no Medion subcase was skipped.
- AddressSanitizer and UndefinedBehaviorSanitizer: all four Medion C suites
  and the shared-driver lifecycle passed.
- Actual `--chip ft9338 --download` and `--chip ft9348 --download` extraction
  from the fixed Microsoft package, confirming both payload sizes and hashes.

These checks did not access a sensor, install system software or load a kernel
module. Fedora SELinux enforcement, Secure Boot and the E3224's physical wiring
still require the user's hardware run.
