#!/usr/bin/env bash
# Run the authoritative PyYAML-based packaging/version validator.
#
# Keep this file as the stable CI entry point. The semantic parsing belongs in
# the Flatpak preflight so the full pre-flight and the version gate cannot drift
# apart.
#
# Usage: check_version_drift.sh [repo-root]   (default: cwd)
set -euo pipefail

ROOT="${1:-.}"
PREFLIGHT="${ROOT}/fincept-qt/packaging/flatpak/preflight.py"
PYTHON_BIN="${PYTHON_BIN:-}"

if [ ! -f "${PREFLIGHT}" ]; then
    echo "::error::${PREFLIGHT} not found"
    exit 1
fi

if [ -z "${PYTHON_BIN}" ]; then
    if command -v python3 >/dev/null 2>&1 && python3 --version >/dev/null 2>&1; then
        PYTHON_BIN=python3
    else
        PYTHON_BIN=python
    fi
fi
if ! command -v "${PYTHON_BIN}" >/dev/null 2>&1; then
    echo "::error::a usable Python interpreter was not found"
    exit 1
fi

exec "${PYTHON_BIN}" "${PREFLIGHT}" --version-only --repo-root "${ROOT}"
