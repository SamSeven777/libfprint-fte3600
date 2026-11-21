#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Standalone synthetic matcher checks; no hardware or system installation.
set -euo pipefail

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_work=$(mktemp -d "${TMPDIR:-/tmp}/fte3600-algorithms.XXXXXX")
trap 'rm -rf -- "$fte_work"' EXIT
read -r -a fte_glib_flags <<< "$(pkg-config --cflags --libs glib-2.0)"

for fte_matcher in brisk ipa; do
  "${CC:-cc}" -std=gnu99 -O2 -Wall -Wextra -Werror \
    "$fte_repo/tests/test-$fte_matcher-core.c" \
    "$fte_repo/libfprint/matchers/$fte_matcher/$fte_matcher.c" \
    "${fte_glib_flags[@]}" -lm -o "$fte_work/test-$fte_matcher-core"
  "$fte_work/test-$fte_matcher-core"
done
