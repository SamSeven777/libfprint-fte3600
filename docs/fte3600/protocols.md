# FocalTech FTE3600 Wire Protocols and Register Reference

[Documentation Index](README.md) · [Architecture](architecture.md)

This reference distinguishes the eight implemented sensor profiles, their wire
formats, and the supported boot paths. A protocol family is not a laptop model
or a unique chip identity. Runtime geometry, ROM-family responses, and silicon
IDs are different kinds of evidence.

## Transport and Reset

The userspace driver accepts SPI Mode 0, with CS polarity selected through the
ACPI glue when that capability is available. Transfers use 8-bit words. Bus
speed is bounded by the ACPI connection and the original spidev maximum; this
project does not establish a universal safe 12 MHz setting.

The physical reset pulse observed in the Windows helper is HIGH for 10 ms,
LOW for 20 ms, then HIGH. The GPIO userspace line is requested active-low:
logical `1` asserts reset and logical `0` releases it. This logical interface
must not be confused with the physical levels passed to the Windows GPIO
controller.

There are distinct uses of that pulse:

| Operation | Action after final release |
| --- | --- |
| Enter legacy ROM for identification or RAM download | Immediately send `55 AA`; no additional sleep |
| Start FT9338 application after RAM download | Finish the startup pulse sequence, then wait 80 ms |
| Start FT9536 application after RAM download | Finish the startup pulse sequence, then wait 180 ms |
| Start FT9348/FT9361 application after RAM download | Finish the startup pulse sequence, then wait 160 ms |
| Shared modern-family factory reset | Wait 10 ms before the next probe |

Legacy application startup uses two reset pulses with the documented 10 ms
inter-pulse interval. It must not be substituted for the single pulse followed
by ROM sync.

Linux queues the reset-release/sync compound operation while reset is still
asserted. The SPI worker validates the frame and generation, releases GPIO,
then sends the prepared two-byte frame through the normal full-duplex path.
No new worker, SSM transition, log, resource query, or intentional wait is
inserted between the two ioctls. The transfer result and generation are checked
afterward. This removes avoidable userspace gaps without claiming that GPIO
and SPI ioctls are one atomic kernel operation.

A full-duplex frame is already one `SPI_IOC_MESSAGE(1)` with CS held throughout.
Splitting it into multiple segments in a single message is possible, but does
not remove a second syscall from the current implementation. Framing, dummy
clocks, and response offsets must remain protocol-specific.

## Identification and Profile Selection

| Profile | Runtime/positive identity | Image size | Boot family |
| --- | --- | --- | --- |
| FT9338 | Legacy geometry `58 58`; B38 OTP high nibble 1 | 88 x 88 | B38 |
| FT9348 | Legacy geometry `60 60`; matching A8 OTP | 96 x 96 | A8 |
| FT9361 | Legacy geometry `40 50`; matching A8 OTP | 64 x 80 | A8 |
| FT9536 | Legacy geometry `40 80`; boot-A FE `02` or B38 OTP high nibble 2 | 64 x 128 | Legacy 38 |
| FW9369 | Silicon ID `9362` at word address `1A8B` | 64 x 80 | Host-controlled |
| FT9365 | Silicon ID `9365` at word address `1A8B` | 64 x 80 | Host-controlled |
| FT9769 | Silicon ID `9391` or `9392` | 40 x 196 | Host-controlled |
| FT9368 | Dedicated application information/identity route | 64 x 80 | Dedicated flash protocol |

Legacy registers `14` and `15` contain geometry, not the hexadecimal model
number. An inactive application is woken with `70`, 5 ms, `70`, followed by MCU
status inspection. The factory's 350 ms delay occurs after confirmed idle,
before geometry reads; it is not a reset-to-`55 AA` delay.

The fixed mailbox sequence starts with `06 F9 00`, operates on address `85C0`,
and yields a ROM-family value. `2B50`, `95A8`, and `23DD` select A8. The inspected
Windows classifier uses a catch-all legacy-38 branch for other successful
results; main explicitly supports the observed positive `1534` B38 response.
It enters the B38 OTP path directly. It does not interpret header-phase receive
bytes as a previous boot-A marker, and `1534` alone does not distinguish FT9338
from FT9536.

