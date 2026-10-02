# Local validation — 2026-10-02

These checks validate software mechanics, not the Medion sensor. No kernel
module was loaded and no fingerprint device was accessed on the development
machine.

## Completed checks

- **30 Python unittest methods passed**, including parameterized cases for
  each setup failure/interruption and each unsafe restoration failure.
- Compiled the actual six extracted reference function bodies against mock
  GPIO/SPI/time interfaces with `-Wall -Wextra -Werror` and UBSan. Checked
  absent/active-high/active-low descriptors, both reset phases, lookup ordering,
  probe-defer propagation, power helpers and the SPI mode/speed cap.
- Compiled and executed the adapter's actual read/write function bodies against
  mocked kernel interfaces. Checked 5-byte framing, TX/RX payload shape, cache
  operations, length rejection, copy failures, removed-device errors and SPI
  error propagation. These mocks do not model controller electrical behavior.
- Compiled the real open/close client against a mock libfprint API. Checked
  success, missing library/symbol, failed context, no/multiple devices, open
  failure and close failure; vendor stdout/stderr is suppressed. The mock also
  verifies the client disables core dumps before loading it.
- Exercised the supervisor with fake host actions: partial setup rollback,
  interruption handling, ownership of runtime masks, continuing power rollback
  after one failed path, refusal to restart authentication after unsafe restore,
  bounded event output and actual child-process timeout termination.
- Checked staging refuses changed sources, changed binaries, symlinks, wrong
  kernel metadata and unrecognized package data. Only validated copies enter
  the private execution directory.
- Full client and kernel-module build succeeded using Ubuntu 24.04 userspace,
  GCC 13.3.0, GLib 2.80.0 and **6.8.0-142-generic headers**. Headers/tools were
  extracted locally for compilation, not installed or loaded. The compiler-name
  warning compares `gcc-13` with `x86_64-linux-gnu-gcc-13`; their version strings
  are the same. BTF generation is skipped without a matching vmlinux image.
- The fixed archived library passed **real bubblewrap ABI-only loading** with
  no sensor or `/sys` exposed. This initially found missing NSS in the local
  test environment; unpacked Ubuntu NSS/NSPR dependencies resolved it without
  replacing system libfprint. This verifies loading and the five API symbols,
  not enumeration or initialization on real hardware.
- Independently downloaded the pinned ctfdavis raw source and compared its
  hash with the committed reference. The older analysis copy's extra trailing
  newline is documented rather than being presented as an identical file.

Reproduction commands are in the README. The GitHub workflow runs unit tests
and a compile-only build; it never loads a module or downloads/runs the vendor
library. A workflow definition is not itself evidence that a remote run passed.

## Not established

- Compilation/loading on tuxman2's exact Fedora kernel, module-signing policy,
  SELinux policy, successful GPIO lookup, actual voltage/waveform or IRQ behavior.
- Real-machine recovery after every failure, hang, forced kill or power loss.
  Mocks cannot validate kernel/controller teardown or guarantee sensor reset.
- Full archived-library opening on Medion, successful firmware/calibration,
  image capture, enrollment, matching or login reliability.
- Identity of every artifact used in the successful historical Mint install.
- The root cause of prior zeros: power, reset and chip-select remain hypotheses
  until new evidence distinguishes them. A successful PCI runtime resume alone
  does not demonstrate sensor power.

The next useful evidence is **one run** of this baseline on the reported Medion,
with the source commit and bounded output. Do not begin another polarity/pin/
firmware matrix before reviewing that result.
