# This target deliberately depends only on GLib and libm. Hardware drivers,
# template containers and authentication policy belong to its consumers.
ipa_core = static_library('fprint-ipa-core',
    sources: 'ipa.c',
    dependencies: [glib_dep, mathlib_dep],
    install: false,
)
ipa_core_dep = declare_dependency(
    link_with: ipa_core,
    include_directories: include_directories('.'),
    dependencies: [glib_dep, mathlib_dep],
)
