# Medion E3224 spidev diagnostics

The `medion-spidev` branch provides an experimental, standalone diagnostic path
for the Medion E3224 wiring under investigation. Its recommended first hardware
stage, `--identify-legacy`, checks the FT9338 and FT9348 identification paths
without uploading firmware, initializing a capture backend or acquiring an
image. Separate stages reuse this repository's general sensor discovery,
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
  systemd cairo-devel python3
```

Build the standalone target:

```sh
meson setup build-medion -Ddrivers=fte3600 \
  -Dfte3600_medion_spidev=true -Dfte3600_personal_auth=false \
  -Dintrospection=false -Ddoc=false -Dgtk-examples=false --prefix=/usr
ninja -C build-medion examples/fte3600-medion
```

Do not run `ninja install` or `setup-fte3600.sh` for this diagnostic path. The
`--prefix=/usr` option does not install anything when building this target.
No custom kernel module, DKMS setup, module signing or service installation is
needed. Keep Secure Boot and SELinux enabled; this tool does not change either
policy. Distribution module loading and access to SPI/GPIO nodes must still be
permitted by the running system.

## Inspect before operating the sensor

The launcher is `scripts/medion-spidev.py`. Its default stage, `--inspect`, only
checks the host configuration. It must find an unambiguous ACPI association
between the FTE3600 SPI device and the expected GPIO controllers. A matching
ACPI name alone is not a sensor identity. This checks the known board's ACPI
namespace paths and hardware IDs; it does not parse or validate the running
firmware's `_CRS` resource descriptors.

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

## Stages and side effects

| Stage | What it establishes | Sensor operations |
| --- | --- | --- |
| `--inspect` | Reports the known ACPI associations, candidate nodes and current driver binding | None |
| `--identify-legacy` | Application and ROM/OTP evidence for FT9338 or FT9348, preserving candidate versus confirmed-profile distinctions | Application register queries and bounded reset/ROM/OTP negotiation; no firmware upload, capture initialization or image acquisition |
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
ROM-A8 or boot-B38 OTP identification paths. A prior `0x58/0x58` application
response is not an entry requirement: a previous all-zero generic probe can
still be followed by this explicit identification experiment. If all paths
return only zeroes, or the responses remain ambiguous or unrecognized, the
result stays unknown; it does not select FT9338 or FT9348 by elimination. Read
the reported evidence and stage result together.

This action does not change the normal driver's discovery rules. It does not
upload firmware, run automatic chip initialization or fall through to capture.
There is no firmware-upload option or environment-variable opt-in for this
action. It is mutually exclusive with all other stages.

The general stages remain available for follow-up after reviewing the
identification result; do not choose or upload firmware merely from a board
name or a candidate chip:

```sh
sudo python3 scripts/medion-spidev.py --probe
sudo python3 scripts/medion-spidev.py --init
sudo python3 scripts/medion-spidev.py --capture "$PWD/medion-capture.pgm"
```

The launcher uses `build-medion/examples/fte3600-medion` by default. Use
`--tool /absolute/path/to/examples/fte3600-medion` only when using a different
build directory. Keep any stage's exact failure and determine whether it came
from host access, identification or sensor initialization. Host-access failures
must be resolved before another hardware stage; an unidentified chip is not
authorization to try guessed firmware.

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
