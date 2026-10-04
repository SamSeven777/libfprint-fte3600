# FT9338 and FT9536: bounded ROM identification and RAM recovery

The FT9338/FT9536 family shares a verified RAM-download procedure, distinct from
the FT9361/FT9348 procedure. This description records independent protocol facts
from the 2.0.3.102 binary identified in `windows-hardware-inventory.json`.
It contains no vendor code or firmware payloads. No physical device has been
used to validate this implementation.

## Identity boundary

The Windows classifier `0x2737c` treats an unrecognized boot-B chip as a reason
to try the FT9338 OTP path; boot-A classification `0x27414` defaults every value
other than `02` to FT9338. Neither default is a positive identity test.

Linux accepts two explicit cases:

* A current boot probe `90 00 00` returned `ef` at receive offset two. After
  the boot-A register preparation, register `fe` returns `02`, positively
  identifying FT9536. Other values remain unsupported.
* This open session first positively observed runtime geometry `5858` (FT9338)
  or `4080` (FT9536). After a controlled reset, boot-B38 OTP has upper nibble
  `1` or `2`, respectively, and agrees with that runtime identity. The original
  runtime pair is retained in the ROM evidence. An unknown A8-family response
  or a cached identity from a previous open is insufficient context.

Consequently, FT9338 with no responsive application and no current-session
runtime identity still lacks a supported first cold-identification route.
The implementation does not force an unknown chip into an FT9338 backend.
FT9536 has the positive boot-A route and the contextual boot-B38 route.

The identification SSM first applies controller-level H10/L20/H, then immediately
sends `55 aa`. Boot-A reads `cb`, writes it OR `20`, writes `fd=11`, writes
`fe=11`, then reads `fe`. Boot-B38 reads `c8`, writes it OR `01`, writes
`f1=1d`, reads `f4`, writes it OR `01`, reads `f3`, then writes `f4=00`.
Cleanup applies H10/L20/H and waits the known application startup interval
(80 ms for FT9338, 180 ms for FT9536; 180 ms if unknown) before returning.
Only successful cleanup commits the identity. Cancellation cannot shorten an
already started reset pulse, and failures retain the first error.

The relevant observations are `0x28114` (boot marker), `0x27414` (boot-A
register preparation), `0x27650` (B38 OTP), `0x2ead0` (entry reset and sync),
and `0x22c78`/`0x2f1b0`/`0x2fc30` (command conversion and register transfers).

## The no-dummy boot-register variant

Vendor command selectors `68` and `69` use **four-byte** one-byte register
transactions. Read is `08 f7 register 00`, with result at offset three.
Write is `09 f6 register value`. The Windows converter has zero extra dummy
bytes for these selectors. The A8 `65`/`66` variants and ordinary application
register access use a different turnaround layout. Reusing a five-byte
register helper here would move the response offset and alter the wire protocol.

## Upload and complete readback

`0x365c0` implements this family's RAM download. Linux requires matching
current-session ROM evidence, a matching firmware catalog entry, and a complete
transfer size before changing hardware. The external loader validates the
catalog size and SHA-256 before returning payload bytes.

| Sensor | Firmware path under `/usr/lib/firmware` | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| FT9338 | `fte3600/ft9338.bin` | 14184 | `ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46` |
| FT9536 | `fte3600/ft9536.bin` | 11934 | `4a62b5d9a8a7b4620bec7843763b030632a5a37a54c4cb7ed2cd96d5c7451e79` |

The existing `FTE3600_FIRMWARE_PATH` override selects a file, without weakening
its identity, size or hash checks. No payload is included in this repository.

1. Apply H10/L20/H and immediately send `55 aa`.
2. Write `c8=ff`, `ca=ff`, `cb=ff`, `b9=bf`, `b9=ff` using the no-dummy boot
   layout, then wait 20 ms.
3. Upload `05 fa 00 00 payload_length_BE16 payload 00` as one transaction.
   Wait 2 ms.
4. Read RAM from address zero with `04 fb 00 00 total_frame_length_BE16`.
   The total frame length is firmware length plus eight, receive data begins
   at offset six, and two additional trailing bytes are not compared. Compare
   every firmware byte; comparing only a prefix is insufficient.
5. Apply two H10/L20/H reset pulses separated by an additional 10 ms high
   interval, then wait 80 ms for FT9338 or 180 ms for FT9536. Poll application
   MCU status at most 20 times, separated by 2 ms, for `a5 5a`. Require the
   matching application geometry before accepting recovery.

The memory helpers are `0x2fd80` → `0x2fab0` (write) and `0x2f330` →
`0x2f020` (read). `0x35ea4` compares the complete payload. Restart is
`0x36bd0`; constructors `0x3574c` and `0x35c44` supply the different startup
delays. This procedure writes volatile RAM, with no flash-erase command.

Both successful completion and failures pass through bounded reset and
application verification. Cleanup ignores cancellation. A previous transfer,
readback or cleanup error prevents a verified-idle result even if a later
geometry read succeeds. A failed cleanup leaves the enclosing driver to end
the session rather than continue SPI operations.

## Mock validation

The independent test fixture uses generated bytes and actual state machines
and transfer objects. It exercises positive and rejected identity paths,
exact no-dummy framing, complete readback including the final firmware byte,
transfer-size and firmware-load gates, cancellation during reset and upload,
SPI failure and a bounded non-idle application. Firmware loader hashing is
covered separately by the existing loader tests; the recovery fixture replaces
that filesystem boundary to avoid distributing vendor payloads.

Physical cold startup, GPIO timing, controller behavior and suspend/resume on
FT9338/FT9536 remain hardware validation work. Mock success does not establish
that a particular laptop has passed these paths.
