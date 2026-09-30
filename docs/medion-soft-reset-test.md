# Medion E3224: one controlled soft-reset comparison

This experiment asks one question: does the current FT9361-style status query
respond differently before and after two `0x70` commands? FT9361 is the working
hypothesis for this experiment, not a confirmed identity of the Medion sensor.
It is not another firmware-recovery attempt or an enrollment test.

The standalone diagnostic performs this sequence in one SPI and power-management
session, at Mode 0, 8 bits, MSB-first, 1 MHz:

1. **A:** read MCU status and geometry without first resetting the sensor.
2. Send `0x70`, wait 5 ms, send `0x70`, then wait 2 ms.
3. **B:** repeat the same status and geometry reads.

There are six SPI transfers in total. No GPIO is requested or changed; there is
no ROM query, firmware upload, image capture, or automatic recovery. Nevertheless,
this is an **active, state-changing experiment**: SPI queries are not passive,
and the two commands may change the sensor's state. `0x70` is not established as
a universal power-on handshake for an unidentified sensor.

The no-GPIO restriction applies to the standalone diagnostic itself. Stopping
or restoring fprintd may execute the installed driver's GPIO cleanup or startup
sequence; those service operations are outside the A/B comparison.

Keep the firmware assumption fixed: do not replace firmware or run an uploader
before or during this test. This mode does not read a firmware file and cannot
establish which firmware, if any, was already running after boot.

## Prepare the standalone test

Use a checkout containing this document and `scripts/test-medion-soft-reset.sh`.
There is no need to build or install libfprint, change authentication settings,
or rebuild the old `focal_spi` module. For a separate checkout:

```sh
git clone --branch medion-e3224 --single-branch https://github.com/SamSeven777/libfprint-fte3600.git libfprint-medion-soft-reset
cd libfprint-medion-soft-reset
bash scripts/test-medion-soft-reset.sh --check-only
```

If you already have this revision, use that checkout instead; do not discard
local changes. The wrapper prints the commit and whether the checkout is dirty.

`--check-only` checks dependencies and fixed system metadata, and compiles into
a temporary directory. It does not run the diagnostic, change services, or
access SPI/GPIO devices. Compilation does create temporary files. Dependencies
include a C compiler, `pkg-config`, GLib, GUdev and libgpiod development packages,
Git, systemd tools, and `flock`; use the reported missing dependency rather than
installing another fingerprint stack. A successful check is not a hardware test.

If there is no correctly bound FTE3600 spidev node, or the check fails, keep that
output and stop. Do not guess a device node, rebind other SPI devices, or repeat
the old recovery procedure as a prerequisite.

## Recommended: a controlled fresh-boot baseline

This is the preferred **single run** if you can prepare a new boot. First confirm
that you can log in and use `sudo` with your password without a fingerprint.
Save other work before shutting down. Do not proceed if you depend on fingerprint
authentication to regain access.

### 1. Record the service state before changing it

From the checkout, save this output in its parent directory so it remains
available after shutdown without making the source checkout dirty:

```sh
systemctl show fprintd.service -p LoadState -p ActiveState -p UnitFileState | tee ../medion-fprintd-before.txt
```

Keep this file for restoration; note whether `ActiveState=active` was present.

- If the service is loaded, not already masked, and its active state is either
  `active` or `inactive`, run the command below. Record that **you added this mask
  for this experiment**.
- If it is already persistently masked and inactive, leave it unchanged. Do not
  remove that pre-existing mask afterward.
- If it is `masked-runtime`, that mask will disappear at shutdown: stop this
  fresh-boot preparation and ask before changing the existing arrangement.
- If the service is active while masked, missing, or has another unexpected
  state, keep the output and ask before proceeding.

For a previously unmasked service only:

```sh
sudo systemctl mask --now fprintd.service
systemctl show fprintd.service -p LoadState -p ActiveState -p UnitFileState
```

Before shutdown, require `LoadState=masked`, `ActiveState=inactive`, and
`UnitFileState=masked` (not `masked-runtime`). If masking fails, stop; **do not use
`--force`, delete a local service file, or change its enable/disable state**.
For example, a custom unit under `/etc` can prevent masking. If this experiment
added a mask but preparation cannot continue, undo only that new mask using the
restoration instructions below.

A persistent mask is used here because merely stopping or disabling fprintd
does not prevent later activation, and a runtime mask does not survive reboot.
This does not change PAM configuration or replace authentication libraries.

### 2. Fully shut down, then turn the laptop on

```sh
sudo systemctl poweroff
```

Wait until shutdown has completed, then turn it on normally. Do not use restart,
suspend, or hibernate for this baseline. Do not change BIOS settings or disconnect
the battery. A normal shutdown does **not** prove that the internal sensor's
power rail was electrically off; this is a controlled fresh-boot software
baseline, not proof of a hardware cold start.

Log in with your password. Before running any fingerprint command or old tool,
return to the checkout and confirm the service is still masked and inactive:

