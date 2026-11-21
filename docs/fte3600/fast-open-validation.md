# FT9361 fast-open validation, 2026-11-21

On the One-Netbook A1, upstream `579ac579` took 3.54–3.92 seconds to open
the reader. The previously installed `395425c` build took 0.36–0.37 seconds.
These are public libfprint synchronous device-open timings, measured over
three open/close cycles per build on the same machine.

The new Linux/libfprint warm-open path sends the existing legacy ping's two
idle commands, then reads MCU readiness, geometry and both firmware-version
bytes. Only an exact, live FT9361 application match qualifies. It still loads
the verified firmware for recovery and runs the ordinary initializer and
acquisition setup. Unknown or incompatible replies use full cold discovery;
I/O errors, stale device generations and cancellation terminate the action.
Explicit recovery of an existing failed session retains cold initialization.
The libfprint adapter also uses cold startup when reopening after an action
failure. No hardware handles or asserted readiness are retained across close.

Three measured opens with this path took 81.25, 73.91 and 73.68 milliseconds.
This measures opening a running sensor application. It does not establish
cold-boot speed, suspend/resume correctness, other-profile hardware support,
or end-to-end fingerprint verification latency.

Regression checks cover successful warm startup followed by image capture,
each failed readiness/geometry/version predicate falling back to the existing
cold transcript, other supported profiles retaining cold startup, mandatory
and correctly bound firmware, fatal I/O, stale generation, cancellation and
explicit cold recovery. Existing Linux board/runtime, authentication,
open/close and probe suites were also run.

Implementation inputs: the current public device-session, legacy ping,
geometry, version, boot initializer and Linux board/runtime sources and their
synthetic tests. Public `395425c` code was consulted to compare lifecycle
behavior; the new path composes current implementation components. No vendor
executables, firmware contents, disassembly or captured fingerprints were read.
The existing firmware was supplied unchanged to the verified loader.
