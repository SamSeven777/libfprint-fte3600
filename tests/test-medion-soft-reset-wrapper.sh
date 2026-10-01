#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Black-box shell regression. No root, systemd, GPIO, SPI, or C compiler needed.
# Only a temporary script copy is redirected away from its two /run paths.
set -euo pipefail

fte_test_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_test_root=$(mktemp -d /tmp/fte3600-wrapper-regression.XXXXXX)
fte_test_child=
fte_test_cleanup() {
  local fte_test_status=$?
  trap - EXIT INT TERM
  if [[ -n $fte_test_child ]]; then
    kill -TERM "$fte_test_child" 2>/dev/null || true
    wait "$fte_test_child" 2>/dev/null || true
  fi
  rm -rf -- "$fte_test_root"
  exit "$fte_test_status"
}
trap fte_test_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fte_test_bash=$(command -v bash)
mkdir -p "$fte_test_root/bin"
cat > "$fte_test_root/bin/mock-command" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
case ${0##*/} in
  id) printf '%s\n' "${FTE_TEST_UID:-0}" ;;
  git)
    case " $* " in
      *' rev-parse '*) echo 0123456789012345678901234567890123456789 ;;
      *' status '*) [[ ${FTE_TEST_DIRTY:-0} != 1 ]] || echo ' M tools/test_medion_e3224.c' ;;
      *) exit 91 ;;
    esac
    ;;
  pkg-config)
    [[ ${FTE_TEST_PKG_FAIL:-0} != 1 ]] || exit 1
    case $1 in
      --modversion) echo 2.2.1 ;;
      --cflags) echo -DFTE_TEST_BUILD=1 ;;
      *) exit 91 ;;
    esac
    ;;
  cc)
    echo cc >> "$FTE_TEST_CASE/events"
    [[ " $* " == *'/libfprint/drivers/fte3600-legacy-proto.c '* ]] || exit 93
    [[ ${FTE_TEST_COMPILE_FAIL:-0} != 1 ]] || exit 1
    fte_mock_out=
    while (( $# )); do
      if [[ $1 == -o ]]; then
        shift
        fte_mock_out=$1
      fi
      shift
    done
    [[ -n $fte_mock_out ]]
    printf '%s\n' "$fte_mock_out" > "$FTE_TEST_CASE/compiled-path"
    cp "$FTE_TEST_TOOL" "$fte_mock_out"
    chmod +x "$fte_mock_out"
    ;;
  systemctl)
    printf 'systemctl %s\n' "$*" >> "$FTE_TEST_CASE/events"
    read -r fte_mock_load fte_mock_active < "$FTE_TEST_CASE/state"
    case $1 in
      show)
        if [[ -L $FTE_TEST_RUNTIME_UNIT && $(readlink "$FTE_TEST_RUNTIME_UNIT") == /dev/null && ${FTE_TEST_MASK_INEFFECTIVE:-0} != 1 ]]; then
          fte_mock_load=masked
        fi
        if [[ $* == *--value* ]]; then
          if [[ $* == *LoadState* ]]; then echo "$fte_mock_load"; else echo "$fte_mock_active"; fi
        else
          printf 'LoadState=%s\nActiveState=%s\n' "$fte_mock_load" "$fte_mock_active"
        fi
        ;;
      is-active) [[ $fte_mock_active == active ]] ;;
      mask)
        [[ $* == 'mask --runtime --now fprintd.service' ]]
        ln -s /dev/null "$FTE_TEST_RUNTIME_UNIT"
        printf '%s inactive\n' "$fte_mock_load" > "$FTE_TEST_CASE/state"
        [[ ${FTE_TEST_MASK_FAIL:-0} != 1 ]] || exit 1
        ;;
      unmask)
        [[ $* == 'unmask --runtime fprintd.service' ]]
        # A separately opened descriptor must remain excluded during service
        # restoration, even though the diagnostic child has already exited.
        if flock --nonblock "$FTE_TEST_CASE/lock" true; then
          echo 'diagnostic lock was released before service cleanup' >&2
          exit 92
        fi
        [[ ${FTE_TEST_UNMASK_FAIL:-0} != 1 ]] || exit 1
        rm -- "$FTE_TEST_RUNTIME_UNIT"
        ;;
      start)
        [[ $* == 'start fprintd.service' ]]
        [[ ${FTE_TEST_START_FAIL:-0} != 1 ]] || exit 1
        printf '%s active\n' "$fte_mock_load" > "$FTE_TEST_CASE/state"
        ;;
      *) exit 91 ;;
    esac
    ;;
  *) exit 91 ;;
