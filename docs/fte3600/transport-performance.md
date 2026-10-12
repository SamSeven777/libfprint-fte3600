# Linux transport validation measurements

Repeated Linux path resolution contributed measurable startup overhead on the
A1 FT9361. This update resolves paths during board open, then checks the saved
canonical path components and current alias targets by inode identity. It still
reads every transport metadata attribute before and after each operation,
including ready status and generation. Changed aliases, inserted symlinks,
replaced objects and removed resources terminate the session before further I/O.

Sensor commands, requested delays, calibration, image processing, firmware and
the BRISK matcher are unchanged. A separate small fix preserves the terminal
device-absent status through portable legacy SPI/GPIO callbacks instead of
collapsing it into an ordinary retryable error. It does not relax Linux's sticky
transport-failure policy or introduce new retries.

## A1 observations

Measurements on 2026-10-12 used the same x86_64 NixOS A1 FT9361 and native stable
libfprint 1.94.10 integration. The tool used public libfprint open/close calls;
open includes the normal initialization and calibration. It did not request
fingerprint acquisition, enrollment, matching or stored-print changes.

| Three-cycle median | Baseline | Updated | Change |
| --- | ---: | ---: | ---: |
| Open wall time | 3955.394 ms | 3488.181 ms | 11.81% lower |
| Open process CPU time | 1297.600 ms | 838.614 ms | 35.37% lower |

All opens and closes succeeded. Returning to the baseline afterward gave
3960.962 ms wall / 1297.652 ms CPU, consistent with the first baseline. A separate
25 ms cancellation request returned cancellation with a closed device in
27.681 ms on the updated runtime; three subsequent ordinary open/close cycles
succeeded. The baseline also passed the cancellation control.

A separate traced discovery/open/close run reduced `readlink` calls from
396583 to 392 and total syscalls from 552164 to 265024. Additional direct
inode checks replace those path walks. Counts for `openat`, `read`, `close`
and `ioctl` were identical between the two runs. Traced timing is not used in
the timing table because tracing changes execution overhead.

These are small repeated measurements on one device. They establish an
observed startup improvement and successful automatic operations, not faster
matching, biometric accuracy, support for other sensors or Windows parity.
Recognition was not repeated in this automatic test round. The native master
integration has separate software validation from the stable hardware build.

## Replay and regressions

Build the diagnostic tool against the intended libfprint development package:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  tools/fte3600-open-bench.c $(pkg-config --cflags --libs libfprint-2) \
  -o fte3600-open-bench
sudo ./fte3600-open-bench 3
sudo ./fte3600-open-bench 3 --cancel-open-ms=25
```

The sensor must be idle and the owning fprintd service stopped for this direct
libfprint probe; restore that service afterward. Select the intended library
explicitly when comparing builds. JSON records wall and process CPU time for
each operation. The optional cancellation check must observe a cancellation
error and a closed device before proceeding to ordinary cycles.

The board regressions cover all 16 metadata fields, missing status/generation,
all 11 resolved aliases, directory/device replacement, and forwarding symlinks
at canonical paths. They exercise mutations before and after SPI and check
that failed transfers do not change the caller buffer. Legacy recovery tests
distinguish terminal absence from ordinary errors across four legacy models
and both callback types. Probe tests exercise timer cancellation, completion
races and cleanup with real GLib timers and synthetic public-API responses.
Normal and ASan/UBSan checks passed in the independent source workspace.

Implementation provenance and source hashes are in
[`independent-source.json`](independent-source.json). The source was authored
from the reviewed independent interfaces and standard filesystem/libfprint
APIs; this optimization introduced no vendor inputs or matcher changes.
