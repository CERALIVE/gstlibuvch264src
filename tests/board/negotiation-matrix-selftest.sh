#!/usr/bin/env bash

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail

  board_test_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
  # shellcheck source=tests/board/negotiation-matrix.sh
  . "$board_test_dir/negotiation-matrix.sh"

  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT

  cat > "$tmpdir/startup-gap.log" <<'EOF'
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: 0:00:01.300000000, pts: 0:00:01.300000000, duration: 0:00:00.033333333, offset: 0)
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: 0:00:01.333333333, pts: 0:00:01.333333333, duration: 0:00:00.033333333, offset: 1)
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: 0:00:01.366666667, pts: 0:00:01.366666667, duration: 0:00:00.033333333, offset: 2)
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: 0:00:01.400000000, pts: 0:00:01.400000000, duration: 0:00:00.033333333, offset: 3)
EOF

  cat > "$tmpdir/one-frame.log" <<'EOF'
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: 0:00:01.300000000, pts: 0:00:01.300000000, duration: 0:00:00.033333333, offset: 0)
EOF

  cat > "$tmpdir/malformed-pts.log" <<'EOF'
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: none, pts: none, duration: none, offset: 0)
/GstPipeline:pipeline0/GstIdentity:auprobe: last-message = chain   ******* (auprobe:sink) (100 bytes, dts: none, pts: none, duration: none, offset: 1)
EOF

  assert_equal() {
    local expected=$1 actual=$2 label=$3
    if [ "$actual" != "$expected" ]; then
      printf 'FAIL: %s expected=%s actual=%s\n' "$label" "$expected" "$actual" >&2
      exit 1
    fi
  }

  old_fps=$(awk 'BEGIN { printf "%.3f", 4 / 1.400 }')
  old_verdict=$(awk -v fps="$old_fps" 'BEGIN { print (fps >= 27.000) ? "PASS" : "FAIL" }')

  read -r stream_s fps source < <(run_rate_metrics "$tmpdir/startup-gap.log" 4 1.400)
  new_verdict=$(awk -v fps="$fps" 'BEGIN { print (fps >= 27.000) ? "PASS" : "FAIL" }')

  assert_equal FAIL "$old_verdict" "whole-process wall method"
  assert_equal 0.100000000 "$stream_s" "PTS span"
  assert_equal 30.000 "$fps" "PTS-span fps"
  assert_equal pts-span "$source" "PTS-span source"
  assert_equal PASS "$new_verdict" "PTS-span floor verdict"

  read -r stream_s fps source < <(run_rate_metrics "$tmpdir/one-frame.log" 1 2.000)
  assert_equal unavailable "$stream_s" "one-frame stream duration"
  assert_equal 0.500 "$fps" "one-frame wall fallback fps"
  assert_equal wall-fallback "$source" "one-frame fallback source"

  read -r stream_s fps source < <(run_rate_metrics "$tmpdir/malformed-pts.log" 2 2.000)
  assert_equal unavailable "$stream_s" "malformed-PTS stream duration"
  assert_equal 1.000 "$fps" "malformed-PTS wall fallback fps"
  assert_equal wall-fallback "$source" "malformed-PTS fallback source"

  transition_kind=$(transition_result_kind 0 2 0 2>/dev/null || true)
  assert_equal error "$transition_kind" "zero-AU element error classification"
  assert_equal invalid-mode "$(transition_result_kind 0 0 1)" "invalid-mode classification"
  assert_equal no-aus "$(transition_result_kind 0 0 0)" "signal-free zero-AU classification"
  assert_equal pass "$(transition_result_kind 30 0 0)" "successful transition classification"

  printf 'OLD_METHOD: wall=1.400s fps=%s floor=27.000 verdict=%s\n' "$old_fps" "$old_verdict"
  printf 'NEW_METHOD: wall=1.400s stream=%ss fps=%s source=%s floor=27.000 verdict=%s\n' \
    "0.100000000" "30.000" "pts-span" "$new_verdict"
  printf 'FALLBACKS: one-frame=PASS malformed-pts=PASS source=wall-fallback\n'
  printf 'TRANSITION_CLASSIFICATION: element-error=FAIL invalid-mode=FAIL no-signal=INCONCLUSIVE pass=PASS\n'
  printf 'PASS: negotiation-matrix PTS-span scoring self-test\n'
fi
