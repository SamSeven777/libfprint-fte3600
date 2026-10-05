# FT9368 SPI application and explicit firmware update

This document records independently expressed wire facts from the 2.0.3.102
Windows driver whose SHA-256 is
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.
Addresses below are relative virtual addresses, for reproducibility. No vendor
code, disassembly, firmware bytes or initialization table is distributed here.
The Linux implementation is experimental: mock tests establish host behavior,
not electrical operation or successful fingerprint capture on a physical FT9368.

## Identity and application framing

FT9368 uses a different command protocol from FT9361. A command has a big-endian
16-bit command followed by a big-endian 16-bit payload length. A command with no
payload occupies four bytes. A read with a nonzero payload adds three zero dummy
bytes followed by zero transmit bytes for the payload. Receive data begins at
offset seven of the same full-duplex transaction (SPI wrapper `0x2e200`).

| Command | Payload length | Meaning established by the driver |
| --- | ---: | --- |
| `ff00` | 0 | Wake; application path waits 10 ms |
| `9180` | 32 | Device information and finger state |
| `9180` | 4 | Windows wake-response check; separate from the full information query |
| `9080` | 6 | Acknowledge/discard a pending capture |
| `9080` | 5120 | Read an 8-bit image |
| `f680` | 4 | Begin normal operation; startup waits 100 ms afterward |

Windows wake helper `0x38E90` sends `ff00`, waits 10 ms, then uses `0x38DEC`
to read four bytes from `9180`. If all four bytes are equal and nonzero, it
repeats the sequence, up to **three attempts total** (counter starts at zero;
`0x38F7C–0x38F81` increments and compares with three). All-zero bytes do not
meet this particular retry condition; that does not make them a valid identity.
Linux initialization and both capture wake points now use this bounded
four-byte check, then require a valid 32-byte information response containing
the established ID and geometry. Three unsuccessful wake checks fail explicitly;
transfer errors and cancellation do not cause unbounded retry. All-zero check
bytes alone never authorize initialization or image interpretation.
The independent wake-response predicate is tested over every nonzero repeated
byte and unequal-byte positions; state-machine tests cover retry success,
exhaustion, cancellation and transfer failure.
See the [lifecycle coverage record](windows-lifecycle-coverage.md).

The information payload contains big-endian chip ID `9368` at offsets 19–20,
application version at 21, manufacturer at 22, width at 23 and height at 24.
The verified geometry is **64 columns by 80 rows**. Constructor `0x35a94`,
sensor-size export `0x2a8f4` and image metadata conversion `0x2d4a0` establish
the width/height order; it must not be inferred by reading the packed constructor
word as a width/height pair.

Both information bytes 1 and 2 must be `11` for the ordinary finger-present
path. Byte 2 equal to `22`, initial bytes `ff ff`, or an incorrect ID indicate
an invalid application state (`0x38670`). Linux also requires the established
geometry before interpreting a frame. Version mismatch alone is acceptable for
ordinary capture. The normal Windows constructor expects version `13`; that
does not prove another responsive version is incompatible.

`0x384f0` reads exactly width × height one-byte pixels. The Linux backend copies
them unchanged: neither the legacy two-byte frame layout nor legacy inversion
applies. It wakes and acknowledges stale capture data, validates finger status,
waits for an interrupt when needed, revalidates status, then wakes and reads the
image. The Windows finger check can gate on GPIO before reading information;
actual IRQ timing still needs hardware validation.

Cleanup reads information twice, acknowledges capture data and reads information
again. Linux declares the session reusable only when every cleanup operation
succeeds and final identity/geometry are valid. This is a responsive application
with acknowledged capture data, not the legacy `a5 5a` idle state and not a claim
that the device is powered off. Cleanup is bounded and ignores cancellation so
the original failure can be reported after cleanup.

Every complete information response is checked for a positive conflicting ID
before another protocol command, including the two preliminary cleanup reads.
Such a response invalidates the session and forbids further cleanup writes.
Blank `0000`/`ffff` identity bytes and temporary health errors are not treated as
proof of a different chip; they retain bounded best-effort cleanup and still
cannot authorize capture or persistent programming.

## Firmware boundary

FT9368 recovery is **persistent flash programming**, unlike the FT9361 RAM
application upload. Normal initialization never selects an update because of
an application version mismatch. Programming is available only when
`FTE3600_FT9368_UPDATE=1` is explicitly set, the current session positively
identified chip `9368`, and both externally supplied files match their audited
size and SHA-256. Remove that setting after an intended update; keeping it set
requests the update again when opening the device.

| Relative path under the firmware directory | Bytes | SHA-256 |
| --- | ---: | --- |
| `fte3600/ft9368-app.bin` | 27120 | `9997aafac8eb9a1aecc7ba5b41212004fe1b325e7bdd1ea9ca08ae54b803e2e0` |
| `fte3600/ft9368-pramboot.bin` | 6096 | `c93a807eaaa34d9e79fbab77b86e910a419fd633ea98b628b72c64b285ffd191` |

