#!/usr/bin/env bash
set -euo pipefail

expected=${1:?Expected Debian suite is required}
ID=
VERSION_CODENAME=
# shellcheck source=/dev/null
source "${2:-/etc/os-release}"

if [[ "$ID" != debian || "$VERSION_CODENAME" != "$expected" ]]; then
  printf 'Build suite mismatch: expected Debian %s, got %s/%s\n' \
    "$expected" "${ID:-unknown}" "${VERSION_CODENAME:-unknown}" >&2
  exit 1
fi
printf 'Build suite verified: Debian %s\n' "$expected"
