#!/usr/bin/env bash
#
# mayhem/test.sh - run the known-answer probe (built by mayhem/build.sh against libvapoursynth with
# upstream's normal flags) and assert every computed value. The comparison happens here in bash, so a
# neutered probe that prints nothing fails.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SRC:=/mayhem}"
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

PROBE="$SRC/kat_probe"
if [ ! -x "$PROBE" ]; then
  echo "FAIL: $PROBE missing - mayhem/build.sh should have built it" >&2
  emit_ctrf kat-probe 0 1
  exit 1
fi

out="$("$PROBE" 2>&1)"
echo "$out"

passed=0 failed=0
check() { # name expected
  if grep -qx "KAT $1 $2" <<<"$out"; then passed=$((passed + 1)); else failed=$((failed + 1)); echo "FAIL: $1 expected '$2', got '$(grep "^KAT $1 " <<<"$out" || echo missing)'" >&2; fi
}
check blank_gray8 50
check expr_double 100
check expr_sum 42
check expr_max_div 10
check expr_gray16 1500
check expr_format_change 200
check invert 245
check crop_dims 12x12
check resize_dims 8x4
check resize_value 77
check splice_frames 3
check splice_last 20
check reverse_first 20
check planestats_avg 0.3922
check planestats_max 100
check turn90_dims 8x16
if grep -q '^KAT_ERROR' <<<"$out"; then failed=$((failed + 1)); echo "FAIL: probe reported errors" >&2; fi

emit_ctrf kat-probe "$passed" "$failed"
