# Medion FT9338 startup correction

Reference: `ftWbioUmdfDriverV2.dll` 2.0.3.102, SHA-256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.
The audit re-decoded 15 relevant functions (1,928 instructions) directly from
that binary and checked the recorded disassembly against them. No vendor host
code or matching library is included in this implementation.

The earlier test matched individual ROM/download helpers but omitted their
calling context. The corrected flow shares one FT9338 engine between the
automatic `--test-ft9338` action and explicitly selected `--boot ft9338`.

| Phase | Windows evidence (RVA) | Corrected behavior |
| --- | --- | --- |
| First two factory rounds | `23faf`–`24244`, `26d9c`, `fba0`/`10f0c`, `10a74`/`10d70`, `fd94` | FT9368 queries, special-family wake and both C6 passes, ID query, hardware-reset/10 ms retry |
| Legacy application wake | `28344`, `270e4`, `28c54` | Up to six paired `70` attempts with immediate status reads; 350 ms only after idle, before geometry |
| ROM selection | `27144`, `2737c`, `27414`, `27650`, `2819c` | Preserve boot-A/B38 classification and defaults; neither `1534` nor `1574` alone selects FT9338 |
| Factory retry | `24008`–`240d6`, `24244`–`24299` | Round 2 includes reset + 300 ms and three FT9368 queries before special-family/ROM retry; no repeated legacy wake loop |
| Selected FT9338 initialization | `2c754`, `28668`, `2c8bc` | Return idle and check FW/AGC on cold and warm paths; reuse matching firmware or download on mismatch/transient version-read error |
| Embedded RAM download | `365c0`, `2c4a4`, `36bd0` | H10/L20/H immediately followed by `55 aa`, boot-register setup, complete upload/readback, two pulses, 80 ms, bounded MCU polling; five attempts per selection |
| MCU configuration | `36a30` | `01=01`, `41=0f`, `30=bb`, 1 ms waits and marker read; non-BB reply warns |
| Sensor-mode entry | `287d4(1)`, `285cc` | Query MCU; return idle if busy; `76=01`, `1f=01`, `1e=01`, 10 ms, finger/MCU reads; write `76=00` and warn if still idle |

The mode argument is saved at the entry stack pointer plus `0x10` (`287e8`).
After three pushes and a `0x40` allocation, this is `[rsp+0x68]`, passed to
the mode writer at `28833`. The separate zeroed `[rsp+0x70]` slot is the
finger-status output, not the initial mode. Tests distinguish the initial
`76=01` from the still-idle fallback `76=00`.

The manual action skips ROM/OTP selection only. It still runs preparation,
return-idle, version checking, configuration and sensor-mode entry. Positive
application evidence of a different chip stops either action. The duplicate
manual FT9338 download implementation was removed; FT9348 keeps its existing
separate experiment.

The diagnostic retains explicit host boundaries: device/session loss and
cancellation are terminal; uncertain OTP state is cleaned up before exit;
recognized other chips are not reflashed. Windows rounds 3/4 force an FT9368
download and are outside this FT9338-only tool. At most two selected FT9338
initializations and ten uploads occur. Unmapped OTP may reach the factory
retry but never authorizes an upload. The Linux action combines selection and
one successful `LoadFW` sequence rather than duplicating UMDF event callbacks.
Unlike the Windows caller, it reports sensor-mode I/O failures instead of
discarding them, without turning them into another download. During application
discovery, I/O errors leave the current probe through the bounded factory retry
or a terminal host error; the diagnostic does not copy vendor helpers that
discard transport errors and interpret failed reads as zero.

Final geometry/version assertions remain diagnostic checks, performed after
initialization. Full RAM comparison now reports the first differing offset,
expected/received byte, mismatch count and all-zero/all-FF classification;
these are firmware diagnostics, not fingerprint data. Reset release and
`55 aa` remain adjacent in the native I/O callback, with validation beforehand.

Regression coverage includes literal packet/order traces, OTP recovery in
round 2, C6 exhaustion followed by ID reading, wrong-family stops, version-read
fallback and short-circuiting, MCU busy/idle sensor-mode branches, no redownload
after a sensor-mode error, full readback and retry limits, cancellation,
cleanup, and the public manual-boot dispatcher using the shared engine.
The former manual FT9338 tests are consolidated into the shared-engine suite;
the FT9348 suite retains its exact trace and transfer-failure sweep.

Validation passed with compiler warnings treated as errors: 71 FT9338 cases
and all 10 selected Medion transport/launcher/identification/boot and protocol
suites, in both authentication-disabled and authentication-enabled builds.
All five changed C files pass the repository formatter; `git diff --check`
reports no whitespace errors. Negative controls that separately omit
return-idle, sensor-mode entry or factory round 2 each fail the corresponding
regression test. These control builds were made outside the repository and
did not change production sources.

Software checks cannot establish that these omissions were the sole cause of
Tuxman2's failure or that the corrected version boots his physical E3224.
Physical startup, GPIO/SPI timing and image capture still require a device run.
