#!/usr/bin/env bash
# Host-side unit tests: plain gcc, no Zephyr/native_sim/twister. See
# tests/host/README.md for why, and what this does/doesn't cover.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

TESTS="test_protocol test_biquad_pipeline test_adau1860_coeffs test_tone_path test_gatt_validation test_settings_dispatch"
FAIL=0

for t in $TESTS; do
	echo "=== building $t ==="
	if ! gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Ifakes \
		-o "$BUILD_DIR/$t" "$t.c" -lm; then
		echo "BUILD FAILED: $t"
		FAIL=1
		continue
	fi
	echo "=== running $t ==="
	if ! "$BUILD_DIR/$t"; then
		FAIL=1
	fi
	echo
done

if [ "$FAIL" -eq 0 ]; then
	echo "ALL TEST SUITES PASSED"
else
	echo "ONE OR MORE TEST SUITES FAILED"
fi
exit $FAIL
