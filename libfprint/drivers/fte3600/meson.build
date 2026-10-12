# SPDX-License-Identifier: LGPL-2.1-or-later
# Keep the feature defines and numeric policy confined to this driver target.
fte3600_sources = files(
    'fte3600.c',
    'device_session.c',
    'factory_integration.c',
    'factory_byte_startup.c',
    'factory_legacy_startup.c',
    'factory_startup.c',
    'ft9368_boot.c',
    'ft9368_lifecycle.c',
    'ft9368.c',
    'ft93xx.c',
    'fw9369_startup.c',
    'fw9369_fdt.c',
    'fw9369_grayscale.c',
    'fw9369_release.c',
    'ft93xx_image_pipeline.c',
    'capture.c',
    'calibration.c',
    'image.c',
    'discovery.c',
    'identity.c',
    'legacy_boot.c',
    'legacy_lifecycle.c',
    'legacy.c',
    'recovery.c',
    'linux_firmware.c',
    'auth.c',
    'auth_profiles.c',
    'sam7-brisk/libfprint/matchers/brisk/brisk.c',
    'sam7-brisk/libfprint/drivers/fte3600-brisk.c',
)

if get_option('fte3600-test-backend')
    fte3600_sources += files('libfprint_backend_fake.c')
else
    fte3600_sources += files('fte3600-ids.c', 'linux_board.c', 'linux_runtime.c')
endif

fte3600_config = configuration_data()
fte3600_config.set_quoted('FTE3600_FIRMWARE_DIR', get_option('fte3600-firmware-dir'))
fte3600_config_header = configure_file(output: 'fte3600-build-config.h',
    configuration: fte3600_config)

fte3600_driver = static_library('fprint-fte3600',
    sources: [fte3600_sources, fte3600_config_header],
    dependencies: deps,
    include_directories: include_directories('include', '.', '../..',
        'sam7-brisk/libfprint/drivers'),
    c_args: ['-include', 'fte3600-build-config.h',
             '-DFTE3600_ENABLE_PERSONAL_AUTH=1', '-DFTE3600_ENABLE_IPA_AUTH=0',
             '-fno-fast-math', '-ffp-contract=off',
             '-fexcess-precision=standard', '-frounding-math'],
    install: false,
)

fte3600_auth_test = executable('test-fte3600-auth', 'test_auth.c',
    dependencies: deps,
    include_directories: include_directories('include'),
    link_with: fte3600_driver,
    install: false)
test('fte3600-auth', fte3600_auth_test, timeout: 180,
    env: ['G_DEBUG=fatal-warnings'])