```sh
systemctl show fprintd.service -p LoadState -p ActiveState -p UnitFileState
```

Do not run enrollment, verification, the earlier recovery script, or other
fingerprint software first. The mask isolates fprintd; it does not rule out
firmware activity during boot or an unrelated fingerprint program.

### 3. Run the comparison once and retain the complete output

Run the following in Bash. The pipe setting preserves a failed diagnostic status
instead of replacing it with the log writer's status:

```sh
set -o pipefail
sudo bash scripts/test-medion-soft-reset.sh --run 2>&1 | tee ../medion-soft-reset.log
printf 'Test/logging exit status: %s\n' "$?"
```

The wrapper builds a temporary standalone tool, verifies that fprintd is
isolated, and calls only `--compare-soft-reset`. It installs nothing and does not
upload the output anywhere. It shares an exclusive lock with the recovery
wrapper until the diagnostic has stopped and service restoration finishes.
Direct diagnostic invocations and other fingerprint clients bypass that lock;
do not run them alongside either wrapper.

The diagnostic checks the selected device's actual SPI hierarchy, including
PCI `0000:00:19.0` and its PXA SPI controller. It temporarily holds runtime power
management on, requires the relevant controllers to be active and PCI state D0,
and checks those conditions at the comparison boundaries. It stops on a failed
check rather than treating that run as a valid A/B comparison. **D0 and an active
SPI controller do not prove that the sensor itself has power.** Keep these lines
in the log; do not substitute a manual power or GPIO change.

The tool restores the SPI settings and runtime-power policies it changed. The
wrapper removes its temporary build and restores only service changes it made
for this invocation. An existing persistent mask from step 1 is deliberately
left in place: restore that separately next. Check for cleanup errors even when
some response bytes looked useful.

### 4. Restore the service arrangement you recorded

Do this after the test, including a failed test. **Only if you added the
persistent mask in step 1**, remove it:

```sh
sudo systemctl unmask fprintd.service
```

If the original saved state was `ActiveState=active`, also run:

```sh
sudo systemctl start fprintd.service
```

If it was originally inactive, do not start it just for restoration. If it was
already masked before this experiment, leave that original mask in place. Do not
enable or disable the service. Finally check:

```sh
systemctl show fprintd.service -p LoadState -p ActiveState -p UnitFileState
```

Keep any restoration error and report it. After unmasking, normal desktop or
authentication activity may activate fprintd again; that is outside the finished
comparison.

## Optional alternative: test the current boot

If a fresh boot is inconvenient, after `--check-only` you may run the same logged
`--run` command once in the current session. The wrapper temporarily stops and
masks fprintd, then restores its prior arrangement. Do not perform both workflows
as an automatic sequence of retries.

Label this result **current-boot state**, and say whether fprintd, enrollment,
verification, or an old diagnostic/recovery tool ran earlier in this boot. If
unknown, say unknown. Stopping fprintd now cannot undo earlier resets or uploads,
so this result is not a clean initial-state observation.

## Interpret the comparison, not just the exit status

Here “idle” means the expected MCU status bytes `a5 5a`, not successful enrollment
or verification. Geometry `40 50` is compatible with 64 by 80 dimensions; it is
**not a chip ID**.

| A, before the commands | B, after the commands | What this run supports |
| --- | --- | --- |
| Idle | Idle | The queried application was already reachable; the commands were not needed to obtain the first idle response. |
| No recognized idle | Idle | The commands were associated with reaching the expected state in this session. This does not prove the sensor model or a universal startup sequence. |
| Idle | No recognized idle | The expected response was lost after the commands. Stop; do not trigger firmware recovery automatically. |
| No recognized idle | No recognized idle | This query did not reach the expected state either way. Even all-zero replies do not prove missing power, a dead sensor, or a particular chip model. |
| An I/O, power, or cleanup error | Any partial response | Preserve the error; do not interpret this as a completed, controlled comparison. |

For a completed comparison, exit status `0` means B had the expected idle status;
`2` means B did not, including when A did. Errors return `1`; interruption is
reported separately. A wrapper cleanup failure can turn the result into an
error. Neither `0` nor `2` is an authentication result. Do not keep rerunning the
experiment to obtain a preferred response.

## What to send back for this first round

Send the complete new log, including the source revision, A and B bytes, power
checks, and cleanup results, plus these short answers:

- Was this a full shutdown followed by power-on, or the current boot? Was fprintd
  persistently masked before boot?
- Before the test in this boot, had any fingerprint command or old tool run?
  “Unknown” is an acceptable answer.
- Was the service arrangement restored afterward, or is anything still masked
  or reporting an error?

Do not send fingerprint images or templates. A full ACPI dump, Windows reinstall,
or another firmware upload is not required for this round. If the original Medion
2018 driver package is already at hand, its filename/version and an official
source link or file hash would be useful optional background; its absence does
not block this experiment.
