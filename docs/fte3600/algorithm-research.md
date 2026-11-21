# Algorithm research branch

`research/algorithms` preserves the BRISK and IPA matcher cores and their
standalone mathematical tests for experiments and comparisons. `main` contains
the BRISK driver, without IPA sources, tests or build options. Both branches
include the FT9361 fast-open path.

The driver on this branch still authenticates with BRISK. The IPA core is not
connected to enrollment, persisted templates or authentication; the old dual
fusion implementation has not been ported. The presence of the core does not
enable dual-matcher verification.

With a C compiler, pkg-config and GLib development files installed, run:

```sh
./scripts/check-fte3600-algorithms.sh
```

The script builds and runs both standalone core suites using synthetic images
and features. It does not access the sensor or install software. Driver and
public API checks remain available through `./scripts/check-fte3600.sh`.
The Fedora and Ubuntu CI jobs also run both standalone core suites.

Keep matcher experiments on this branch. Record the exact revision, profile,
thresholds and test inputs for comparisons. Changes to template format or
authentication policy need explicit versioning and regression coverage before
driver integration. Synthetic checks do not establish population false
acceptance or false rejection rates.