A8 OTP uses C8/F1/F4/F3 without the B38 reset/sync preamble. B38 OTP follows the
reset/sync preamble and its own four-byte boot-register format. Windows accepts
B38 OTP `FF` as FT9338 as well as high nibble 1; main currently requires positive
high nibble 1 or 2. This is an explicit compatibility difference. `00` is not a
positive model identity in either classifier.

Known IDs `9349`, `9363`, `9372`, and `9395` through `9398` remain unmapped to a
capture backend. Recognition in a vendor probe is not proof of a supported
image profile.

## Legacy Application and A8 Recovery

Application register access uses opcode/complement pairs `10 EF` for reads and
`11 EE` for writes. Important registers are `14/15` (geometry), `20/21` (MCU
idle signature `A5 5A`), and `1D` (capture state). Boot-register framing is
separate; use the builders in `fte3600-protocol.c` rather than substituting an
application opcode.

An image request starts `04 FB 34 00`, followed by the complete frame length in
big-endian order. The frame is `width * height + 8` bytes. Pixels start at
response offset 8 and are inverted byte by byte. FT9361 therefore uses a
5,128-byte frame; FT9348 uses 9,224 bytes; FT9338 uses 7,752 bytes; FT9536 uses
8,200 bytes.

FT9348 and FT9361 recovery uses the matching, hash-validated application image:
reset/sync, RAM upload, a 2 ms upload settle, the two startup reset pulses,
160 ms application boot, then the application soft-reset and idle checks.
They do not share one firmware image merely because their framing is shared.

## FT9338 / FT9536 RAM Recovery

This path is distinct from A8 recovery:

1. Validate the current identity and the selected chip's firmware.
2. Perform HIGH 10 ms / LOW 20 ms / release-and-`55 AA`.
3. Write C8=`FF`, CA=`FF`, CB=`FF`, B9=`BF`, then B9=`FF` using four-byte boot writes.
4. Wait 20 ms, then send one RAM upload frame: `05 FA 00 00`, the two-byte
   payload length, the payload, and one trailing zero byte.
5. Wait 2 ms and read RAM back using the corresponding `04 FB 00 00` frame.
   Compare the returned payload before starting the application.
6. Perform the two startup reset pulses and wait 80 ms for FT9338 or 180 ms for
   FT9536. Poll MCU idle at 2 ms intervals, at most 20 reads, then verify geometry.

The matching firmware sizes are 14,184 bytes for FT9338 and 11,934 bytes for
FT9536. There is no `12` upload / `13` jump-to-application sequence in this path.
See `fte3600-legacy-recovery-protocol.*` and `fte3600-legacy-recovery-timing.h`.

## FW9369 (Silicon ID 9362)

SFR writes use `09 F6 reg value`; SFR reads use `08 F7 reg 00` and one response
byte (five clocked bytes). C6 is the SPI-mode SFR, not the word-read opcode.

Initialization follows the inspected Windows C6 helper: write C6=`01`, wait
4 ms, read it back, up to 31 attempts per helper call. Two helper calls precede
the ID read. Exhausted acknowledgement retries do not independently veto a
valid chip ID; an actual transport failure remains an error.

The Windows-compatible word read for `1A8B` starts `04 FB 9A 8B 00 01`, clocks
12 bytes in total, and decodes the big-endian result at offset 6. The archived
Ubuntu 20250112 FW9369 path instead encodes count `0000` and clocks ten bytes.
That is a vendor-version/path difference; do not globally shorten this backend
to match a different helper.

Commands are three-byte opcode/complement/zero frames: `5A A5 00` wakes,
`A5 5A 00` completes wake, `C0 3F 00` requests idle, `C1 3E 00` requests deep
sleep, `C2 3D 00` starts finger detection, and `C4 3B 00` starts image acquisition.
C4 is not the bulk pixel-read transaction. Events are read from word address
`1A82`, enabled through `1A83`, and acknowledged through `1A84`.

`INVALID` (`0010`) is a recovery event, not proof of a particular DAC failure.
In Windows, `fw9369_query_event_status` tests this bit at RVA `103D3` and
returns the ESD status through `10824`. The IRQ handler at `2DE7C` waits 5 ms
and calls `fw9369_init_chip` through `2DF4A` / `FED8`. Initialization reacquires
FDT and image calibration/baselines (`157C0`) before restarting detection.
The preceding virtual call at `2DE99` resolves through FT9369 vtable
`49410 + 68` to `35F60`, a no-op; it does not pulse GPIO.

