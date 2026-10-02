# Medion E3224: archived-stack initialization baseline

This branch is a **diagnostic reproduction**, not a working login driver or an
upstream libfprint submission. It tests whether an archived FocalTech libfprint
library can initialize the same Medion on which tuxman2 reported Mint success.
There is no Medion hardware result for this tool yet.

Instead of substituting A1 initialization commands, it calls the archived
library's public **open and close** functions. A small kernel adapter supplies
the old `/dev/focal_moh_spi` interface, using the archived ctfdavis GPIO and SPI
setup function bodies. The adapter is modified code, not the untouched original
module. See [provenance and differences](docs/provenance.md).

## What this test does

- Accepts only the reported `MEDION / E3224 / FT / YS13G` profile and
  `spi-FTE3600:00` below PCI `0000:00:19.0`.
- Uses Mode 0, 8 bits, the old 1 MHz speed cap, named optional GPIO lookup, and
  **both** old reset phases. It neither guesses Pin 39 nor adds an A1 reset path.
- Temporarily pauses fprintd, holds the SPI parent devices awake, and inhibits
  ordinary system sleep. It replaces only this sensor's binding for the test.
- Runs the old library without network, home-directory access, system write
  access, or access to other real `/dev` devices. No enrollment/matching API is
  called. Do not touch the sensor during the test.
- Reports GPIO mappings/logical values, the first 64 SPI transfer lengths/status,
  and only the short C6/identity replies. Other vendor output is suppressed.
- Attempts to restore binding, SPI settings, power policies and fprintd on
  success, failure, timeout and ordinary interruption. No system library,
  firmware file, udev rule, PAM setting or persistent module configuration is
  installed by the tool.

**This is not a read-only probe.** It executes archived vendor code and performs
GPIO/register writes and vendor initialization/calibration. The sandbox does
not make sensor operations risk-free. Sensor-internal state and prior physical
GPIO state cannot be guaranteed restored. Save your work and retain password
login. Do not run it on a machine you cannot restart. Do not suspend, close the
lid, run another fingerprint test, or force-unload modules while it runs.

## Test on Fedora

Use a fresh directory, so existing development changes stay untouched:

```sh
git clone --single-branch --branch medion-e3224 https://github.com/SamSeven777/libfprint-fte3600.git medion-baseline
cd medion-baseline
git rev-parse HEAD
```

Install build/runtime prerequisites from Fedora's repositories, if needed:

```sh
sudo dnf install gcc make pkgconf-pkg-config glib2-devel binutils zstd python3 kmod bubblewrap libgusb libgudev pixman nss "kernel-devel-$(uname -r)"
```

The [kernel-devel package](https://packages.fedoraproject.org/pkgs/kernel/kernel-devel/)
must match the running kernel. If that exact package is unavailable, stop and
report the error; do not guess another header version. The test refuses kernel
lockdown/unsigned-module restrictions; **do not disable Secure Boot or SELinux**
to make it run. Report such a refusal instead.

Build as your regular user, then run one test:

```sh
python3 scripts/prepare.py && sudo python3 scripts/run-baseline.py --run
```

`prepare.py` downloads a fixed, hash-checked `.deb` and extracts only its library
into `.baseline/build`. It does **not** install the package or run its scripts.
An existing copy can be supplied with `--deb /path/to/reference.deb`; it must have
the same hash. Build paths must not contain whitespace. After a source or kernel
update, run prepare again. The default library timeout is 120 seconds; do not
increase it or retry repeatedly without reviewing the first result.

Paste the commit ID and complete terminal output. No serial number, fingerprint
image, enrolled print, or full system journal is requested. We already have the
earlier kernel report; this build records/checks its own kernel version.

## Interpreting the result

- `ABI_*`/`MISSING_*` failure: dependency/interface/isolation setup failed before
  hardware changes. This says nothing about sensor power.
- `LOOKUP`/`GPIO_MAP`/`GPIO`: what Linux actually supplied and what logical writes
  were requested. `requested_raw` is the polarity conversion, **not an electrical
  measurement**. An absent descriptor means no write; there is no pin fallback.
- `C6`/`IDENTITY`: actual short replies from this run. All-zero data alone does
  not distinguish power, reset, chip select or communication failures.
- `OPEN_OK`, `CLOSE_OK`, and the final restoration-success message: the archived
  library completed opening and closing in this test. **Enrollment remains
  untested**; it is not permission to replace your installed libfprint.
- `RESTORE INCOMPLETE`: stop testing, continue using password login and reboot
  before further fingerprint use. The tool deliberately does not restart
  authentication after unsafe binding/power restoration. Never force `rmmod`.

The root-private `/run/medion-baseline-*` directory printed by the tool retains
the pre-test restoration record and copied artifacts until reboot. SIGKILL,
power loss and a stuck kernel cannot be made recoverable by a Python `finally`
block. Reboot clears the temporary service mask and module binding; a full
shutdown may still be needed to reset the sensor itself.

## Local checks (no hardware)

```sh
python3 -m unittest discover -s tests -p test_baseline.py -v
python3 scripts/prepare.py --compile-only
```

Unit tests compile the actual extracted reset/lookup/SPI bodies and actual
adapter transfer bodies, exercise a mock library, and inject supervisor failures
and interruptions. Compile-only downloads/runs no vendor code and creates no
runnable manifest. `--kernel-build /path/to/headers` is available for build CI,
not as a way to load a module into a different kernel.

Optional, after a normal prepare: `python3 tests/check-isolation.py` loads the
archived library inside the real sandbox to check symbols, without exposing a
sensor or `/sys`. This executes the library loader/constructors, not enrollment
or hardware initialization. Local verification and its limits are recorded in
[validation](docs/validation.md).

## License and scope

The reference/module and C hardware harnesses are GPL-2.0-only
([COPYING.GPL-2](COPYING.GPL-2)); the new userspace client, Python tools/tests and
workflow are LGPL-2.1-or-later ([COPYING](COPYING)). No vendor binary or firmware
is committed or redistributed in this branch. The archive's presence and hashes
do not establish a license to redistribute its contents or an upstream-ready
implementation. Keep this diagnostic baseline separate from any future
independent libfprint driver.
