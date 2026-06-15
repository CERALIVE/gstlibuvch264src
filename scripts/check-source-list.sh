#!/usr/bin/env bash
#
# check-source-list.sh — single-source plugin file-list consistency CI guard.
#
# Task 23 (harden-v2). sources.txt (libuvch264src/src/sources.txt, created in
# Task 18) is the SINGLE source of truth for the element's .c files. Three build
# consumers read it:
#   - top-level CMakeLists.txt      via file(STRINGS ... sources.txt)
#   - tests/CMakeLists.txt          via file(STRINGS ... sources.txt)
#   - libuvch264src/src/meson.build via fs.read('sources.txt')
#
# This guard fails LOUDLY (non-zero exit) if sources.txt disagrees with what the
# build actually compiles, i.e. if ANY of the following holds:
#
#   1. A name listed in sources.txt has no matching .c file in src/
#      (a build referencing a missing TU — configure/compile error).
#   2. A .c file exists in src/ but is NOT listed in sources.txt
#      (a translation unit silently dropped from every build).
#   3. A build consumer (top CMakeLists.txt / tests CMakeLists.txt / meson.build)
#      stopped reading sources.txt and hardcoded its own list instead
#      (the single-source invariant broken — drift becomes possible).
#
# Usage:
#   scripts/check-source-list.sh [--sources=<path>]
#
#     --sources=<path>  Override the sources.txt path (used by the self-test to
#                       point at a scratch copy with a deliberate mismatch).
#                       Default: libuvch264src/src/sources.txt.
#
# Exit codes:
#   0  sources.txt is consistent with the source tree and all build consumers
#   1  a mismatch was found
#   2  a precondition is missing (tool/file not found)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SRC_DIR="${REPO_ROOT}/libuvch264src/src"
SOURCES_TXT="${SRC_DIR}/sources.txt"
TOP_CMAKE="${REPO_ROOT}/CMakeLists.txt"
TESTS_CMAKE="${REPO_ROOT}/tests/CMakeLists.txt"
MESON="${SRC_DIR}/meson.build"

PASS_MARK="PASS"
FAIL_MARK="FAIL"
fail_count=0

log()  { printf '[check-srclist] %s\n' "$*"; }
ok()   { printf '[check-srclist]   %s %s\n' "${PASS_MARK}" "$*"; }
bad()  { printf '[check-srclist]   %s %s\n' "${FAIL_MARK}" "$*" >&2; fail_count=$((fail_count + 1)); }
die()  { printf '[check-srclist] PRECONDITION ERROR: %s\n' "$*" >&2; exit 2; }

# --- Args -------------------------------------------------------------------
for arg in "$@"; do
	case "$arg" in
		--sources=*) SOURCES_TXT="${arg#*=}" ;;
		--sources)   die "--sources requires a value: --sources=<path>" ;;
		-h|--help)   sed -n '2,33p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) die "unknown argument: '${arg}' (try --help)" ;;
	esac
done

command -v grep >/dev/null 2>&1 || die "grep not found on PATH"
command -v sort >/dev/null 2>&1 || die "sort not found on PATH"
command -v comm >/dev/null 2>&1 || die "comm not found on PATH"
command -v find >/dev/null 2>&1 || die "find not found on PATH"
[ -f "${SOURCES_TXT}" ] || die "sources.txt not found: ${SOURCES_TXT}"
[ -d "${SRC_DIR}" ]     || die "src dir not found: ${SRC_DIR}"
[ -f "${TOP_CMAKE}" ]   || die "top CMakeLists.txt not found: ${TOP_CMAKE}"
[ -f "${MESON}" ]       || die "src meson.build not found: ${MESON}"

log "sources.txt = ${SOURCES_TXT}"
log "src dir     = ${SRC_DIR}"

# --- Build the two sets -----------------------------------------------------
# LISTED: bare .c names from sources.txt (ignore blank lines and # comments,
# strip CR for CRLF safety, trim surrounding whitespace).
listed="$(
	sed -e 's/\r$//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "${SOURCES_TXT}" \
		| grep -vE '^[[:space:]]*$' \
		| grep -vE '^#' \
		| sort -u
)"

# ON_DISK: every .c file actually present in src/ (bare names).
on_disk="$(
	find "${SRC_DIR}" -maxdepth 1 -type f -name '*.c' -printf '%f\n' | sort -u
)"

n_listed="$(printf '%s\n' "${listed}" | grep -cE '.' || true)"
n_disk="$(printf '%s\n' "${on_disk}" | grep -cE '.' || true)"
log "listed in sources.txt: ${n_listed} file(s)"
log "present in src/ (.c):  ${n_disk} file(s)"

# --- Check 1: every listed name exists on disk ------------------------------
log "=== 1. Every sources.txt entry exists in src/ ==="
missing_on_disk="$(comm -23 <(printf '%s\n' "${listed}") <(printf '%s\n' "${on_disk}") || true)"
if [ -z "${missing_on_disk}" ]; then
	ok "all listed sources exist as .c files"
else
	while IFS= read -r f; do
		[ -n "$f" ] && bad "listed in sources.txt but MISSING from src/: ${f}"
	done <<<"${missing_on_disk}"
fi

# --- Check 2: every .c on disk is listed (no silently-dropped TU) -----------
log "=== 2. Every src/*.c is listed in sources.txt ==="
not_listed="$(comm -13 <(printf '%s\n' "${listed}") <(printf '%s\n' "${on_disk}") || true)"
if [ -z "${not_listed}" ]; then
	ok "no orphan .c files (every translation unit is in sources.txt)"
else
	while IFS= read -r f; do
		[ -n "$f" ] && bad "present in src/ but NOT in sources.txt (dropped from build): ${f}"
	done <<<"${not_listed}"
fi

# --- Check 3: build consumers still READ sources.txt (single-source intact) --
log "=== 3. Build consumers read sources.txt (no hardcoded list) ==="
if grep -q 'sources\.txt' "${TOP_CMAKE}"; then
	ok "top CMakeLists.txt reads sources.txt"
else
	bad "top CMakeLists.txt no longer references sources.txt (hardcoded list?)"
fi
if [ -f "${TESTS_CMAKE}" ]; then
	if grep -q 'sources\.txt' "${TESTS_CMAKE}"; then
		ok "tests/CMakeLists.txt reads sources.txt"
	else
		bad "tests/CMakeLists.txt no longer references sources.txt (hardcoded list?)"
	fi
fi
if grep -q 'sources\.txt' "${MESON}"; then
	ok "src/meson.build reads sources.txt"
else
	bad "src/meson.build no longer references sources.txt (hardcoded list?)"
fi

# --- Summary ----------------------------------------------------------------
if [ "${fail_count}" -eq 0 ]; then
	log "=== SUMMARY: sources.txt consistent with src/ and all build consumers ==="
	exit 0
else
	log "=== SUMMARY: ${fail_count} mismatch(es) found ==="
	exit 1
fi