Linux acknowledges INVALID, RESET or ESD and reuses its initialization state
machine after the same 5 ms wait. Successful recovery resumes the current
DOWN or UP wait without discarding enrollment progress. It does not repeat
factory discovery or the GPIO reset used when reopening from C1, even when
the current open used a cached identity. A fresh finger event is still required.
Three recoveries without completing that capture/release wait are allowed;
this bound is Linux host policy. Calibration, transport, identity or cancellation
failures propagate normally, with the existing resource-generation checks.

The image FIFO read starts `06 F9 9A 05`. A frame contains a six-byte header and
10,240 bytes of big-endian 16-bit samples (10,246 bytes total). The host collects
an empty-sensor baseline and forms nonnegative baseline-minus-sample differences
before producing an 8-bit image. DAC search, stability qualification, and final
normalization are independent host policies, not a claim of identical Windows
image processing. Baseline acquisition must be evaluated on real hardware.

See `fte3600-fw9369-protocol.*` for register addresses and packet builders.

## FT9365 / FT9769

These use the same SFR command pairs but their word read of `1A8B` is
`04 FB 9A 8B 00 00`, ten clocked bytes total, with the result at offset 6. The
word trailer is checked using the implemented CRC/sentinel rules.

The Windows FT93xx C6 helper permits four write/4 ms/read attempts. Initialization
calls it during probing, validates the chip ID, then calls it again. An
unacknowledged C6 byte does not replace the subsequent identity decision.

Capture reads 16-bit raw samples in bounded FIFO chunks. The host masks samples
with `0FFC`, qualifies exposure and contrast, and normalizes to 8-bit grayscale.
FT9365 raw pixels occupy 10,240 bytes. FT9769 includes four extra raw rows:
40 x 200 x 2 = 16,000 raw bytes, cropped to 40 x 196 output pixels. The `9392`
profile also applies its documented pixel ordering. Simple byte inversion is
not an equivalent decoder.

See `fte3600-ft93xx-protocol.*` and `fte3600-ft93xx-timing.h`. The host polling
and exposure policies are distinct from the vendor's complete finger-detection
policy and require device testing for performance and power comparisons.

## FT9368

FT9368 has a dedicated seven-byte-header protocol. Command words include
`FF00` (wake), `9180` (application information), `9080` (image), and `F680`
(start). It is not identified through a single-byte FW9369 `5A` command.

Ordinary operation requires valid application information. The opt-in
`FTE3600_FT9368_UPDATE=1` path uses a 6,096-byte PRAM image and a 27,120-byte
application image, with boot identification, erase/program status polling,
checksum verification, and reboot. This is a persistent flash update, not a
routine RAM upload or a generic recovery for an unidentified blank device.
The PRAM and application transfer chunks are 128 and 256 bytes respectively.
See `fte3600-ft9368-protocol.*` and `fte3600-ft9368-timing.h` for the full encoding
and separate stage waits.

## Evidence Baseline

Windows addresses refer to `ftWbioUmdfDriverV2.dll` from package 2.0.3.102,
SHA-256 `0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.

| Behavior | Windows RVA evidence |
| --- | --- |
| Download reset followed by sync | `39730` -> `2EAD0`, GPIO helper `2F5E0` |
| A8 versus legacy-38 family classifier | `2831B` through `28335` |
| B38 OTP model classification | `27807` through `27889` |
| FT9338 / FT9348 / FT9361 / FT9536 profile restart waits | Constructors `3574C`, `35834`, `35914`, `35C44` |
| FW9369 bounded C6 configuration and ID ordering | Helper `FD94`, calls `100D7` and `10D79` |
| FT93xx bounded C6 configuration and ID ordering | Helper `195D0`, calls `199CA` and `19717` |

The archived Ubuntu 20250112 library uses C6 helper VA `158A2D` and word-read
helper VA `15F227`; those are ELF addresses, not Windows RVAs. Its reachable
SPI path and firmware data in the older 20240620 library's debug information
must be distinguished. Presence of bytes is not evidence that a particular
loader ran during a historical installation.
