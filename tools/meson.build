# SPDX-License-Identifier: LGPL-2.1-or-later
if 'fte3600' in drivers
  executable('fte3600-open-bench', 'fte3600-open-bench.c',
    dependencies: libfprint_dep,
    include_directories: include_directories('../libfprint'),
    install: false)
  if not get_option('fte3600-test-backend')
    fte3600_probe = executable('fte3600-probe', 'fte3600-probe.c',
      dependencies: deps,
      include_directories: include_directories('../libfprint/drivers/fte3600/include', '../libfprint/drivers/fte3600'),
      link_with: fte3600_driver,
      c_args: ['-include', 'fte3600-build-config.h'],
      # Use the same configured firmware directory as the driver.
      install: false)
  endif
endif