esac
MOCK
chmod +x "$fte_test_root/bin/mock-command"
for fte_test_command in id git pkg-config cc systemctl; do
  ln -s mock-command "$fte_test_root/bin/$fte_test_command"
done

export FTE_TEST_TOOL="$fte_test_root/mock-diagnostic"
cat > "$FTE_TEST_TOOL" <<'TOOL'
#!/usr/bin/env bash
set -euo pipefail
if [[ -n ${FTE_TEST_EXPECT_TOOL_MODE:-} ]]; then
  [[ $# == 1 && $1 == "$FTE_TEST_EXPECT_TOOL_MODE" ]]
else
  [[ ( $# == 1 && $1 == --compare-soft-reset ) ||
     ( $# == 2 && $1 == --test-vendor-recovery ) ]]
fi
echo "tool $*" >> "$FTE_TEST_CASE/events"
echo 'MOCK diagnostic stdout: comparison result'
echo 'MOCK diagnostic stderr: retained' >&2
if [[ ${FTE_TEST_REPLACE_UNIT:-0} == 1 ]]; then
  rm -- "$FTE_TEST_RUNTIME_UNIT"
  echo replacement > "$FTE_TEST_RUNTIME_UNIT"
fi
if [[ ${FTE_TEST_WAIT:-0} == 1 ]]; then
  trap 'echo tool-cleanup >> "$FTE_TEST_CASE/events"; exit 143' TERM
  touch "$FTE_TEST_CASE/tool-ready"
  while :; do sleep 0.02; done
fi
exit "${FTE_TEST_TOOL_EXIT:-0}"
TOOL
export PATH="$fte_test_root/bin:$PATH"

fte_test_count=0
fte_test_new() {
  fte_test_count=$((fte_test_count + 1))
  export FTE_TEST_CASE="$fte_test_root/case-$fte_test_count"
  export FTE_TEST_RUNTIME_UNIT="$FTE_TEST_CASE/runtime-unit"
  mkdir -p "$FTE_TEST_CASE/repo/scripts"
  : > "$FTE_TEST_CASE/events"
  echo 'loaded active' > "$FTE_TEST_CASE/state"
  unset FTE_TEST_UID FTE_TEST_DIRTY FTE_TEST_PKG_FAIL FTE_TEST_COMPILE_FAIL
  unset FTE_TEST_MASK_INEFFECTIVE FTE_TEST_MASK_FAIL FTE_TEST_UNMASK_FAIL
  unset FTE_TEST_START_FAIL FTE_TEST_REPLACE_UNIT FTE_TEST_WAIT FTE_TEST_TOOL_EXIT
  unset FTE_TEST_EXPECT_TOOL_MODE
  fte_test_script=test-medion-soft-reset.sh
  # Keep the real control flow, root check, traps, and tool argument dispatch.
  # Only immutable absolute paths are mapped into this private test directory.
  for fte_test_wrapper in test-medion-soft-reset.sh test-medion-recovery.sh; do
    sed -e "s@/run/systemd/system/fprintd.service@$FTE_TEST_RUNTIME_UNIT@g" \
        -e "s@/run/fte3600-medion-diagnostic.lock@$FTE_TEST_CASE/lock@g" \
        "$fte_test_repo/scripts/$fte_test_wrapper" \
        > "$FTE_TEST_CASE/repo/scripts/$fte_test_wrapper"
  done
  cp "$fte_test_repo/scripts/test-medion-legacy-id.sh" \
    "$FTE_TEST_CASE/repo/scripts/test-medion-legacy-id.sh"
}

fte_test_fail() {
  echo "FAIL $fte_test_count: $*" >&2
  [[ ! -f $FTE_TEST_CASE/output ]] || cat "$FTE_TEST_CASE/output" >&2
  cat "$FTE_TEST_CASE/events" >&2
  exit 1
}
fte_test_run() {
  local fte_test_expected=$1 fte_test_result
  shift
  if "$fte_test_bash" "$FTE_TEST_CASE/repo/scripts/$fte_test_script" "$@" > "$FTE_TEST_CASE/output" 2>&1; then
    fte_test_result=0
  else
    fte_test_result=$?
  fi
  [[ $fte_test_result == "$fte_test_expected" ]] || fte_test_fail "exit $fte_test_result, expected $fte_test_expected"
  if [[ -f $FTE_TEST_CASE/compiled-path ]]; then
    read -r fte_test_compiled < "$FTE_TEST_CASE/compiled-path"
    [[ ! -e ${fte_test_compiled%/*} ]] || fte_test_fail 'temporary compilation directory leaked'
  fi
}
fte_test_run_legacy() {
  local fte_test_expected=$1 fte_test_result
  shift
  if "$fte_test_bash" "$FTE_TEST_CASE/repo/scripts/test-medion-legacy-id.sh" "$@" > "$FTE_TEST_CASE/output" 2>&1; then
    fte_test_result=0
  else
    fte_test_result=$?
  fi
  [[ $fte_test_result == "$fte_test_expected" ]] || fte_test_fail "exit $fte_test_result, expected $fte_test_expected"
  if [[ -f $FTE_TEST_CASE/compiled-path ]]; then
    read -r fte_test_compiled < "$FTE_TEST_CASE/compiled-path"
    [[ ! -e ${fte_test_compiled%/*} ]] || fte_test_fail 'temporary compilation directory leaked'
  fi
}
fte_test_no_mutation() {
  if grep -Eq '^systemctl (mask|unmask|start)|^tool ' "$FTE_TEST_CASE/events"; then
    fte_test_fail 'unexpected service mutation or diagnostic execution'
  fi
}
fte_test_restored_active() {
  [[ $(< "$FTE_TEST_CASE/state") == 'loaded active' ]] || fte_test_fail 'active service was not restored'
  [[ ! -e $FTE_TEST_RUNTIME_UNIT && ! -L $FTE_TEST_RUNTIME_UNIT ]] || fte_test_fail 'runtime mask remains'
  grep -q '^systemctl start fprintd.service$' "$FTE_TEST_CASE/events" || fte_test_fail 'missing restart'
}
fte_test_ok() { printf 'ok %s - %s\n' "$fte_test_count" "$1"; }

fte_test_new
fte_test_run 0
fte_test_no_mutation
grep -q '^cc$' "$FTE_TEST_CASE/events" || fte_test_fail 'default did not compile'
fte_test_ok 'default is compilation-only preflight'

fte_test_new
export FTE_TEST_UID=1000 FTE_TEST_DIRTY=1
fte_test_run 0 --check-only
fte_test_no_mutation
grep -q 'Source worktree dirty: yes' "$FTE_TEST_CASE/output" || fte_test_fail 'dirty source not reported'
fte_test_ok 'explicit preflight needs no root and reports dirty source'

fte_test_new
fte_test_run 1 --unknown
fte_test_no_mutation
fte_test_ok 'unknown option rejected'

fte_test_new
export FTE_TEST_UID=1000
fte_test_run 1 --run
fte_test_no_mutation
fte_test_ok 'run requires root'

fte_test_new
export FTE_TEST_UID=1000
fte_test_run 1 --legacy-id-run
fte_test_no_mutation
fte_test_ok 'legacy identity run requires root'

fte_test_new
export FTE_TEST_UID=1000
fte_test_run 1 --legacy-historical-id-run
fte_test_no_mutation
fte_test_ok 'historical legacy run requires root'

fte_test_new
fte_test_run_legacy 0
fte_test_no_mutation
grep -q '^cc$' "$FTE_TEST_CASE/events" || fte_test_fail 'legacy entry point default did not compile'
fte_test_ok 'legacy entry point defaults to compilation-only preflight'

fte_test_new
fte_test_run_legacy 0 --help
fte_test_no_mutation
! grep -q '^cc$' "$FTE_TEST_CASE/events" || fte_test_fail 'legacy help compiled unexpectedly'
grep -q 'later unverified attachment candidate' "$FTE_TEST_CASE/output" || fte_test_fail 'legacy help omitted attachment boundary'
fte_test_ok 'legacy entry point help is local and non-mutating'

fte_test_new
export FTE_TEST_COMPILE_FAIL=1
fte_test_run 1 --run
fte_test_no_mutation
[[ $(< "$FTE_TEST_CASE/state") == 'loaded active' ]] || fte_test_fail 'compile failure changed state'
fte_test_ok 'compilation failure precedes all service changes'

fte_test_new
export FTE_TEST_PKG_FAIL=1
fte_test_run 1 --run
fte_test_no_mutation
fte_test_ok 'dependency failure does not change service state'

fte_test_new
fte_test_run 0 --run
fte_test_restored_active
fte_test_ok 'active service restored after successful comparison'

fte_test_new
export FTE_TEST_EXPECT_TOOL_MODE=--legacy-id-no-init
fte_test_run 0 --legacy-id-run
fte_test_restored_active
grep -q '^tool --legacy-id-no-init$' "$FTE_TEST_CASE/events" || fte_test_fail 'wrong legacy diagnostic mode'
grep -q 'one bounded legacy-protocol identity candidate read' "$FTE_TEST_CASE/output" || fte_test_fail 'legacy boundary not reported'
fte_test_ok 'legacy identity route invokes only its bounded mode and restores service'

fte_test_new
export FTE_TEST_EXPECT_TOOL_MODE=--legacy-id-no-init
fte_test_run_legacy 0 --run
fte_test_restored_active
grep -q '^tool --legacy-id-no-init$' "$FTE_TEST_CASE/events" || fte_test_fail 'legacy entry point changed the established run mode'
fte_test_ok 'legacy entry point preserves its bounded run behavior'

fte_test_new
export FTE_TEST_EXPECT_TOOL_MODE=--legacy-historical-id
fte_test_run_legacy 0 --historical-run
fte_test_restored_active
grep -q '^tool --legacy-historical-id$' "$FTE_TEST_CASE/events" || fte_test_fail 'legacy historical entry point dispatched wrong mode'
fte_test_ok 'legacy historical entry point is explicit and restores service'

fte_test_new
export FTE_TEST_EXPECT_TOOL_MODE=--legacy-historical-id
fte_test_run 0 --legacy-historical-id-run
fte_test_restored_active
grep -q '^tool --legacy-historical-id$' "$FTE_TEST_CASE/events" || fte_test_fail 'wrong historical diagnostic mode'
grep -q 'later unverified attachment candidate' "$FTE_TEST_CASE/output" || fte_test_fail 'attachment boundary not reported'
fte_test_ok 'attachment route invokes only its prefix and restores service'

fte_test_new
export FTE_TEST_EXPECT_TOOL_MODE=--legacy-ctfdavis-id
fte_test_run_legacy 0 --ctfdavis-run
fte_test_restored_active
grep -q '^tool --legacy-ctfdavis-id$' "$FTE_TEST_CASE/events" || fte_test_fail 'ctfdavis entry point dispatched wrong mode'
fte_test_ok 'ctfdavis entry point runs the no-GPIO candidate and restores service'

fte_test_new
export FTE_TEST_EXPECT_TOOL_MODE=--legacy-id-no-init FTE_TEST_TOOL_EXIT=2
fte_test_run 2 --legacy-id-run
fte_test_restored_active
grep -q 'Diagnostic exit: 2' "$FTE_TEST_CASE/output" || fte_test_fail 'legacy inconclusive status lost'
fte_test_ok 'legacy inconclusive result is preserved and service restored'

fte_test_new
echo 'loaded inactive' > "$FTE_TEST_CASE/state"
fte_test_run 0 --run
[[ $(< "$FTE_TEST_CASE/state") == 'loaded inactive' ]] || fte_test_fail 'inactive service was started'
! grep -q '^systemctl start ' "$FTE_TEST_CASE/events" || fte_test_fail 'unexpected restart'
fte_test_ok 'inactive service remains inactive'

fte_test_new
export FTE_TEST_TOOL_EXIT=2
fte_test_run 2 --run
fte_test_restored_active
grep -q 'MOCK diagnostic stdout' "$FTE_TEST_CASE/output" || fte_test_fail 'stdout lost'
grep -q 'MOCK diagnostic stderr' "$FTE_TEST_CASE/output" || fte_test_fail 'stderr lost'
grep -q 'Diagnostic exit: 2' "$FTE_TEST_CASE/output" || fte_test_fail 'comparison status lost'
fte_test_ok 'exit 2 and both output streams retained; service restored'

fte_test_new
export FTE_TEST_TOOL_EXIT=1
fte_test_run 1 --run
fte_test_restored_active
fte_test_ok 'tool error still restores active service'

fte_test_new
echo 'masked inactive' > "$FTE_TEST_CASE/state"
fte_test_run 0 --run
! grep -Eq '^systemctl (mask|unmask|start)' "$FTE_TEST_CASE/events" || fte_test_fail 'pre-existing persistent mask changed'
fte_test_ok 'pre-existing persistent mask preserved'

fte_test_new
echo 'loaded inactive' > "$FTE_TEST_CASE/state"
ln -s /dev/null "$FTE_TEST_RUNTIME_UNIT"
fte_test_run 0 --run
[[ -L $FTE_TEST_RUNTIME_UNIT && $(readlink "$FTE_TEST_RUNTIME_UNIT") == /dev/null ]] || fte_test_fail 'pre-existing runtime mask removed'
! grep -Eq '^systemctl (mask|unmask|start)' "$FTE_TEST_CASE/events" || fte_test_fail 'pre-existing runtime mask changed'
fte_test_ok 'pre-existing runtime mask preserved'

fte_test_new
echo 'masked active' > "$FTE_TEST_CASE/state"
fte_test_run 1 --run
fte_test_no_mutation
fte_test_ok 'masked but active state rejected'

fte_test_new
echo original > "$FTE_TEST_RUNTIME_UNIT"
fte_test_run 1 --run
fte_test_no_mutation
[[ $(< "$FTE_TEST_RUNTIME_UNIT") == original ]] || fte_test_fail 'runtime override changed'
fte_test_ok 'regular runtime override preserved and rejected'

fte_test_new
ln -s /nonexistent/example "$FTE_TEST_RUNTIME_UNIT"
fte_test_run 1 --run
fte_test_no_mutation
[[ $(readlink "$FTE_TEST_RUNTIME_UNIT") == /nonexistent/example ]] || fte_test_fail 'runtime symlink changed'
fte_test_ok 'non-mask runtime symlink preserved and rejected'

fte_test_new
export FTE_TEST_MASK_FAIL=1
fte_test_run 1 --run
fte_test_restored_active
! grep -q '^tool ' "$FTE_TEST_CASE/events" || fte_test_fail 'tool ran after mask failure'
fte_test_ok 'partially successful mask command is rolled back'

fte_test_new
export FTE_TEST_MASK_INEFFECTIVE=1
fte_test_run 1 --run
fte_test_restored_active
! grep -q '^tool ' "$FTE_TEST_CASE/events" || fte_test_fail 'tool ran without effective isolation'
fte_test_ok 'ineffective runtime mask blocks diagnostic and restores service'

fte_test_new
export FTE_TEST_START_FAIL=1
fte_test_run 1 --run
grep -q 'cleanup: incomplete' "$FTE_TEST_CASE/output" || fte_test_fail 'restart failure not reported'
fte_test_ok 'restart failure is an error, not comparison success'

fte_test_new
export FTE_TEST_UNMASK_FAIL=1
fte_test_run 1 --run
[[ -L $FTE_TEST_RUNTIME_UNIT ]] || fte_test_fail 'unexpected removal after failed unmask'
! grep -q '^systemctl start ' "$FTE_TEST_CASE/events" || fte_test_fail 'restart attempted while unmask failed'
fte_test_ok 'unmask failure reported without unsafe restart'

fte_test_new
export FTE_TEST_REPLACE_UNIT=1
fte_test_run 1 --run
[[ $(< "$FTE_TEST_RUNTIME_UNIT") == replacement ]] || fte_test_fail 'externally replaced runtime unit deleted'
! grep -Eq '^systemctl (unmask|start)' "$FTE_TEST_CASE/events" || fte_test_fail 'external runtime replacement changed'
fte_test_ok 'external runtime replacement preserved'

fte_test_new
exec {fte_test_lock_fd}>>"$FTE_TEST_CASE/lock"
flock --nonblock "$fte_test_lock_fd"
fte_test_run 1 --run
fte_test_no_mutation
flock --unlock "$fte_test_lock_fd"
exec {fte_test_lock_fd}>&-
fte_test_ok 'concurrent wrapper rejected before service changes'

fte_test_new
echo 'loaded activating' > "$FTE_TEST_CASE/state"
fte_test_run 1 --run
fte_test_no_mutation
fte_test_ok 'transitional service state rejected'

fte_test_new
export FTE_TEST_WAIT=1
"$fte_test_bash" "$FTE_TEST_CASE/repo/scripts/test-medion-soft-reset.sh" --run > "$FTE_TEST_CASE/output" 2>&1 &
fte_test_child=$!
for ((fte_test_poll=0; fte_test_poll<250; fte_test_poll++)); do
  [[ ! -e $FTE_TEST_CASE/tool-ready ]] || break
  sleep 0.02
done
[[ -e $FTE_TEST_CASE/tool-ready ]] || fte_test_fail 'diagnostic did not become ready'
kill -TERM "$fte_test_child"
if wait "$fte_test_child"; then fte_test_result=0; else fte_test_result=$?; fi
fte_test_child=
[[ $fte_test_result == 143 ]] || fte_test_fail "TERM returned $fte_test_result instead of 143"
fte_test_restored_active
fte_test_tool_line=$(grep -n '^tool-cleanup$' "$FTE_TEST_CASE/events" | cut -d: -f1)
fte_test_start_line=$(grep -n '^systemctl start fprintd.service$' "$FTE_TEST_CASE/events" | cut -d: -f1)
[[ -n $fte_test_tool_line && $fte_test_tool_line -lt $fte_test_start_line ]] || fte_test_fail 'service restarted before diagnostic stopped'
fte_test_ok 'TERM waits for diagnostic cleanup, then restores service'

fte_test_new
export FTE_TEST_WAIT=1
# timeout forwards INT even when this test itself is launched asynchronously;
# plain asynchronous Bash jobs inherit SIGINT as ignored. Its deadline is only
# a fallback: the test sends INT immediately after the mocked tool is ready.
timeout --preserve-status 10s "$fte_test_bash" "$FTE_TEST_CASE/repo/scripts/test-medion-soft-reset.sh" --run > "$FTE_TEST_CASE/output" 2>&1 &
fte_test_child=$!
for ((fte_test_poll=0; fte_test_poll<250; fte_test_poll++)); do
  [[ ! -e $FTE_TEST_CASE/tool-ready ]] || break
  sleep 0.02
done
[[ -e $FTE_TEST_CASE/tool-ready ]] || fte_test_fail 'INT diagnostic did not become ready'
kill -INT "$fte_test_child"
if wait "$fte_test_child"; then fte_test_result=0; else fte_test_result=$?; fi
fte_test_child=
[[ $fte_test_result == 130 ]] || fte_test_fail "INT returned $fte_test_result instead of 130"
fte_test_restored_active
read -r fte_test_compiled < "$FTE_TEST_CASE/compiled-path"
[[ ! -e ${fte_test_compiled%/*} ]] || fte_test_fail 'interrupted compilation directory leaked'
fte_test_ok 'INT preserves signal status and restores service and temporary files'

fte_test_new
echo 'loaded inactive' > "$FTE_TEST_CASE/state"
ln -s /dev/null "$FTE_TEST_RUNTIME_UNIT"
export FTE_TEST_REPLACE_UNIT=1
fte_test_run 1 --run
[[ $(< "$FTE_TEST_RUNTIME_UNIT") == replacement ]] || fte_test_fail 'replacement of old mask changed'
! grep -Eq '^systemctl (unmask|start)' "$FTE_TEST_CASE/events" || fte_test_fail 'external replacement of old mask changed'
fte_test_ok 'external replacement of a pre-existing mask is not called successful cleanup'

for fte_test_load in not-found error bad-setting; do
  fte_test_new
  printf '%s inactive\n' "$fte_test_load" > "$FTE_TEST_CASE/state"
  fte_test_run 1 --run
  fte_test_no_mutation
  fte_test_ok "unloaded service ($fte_test_load) rejected before isolation"
done

# Run every pair against the same service and lock. The second wrapper must
# neither launch a diagnostic nor unmask/restart the first wrapper's daemon.
for fte_test_holder in test-medion-soft-reset.sh test-medion-recovery.sh; do
  for fte_test_contender in test-medion-soft-reset.sh test-medion-recovery.sh; do
    fte_test_new
    export FTE_TEST_WAIT=1
    fte_test_args=()
    [[ $fte_test_holder != test-medion-soft-reset.sh ]] || fte_test_args=(--run)
    "$fte_test_bash" "$FTE_TEST_CASE/repo/scripts/$fte_test_holder" "${fte_test_args[@]}" > "$FTE_TEST_CASE/holder-output" 2>&1 &
    fte_test_child=$!
    for ((fte_test_poll=0; fte_test_poll<250; fte_test_poll++)); do
      [[ ! -e $FTE_TEST_CASE/tool-ready ]] || break
      sleep 0.02
    done
    [[ -e $FTE_TEST_CASE/tool-ready ]] || fte_test_fail 'lock holder diagnostic did not start'
    fte_test_script=$fte_test_contender
    fte_test_args=()
    [[ $fte_test_contender != test-medion-soft-reset.sh ]] || fte_test_args=(--run)
    fte_test_run 1 "${fte_test_args[@]}"
    [[ $(grep -c '^tool ' "$FTE_TEST_CASE/events") == 1 ]] || fte_test_fail 'concurrent diagnostic started'
    ! grep -Eq '^systemctl (unmask|start)' "$FTE_TEST_CASE/events" || fte_test_fail 'contender changed holder service state'
    kill -TERM "$fte_test_child"
    if wait "$fte_test_child"; then fte_test_result=0; else fte_test_result=$?; fi
    fte_test_child=
    [[ $fte_test_result == 143 ]] || fte_test_fail 'interrupted holder lost signal status'
    fte_test_restored_active
    fte_test_tool_line=$(grep -n '^tool-cleanup$' "$FTE_TEST_CASE/events" | cut -d: -f1)
    fte_test_start_line=$(grep -n '^systemctl start fprintd.service$' "$FTE_TEST_CASE/events" | cut -d: -f1)
    [[ -n $fte_test_tool_line && $fte_test_tool_line -lt $fte_test_start_line ]] || fte_test_fail 'holder restarted service before child stopped'
    fte_test_ok "$fte_test_holder excludes $fte_test_contender through cleanup"
  done
done

printf 'PASS: %s shell-wrapper regression cases (mocked, no hardware access).\n' "$fte_test_count"