The default directory is `/usr/lib/firmware`; `FTE3600_FIRMWARE_DIR` can select
another directory. Firmware payloads must be supplied separately; they are not
included in this repository. Hash validation checks compatibility with these
observations, not the provenance or license of a locally supplied file.

The Windows top-level fallback (`0x24037`) attempts this updater after failed
identification. Linux deliberately requires positive identification instead.
A completely blank or unresponsive chip with no positive identification remains
unsupported. Failure during erase/programming, power loss or removal can leave
the application unusable; implementing the updater does not establish a blank
chip recovery route.

## PRAM transport and verification

The observed PRAM loader is `0x25328`; low-level helpers are `0x25cdc`,
`0x25d70`, `0x25bb0` and `0x258c0`.

1. Hardware reset follows the established controller-level high 10 ms, low
   20 ms, high sequence. Linux expresses that as active-low logical 0, 1, 0.
2. Send mode packet `70 55 aa`.
3. SFR writes use `70 07 f8 address_BE16 00 00 value_BE16 00 00`.
   Write SFR `0004=aa55`, then `0048=0110`.
4. PRAM write packets are `70 05 fa address_BE16 count_minus_one_BE16 data`.
   The address is a word address, `2000 + byte_offset / 4`. Use up to 32 words
   per packet and a correctly bounded final packet. Lengths are multiples of
   four bytes. The independent implementation never reads beyond the file.
5. Readback first selects a region with `70 04 fb address_BE16
   count_minus_one_BE16`, waits, then clocks `71` followed by the read bytes.
   Data starts at receive offset one. Read at most 64 words per region and
   compare every byte against the validated PRAM image.
6. Write SFR `0007=5a5a`, send mode `70 0a f5`, and wait 10 ms.

## Application flash transport

The updater is observed at `0x266b4`, with SPI application helpers at `0x26590`
and the surrounding SPI0 wrappers. Each SPI0 transaction first wakes the
device. Linux rounds the short command separation upward to 1 ms instead of
using a busy wait. Writes use `opcode 00 length_BE16 00 00 00 payload`.

1. Send `5500` with zero length, wait 10 ms, then read `9080` length two.
   Require PRAM boot identity `56a2` before allowing an erase.
2. Write command `09` payload `0a`, then `10` payload `0c`.
3. Write `61` payload `00` to erase. Poll `6a80` length two with bounded
   10 ms delays; require acknowledgment `f0aa`.
4. For each application chunk, write `ab` with its 24-bit big-endian byte
   offset, then `bf` with at most 256 application bytes. Poll `6a80` for the
   program acknowledgment. The observed acknowledgment is
   `1000 + byte_offset / this_packet_length`. The final 240-byte packet at
   offset 26880 therefore acknowledges **`1070`**, not the value inferred by
   treating every packet as a 256-byte page.
5. Send zero-length `6400`, then write `65` with six bytes containing the
   24-bit start offset zero and 24-bit application length `0069f0`.
   Wait 5 ms, read `6680` length two, and compare the big-endian result against
   the independently calculated checksum.
6. Send zero-length `0700`, perform hardware reset, wait 400 ms, wake, wait
   5 ms and read information again. Require ID `9368`, geometry 64 × 80 and
   expected programmed version `13` before declaring update completion.

Checksum calculation starts with a 16-bit zero value. For each big-endian
16-bit application word, XOR that word into the value, then repeat 16 times:
right-shift and XOR polynomial `8408` when the preceding low bit was one.
This is the observed word-oriented calculation, not a substituted byte CRC.

Before erase, cancellation stops the update and performs reset/verification
cleanup. Once erase starts, the bounded programming and verification sequence
does not observe cancellation; the enclosing operation reports a pending
cancellation afterward. Transport errors still abort programming and retain
the original error while reset cleanup runs. Erase, program acknowledgment and
readback/checksum checks are mandatory; successful SPI submission alone is not
evidence that flashing succeeded.

## Validation limits

The protocol tests cover exact framing, partial PRAM/program packets, parsed
identity, geometry, bounds and an independently calculated checksum fixture.
The backend tests run real state machines and transfer objects with simulated
SPI, GPIO and firmware-loading boundaries. They cover repeated warm capture,
cancellation, cleanup errors, opt-in/identity gates, full generated-image PRAM
readback and flash programming, wrong boot identity, bounded acknowledgment
timeouts, checksum mismatch and post-update identity/version checks.

These tests do not exercise the kernel controller, voltage levels, suspend,
power interruption or physical FT9368 firmware programming. No firmware update
has been executed on a real device during this implementation task.
