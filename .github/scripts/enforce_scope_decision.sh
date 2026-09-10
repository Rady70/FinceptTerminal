#!/usr/bin/env bash
# Enforce the PR Scope Gate's fail-closed output contract.
set -euo pipefail

decision="${1-}"
if [[ "$#" -eq 1 && "$decision" == "false" ]]; then
    echo "PR Scope Gate decision is explicitly blocked=false; passing."
    exit 0
fi

if [[ "$#" -eq 0 ]]; then
    received='<missing>'
elif [[ -z "$decision" ]]; then
    received='<empty>'
else
    printf -v received '%q' "$decision"
fi
printf '::error::PR Scope Gate requires the exact decision blocked=false; received %s.\n' "$received"
exit 1
