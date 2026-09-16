#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/test-results"
scratch=$(mktemp -d "$root/test-results/build-suite.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

printf 'ID=debian\nVERSION_CODENAME=trixie\n' > "$scratch/trixie"
printf 'ID=debian\nVERSION_CODENAME=bookworm\n' > "$scratch/bookworm"
printf 'ID=ubuntu\nVERSION_CODENAME=trixie\n' > "$scratch/wrong-distro"
printf 'ID=debian\n' > "$scratch/missing-suite"

check() {
  bash "$root/scripts/check-build-suite.sh" "$@"
}

reject() {
  if check "$@" > "$scratch/output" 2>&1; then
    printf 'FAIL: accepted incompatible build suite: %s\n' "$*" >&2
    exit 1
  fi
  grep -q 'Build suite mismatch:' "$scratch/output"
}

check trixie "$scratch/trixie"
check bookworm "$scratch/bookworm"
reject trixie "$scratch/bookworm"
reject bookworm "$scratch/trixie"
reject trixie "$scratch/wrong-distro"
# An inherited environment must not fill in missing image metadata.
VERSION_CODENAME=trixie reject trixie "$scratch/missing-suite"

printf 'PASS: build suite contract (production, portability, mismatch, missing metadata)\n'
