# SPDX-License-Identifier: LGPL-2.1-or-later
if 'fte3600' in drivers and not get_option('fte3600-test-backend')
  executable('fte3600-tool', 'fte3600-tool.c',
    dependencies: libfprint_dep,
    include_directories: include_directories('../libfprint',
      '../libfprint/drivers/fte3600/include', '../libfprint/drivers/fte3600'),
    link_with: fte3600_driver,
    c_args: ['-include', 'fte3600-build-config.h'],
    install: false)
endif
