# This target deliberately depends only on GLib and libm. Hardware drivers,
# template containers and authentication policy belong to its consumers.
brisk_core = static_library('fprint-brisk-core',
    sources: 'brisk.c',
    dependencies: [glib_dep, mathlib_dep],
    install: false,
)
brisk_core_dep = declare_dependency(
    link_with: brisk_core,
    include_directories: include_directories('.'),
    dependencies: [glib_dep, mathlib_dep],
)
