
examples = [
    'enroll',
    'identify',
    'img-capture',
    'manage-prints',
    'verify',
    'clear-storage',
]

foreach example: examples
    executable(example,
        [ example + '.c', 'storage.c', 'utilities.c' ],
        dependencies: [
            libfprint_dep,
            glib_dep,
        ],
    )
endforeach

executable('cpp-test',
    'cpp-test.cpp',
    dependencies: libfprint_dep,
)

if installed_tests
    install_subdir('prints',
        install_dir: installed_tests_testdir)
endif

if get_option('fte3600_medion_spidev')
    if 'fte3600' not in drivers
        error('The Medion spidev experiment requires -Ddrivers=fte3600')
    endif
    medion_spidev = executable('fte3600-medion',
        sources: ['fte3600-medion.c', 'fte3600-medion-transport.c'],
        dependencies: libfprint_private_dep,
        link_with: libfprint_drivers,
        c_args: common_cflags + fte3600_personal_auth_cflags,
        install: false,
    )
    test('medion-spidev-help', medion_spidev, args: ['--help'])
endif
