#!/usr/bin/env bash
# Contract test for enforce_scope_decision.sh.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ENFORCER="${SCRIPT_DIR}/enforce_scope_decision.sh"

expect_success() {
    local label="$1"
    shift
    if bash "$ENFORCER" "$@"; then
        echo "PASS: ${label}"
    else
        echo "FAIL: ${label} should succeed" >&2
        return 1
    fi
}

expect_failure() {
    local label="$1"
    shift
    if bash "$ENFORCER" "$@" >/dev/null 2>&1; then
        echo "FAIL: ${label} should fail" >&2
        return 1
    else
        echo "PASS: ${label}"
    fi
}

expect_success 'explicit blocked=false' false
expect_failure 'blocked=true' true
expect_failure 'unexpected decision' pending
expect_failure 'missing decision'
expect_failure 'empty decision' ''

echo 'Scope decision contract passed.'
