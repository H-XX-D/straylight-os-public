#!/bin/bash
# tests/integration/test_firstboot_script.sh
# Integration test for the straylight-firstboot shell script.
# Creates a temp state file and runs the script with STATE_FILE override.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
FIRSTBOOT_SCRIPT="${SCRIPT_DIR}/services/firstboot/straylight-firstboot"

TMPDIR=$(mktemp -d)
STATE_FILE="${TMPDIR}/state"
trap "rm -rf ${TMPDIR}" EXIT

echo "=== Test 1: firstboot -> oobe transition ==="
echo -n "firstboot" > "${STATE_FILE}"
export STATE_FILE
bash "${FIRSTBOOT_SCRIPT}"
STATE=$(cat "${STATE_FILE}")
if [ "${STATE}" = "oobe" ]; then
    echo "PASS: state transitioned to 'oobe'"
else
    echo "FAIL: expected 'oobe', got '${STATE}'"
    exit 1
fi

echo "=== Test 2: idempotent — second run with state=oobe ==="
bash "${FIRSTBOOT_SCRIPT}"
STATE=$(cat "${STATE_FILE}")
if [ "${STATE}" = "oobe" ]; then
    echo "PASS: state remains 'oobe' (idempotent)"
else
    echo "FAIL: expected 'oobe', got '${STATE}'"
    exit 1
fi

echo "=== Test 3: exits 0 when state is 'complete' ==="
echo -n "complete" > "${STATE_FILE}"
bash "${FIRSTBOOT_SCRIPT}"
STATE=$(cat "${STATE_FILE}")
if [ "${STATE}" = "complete" ]; then
    echo "PASS: state unchanged at 'complete'"
else
    echo "FAIL: expected 'complete', got '${STATE}'"
    exit 1
fi

echo "=== All firstboot script tests passed ==="
