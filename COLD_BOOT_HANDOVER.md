# FT9361 cold-boot recovery

## Result

On the verified One-Netbook A1, a standalone recovery changed MCU status from
`00 00` to `a5 5a`. The corrected libfprint driver passed repeated open/close
checks. On 2026-09-12 the owner then powered the machine off and booted directly
into Linux; device initialization and actual fingerprint authentication worked.

The normal journal did not show whether that particular boot needed an upload.
These observations establish a working recovery sequence and a successful
cold-boot test on one A1; they do not prove that the chip has no flash, or that
every all-zero SPI response has the same cause.

## Correct initialization sequence

The matching Windows driver's FT9361 function table selects the FT95a8Base
download path. The earlier handover incorrectly selected FT9338Base and its
five `09 f6` unlock-register writes. Those writes are absent from the FT9361
path and are not used by this driver.

The independently implemented FT9361 recovery performs:

1. Assert reset (GPIO offset 85 low) for 5 ms, then release it high.
2. Send the two-byte bootloader synchronization command `55 aa`.
3. Send one 10,403-byte SPI transaction:
   `05 fa 00 00 28 9c`, 10,396 firmware bytes, and one trailing zero.
4. Wait 2 ms, pulse reset low for 5 ms, wait 10 ms, and repeat the reset pulse.
5. Wait 160 ms, send `70`, wait 5 ms, send `70`, then wait 2 ms.
6. Poll MCU status register `20` up to 20 times, 2 ms apart, for `a5 5a`;
   only then proceed to sensor identity, firmware version, and application
   configuration checks.

The driver first attempts ordinary soft/hardware-reset recovery. If that
fails, it attempts one firmware upload per open. GPIO routing remains limited
to the exact verified A1 profile. The firmware transaction is never split
across chip-select assertions and its payload is redacted from debug logs.

## Firmware and packaging

The owner supplies `/usr/lib/firmware/fte3600/ft9361.bin`. An explicit
`FTE3600_FIRMWARE_PATH` override is available for local diagnostics.
Only the following firmware is accepted:

- Length: 10,396 bytes.
- SHA256: `027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.

Firmware and vendor DLLs are not included in the repository or distribution
package. Install the owner's firmware separately as described in
[the installation guide](docs/fte3600/install.md).
The packaged spidev configuration sets `bufsiz=32768`; at least 10,403 bytes
are needed for recovery.

The runtime-PM helper addresses the separately observed SPI-controller suspend
problem. Its successful systemd status alone does not demonstrate a working
sensor and it cannot replace firmware recovery.

## Verification

Run the repository checks for both authentication policies:

```sh
./scripts/check-fte3600.sh
```

The driver suite checks missing, empty, truncated, oversized, wrong-hash, and
non-regular firmware files using generated fixtures. To additionally validate
the owner's local firmware without including it in test data:

```sh
FTE3600_TEST_FIRMWARE=/usr/lib/firmware/fte3600/ft9361.bin \
  meson test -C build-fte3600-ci-true --print-errorlogs fte3600-driver
```

For a real-device check without capturing images or accessing templates,
compile the open/close diagnostic against the installed library:

```sh
cc -Wall -Wextra -Werror -o tools/test_ft9361_open \
  tools/test_ft9361_open.c $(pkg-config --cflags --libs libfprint-2)
sudo ./tools/test_ft9361_open
```

Use this diagnostic while fprintd has released the device. Normal fprintd
initialization can also be checked through its D-Bus Claim operation.

Cold-boot acceptance requires shutting down, powering on directly into Linux,
and checking actual fingerprint authentication. A warm reboot after Windows
initialization is not sufficient evidence.

Obsolete raw SPI experiments were removed from the maintained source after
the corrected path was validated. They are not supported recovery tools.
