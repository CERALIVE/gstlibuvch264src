#!/usr/bin/env bash
#
# check-libuvc-fork.sh — fork pin-integrity + capability-presence CI guard.
#
# Task 23 (harden-v2). Fails LOUDLY (non-zero exit) if EITHER:
#
#   1. PIN INTEGRITY: the pinned fork SHA in the build config drifts from the SHA
#      recorded in the ADR (libuvch264src/docs/notes/libuvc-fork-adr.md).
#
#      Post-Task-22 the fork SHA lives in exactly ONE build location —
#      scripts/build-libuvc.sh (FORK_SHA=...) — which the Dockerfile and the
#      top-level CMakeLists.txt both call. So the build-side pin is read from
#      that single source of truth, and CMakeLists.txt / Dockerfile are asserted
#      to delegate to the script (i.e. NOT carry a second, drift-prone SHA).
#
#   2. CAPABILITY PRESENCE: the fork at the pinned SHA is MISSING any of the
#      three CeraLive divergences the plugin requires. The CeraLive/libuvc fork
#      is a HARD DIVERGENCE (ADR: upstream-sync NONE), so this is a
#      capability-PRESENCE check, NOT an upstream-drift check:
#        a. UVC_FRAME_FORMAT_H265            in include/  (H.265 format enum)
#        b. 0x0150                           in src/      (UVC 1.5 bcdUVC case)
#        c. LIBUVC_AUTO_DETACH_KERNEL_DRIVER in CMakeLists.txt (configurable
#                                                          auto-detach option)
#
# The fork is PUBLIC (ADR: fork-visibility PUBLIC) — the clone uses NO
# credentials, deploy keys, or tokens. A failure to clone without auth is itself
# a guard failure (the ADR's no-credential requirement).
#
# Usage:
#   scripts/check-libuvc-fork.sh
#
# Exit codes:
#   0  all checks passed
#   1  a check failed (pin drift, missing capability, or unauthenticated clone)
#   2  a precondition is missing (tool/file not found)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_SCRIPT="${REPO_ROOT}/scripts/build-libuvc.sh"
ADR="${REPO_ROOT}/libuvch264src/docs/notes/libuvc-fork-adr.md"
CMAKELISTS="${REPO_ROOT}/CMakeLists.txt"
DOCKERFILE="${REPO_ROOT}/Dockerfile"

FORK_URL="https://github.com/CeraLive/libuvc.git"

PASS_MARK="PASS"
FAIL_MARK="FAIL"
fail_count=0

log()  { printf '[check-fork] %s\n' "$*"; }
ok()   { printf '[check-fork]   %s %s\n' "${PASS_MARK}" "$*"; }
bad()  { printf '[check-fork]   %s %s\n' "${FAIL_MARK}" "$*" >&2; fail_count=$((fail_count + 1)); }
die()  { printf '[check-fork] PRECONDITION ERROR: %s\n' "$*" >&2; exit 2; }

command -v git    >/dev/null 2>&1 || die "git not found on PATH"
command -v grep   >/dev/null 2>&1 || die "grep not found on PATH"
[ -f "${BUILD_SCRIPT}" ] || die "build script not found: ${BUILD_SCRIPT}"
[ -f "${ADR}" ]          || die "ADR not found: ${ADR}"
[ -f "${CMAKELISTS}" ]   || die "CMakeLists.txt not found: ${CMAKELISTS}"

# A 40-char lowercase-hex git object id (ERE — consumed by grep -oE).
SHA_RE='[0-9a-f]{40}'

# --- 1. PIN INTEGRITY -------------------------------------------------------
log "=== 1. Pin integrity (build config SHA vs ADR SHA) ==="

# 1a. Build-side pin: the single FORK_SHA in scripts/build-libuvc.sh.
BUILD_SHA="$(grep -oE 'FORK_SHA="[0-9a-f]{40}"' "${BUILD_SCRIPT}" | head -1 | grep -oE '[0-9a-f]{40}' || true)"
if [ -z "${BUILD_SHA}" ]; then
	bad "could not extract FORK_SHA from ${BUILD_SCRIPT#"${REPO_ROOT}/"}"
else
	log "build-libuvc.sh FORK_SHA = ${BUILD_SHA}"
fi

# 1b. ADR-side pin: the SHA on the GIT_TAG pin line in the ADR's build-pin block,
#     cross-checked against the "Tag/HEAD commit SHA" provenance line. Both must
#     agree and both must equal the build-side pin.
ADR_GITTAG_SHA="$(grep -E '^[[:space:]]*GIT_TAG' "${ADR}" | grep -oE "${SHA_RE}" | head -1 || true)"
ADR_HEAD_SHA="$(grep -iE 'Tag/HEAD commit SHA' "${ADR}" | grep -oE "${SHA_RE}" | head -1 || true)"

if [ -z "${ADR_GITTAG_SHA}" ]; then
	bad "could not extract GIT_TAG pin SHA from ADR"
else
	log "ADR GIT_TAG pin SHA      = ${ADR_GITTAG_SHA}"
fi
if [ -z "${ADR_HEAD_SHA}" ]; then
	bad "could not extract 'Tag/HEAD commit SHA' from ADR"
else
	log "ADR Tag/HEAD SHA         = ${ADR_HEAD_SHA}"
fi

# 1c. Internal ADR consistency: GIT_TAG pin == documented HEAD SHA.
if [ -n "${ADR_GITTAG_SHA}" ] && [ -n "${ADR_HEAD_SHA}" ]; then
	if [ "${ADR_GITTAG_SHA}" = "${ADR_HEAD_SHA}" ]; then
		ok "ADR internally consistent (GIT_TAG pin == Tag/HEAD SHA)"
	else
		bad "ADR self-disagreement: GIT_TAG ${ADR_GITTAG_SHA} != Tag/HEAD ${ADR_HEAD_SHA}"
	fi
fi

# 1d. The core pin-integrity assertion: build pin == ADR pin.
if [ -n "${BUILD_SHA}" ] && [ -n "${ADR_GITTAG_SHA}" ]; then
	if [ "${BUILD_SHA}" = "${ADR_GITTAG_SHA}" ]; then
		ok "pin integrity: build-libuvc.sh SHA matches ADR (${BUILD_SHA})"
	else
		bad "PIN DRIFT: build-libuvc.sh ${BUILD_SHA} != ADR ${ADR_GITTAG_SHA}"
	fi
fi

# 1e. Single-source guard: CMakeLists.txt and Dockerfile must NOT carry a second
#     hardcoded FORK SHA — they delegate to scripts/build-libuvc.sh. A stray fork
#     SHA in either file is a drift vector even if build-libuvc.sh is correct.
#     (The upstream base SHA 68d07a0... is allowed only as the documented
#     rollback/provenance pin, so we look specifically for the FORK SHA.)
if [ -n "${BUILD_SHA}" ]; then
	if grep -q "${BUILD_SHA}" "${CMAKELISTS}"; then
		bad "CMakeLists.txt hardcodes the fork SHA — must delegate to build-libuvc.sh"
	else
		ok "CMakeLists.txt carries no second fork SHA (delegates to build-libuvc.sh)"
	fi
	if [ -f "${DOCKERFILE}" ]; then
		if grep -q "${BUILD_SHA}" "${DOCKERFILE}"; then
			bad "Dockerfile hardcodes the fork SHA — must delegate to build-libuvc.sh"
		else
			ok "Dockerfile carries no second fork SHA (delegates to build-libuvc.sh)"
		fi
	fi
	if grep -q 'scripts/build-libuvc.sh' "${CMAKELISTS}"; then
		ok "CMakeLists.txt references scripts/build-libuvc.sh (single-source build)"
	else
		bad "CMakeLists.txt does not reference scripts/build-libuvc.sh"
	fi
fi

PIN_SHA="${BUILD_SHA:-${ADR_GITTAG_SHA:-}}"
[ -n "${PIN_SHA}" ] || die "no usable pinned SHA resolved; cannot run capability check"

# --- 2. CAPABILITY PRESENCE -------------------------------------------------
log "=== 2. Capability presence (fork @ ${PIN_SHA}) ==="

WORK="$(mktemp -d)"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

# SHA-pinned, unauthenticated, shallow fetch (PUBLIC fork — no credentials). The
# git init + fetch --depth 1 + checkout FETCH_HEAD idiom is required: a bare SHA
# cannot be passed to `git clone --branch`. Disable any ambient credential helper
# so the no-auth requirement is actually exercised, not masked by a cached token.
log "cloning ${FORK_URL} @ ${PIN_SHA} (no credentials)"
if ! (
	cd "${WORK}"
	git init -q .
	git remote add origin "${FORK_URL}"
	git \
		-c credential.helper= \
		-c http.https://github.com/.extraheader= \
		fetch -q --depth 1 origin "${PIN_SHA}"
	git checkout -q -f FETCH_HEAD
); then
	bad "unauthenticated clone of fork @ ${PIN_SHA} FAILED (network or pin error)"
	log "=== SUMMARY: ${fail_count} failure(s) ==="
	exit 1
fi
ok "unauthenticated clone of fork @ ${PIN_SHA} succeeded"

# Confirm the checked-out tree is exactly the pinned commit.
GOT_SHA="$(cd "${WORK}" && git rev-parse HEAD)"
if [ "${GOT_SHA}" = "${PIN_SHA}" ]; then
	ok "checked-out HEAD == pinned SHA"
else
	bad "checked-out HEAD ${GOT_SHA} != pinned ${PIN_SHA}"
fi

# 2a. H.265 format enum in the public header.
if grep -rq 'UVC_FRAME_FORMAT_H265' "${WORK}/include"; then
	ok "capability: UVC_FRAME_FORMAT_H265 present in include/"
else
	bad "MISSING capability: UVC_FRAME_FORMAT_H265 not found in include/"
fi

# 2b. UVC 1.5 bcdUVC case in the source. Match 0x0150 case-insensitively so
#     0x0150 / 0x0150u and either letter case all count.
if grep -riq '0x0150' "${WORK}/src"; then
	ok "capability: UVC 1.5 case (0x0150) present in src/"
else
	bad "MISSING capability: UVC 1.5 case (0x0150) not found in src/"
fi

# 2c. Configurable auto-detach option in the fork's CMakeLists.txt.
if [ -f "${WORK}/CMakeLists.txt" ] && grep -q 'LIBUVC_AUTO_DETACH_KERNEL_DRIVER' "${WORK}/CMakeLists.txt"; then
	ok "capability: LIBUVC_AUTO_DETACH_KERNEL_DRIVER option present in fork CMakeLists.txt"
else
	bad "MISSING capability: LIBUVC_AUTO_DETACH_KERNEL_DRIVER not found in fork CMakeLists.txt"
fi

# --- Summary ----------------------------------------------------------------
if [ "${fail_count}" -eq 0 ]; then
	log "=== SUMMARY: ALL CHECKS PASSED (pin integrity + 3 capabilities) ==="
	exit 0
else
	log "=== SUMMARY: ${fail_count} CHECK(S) FAILED ==="
	exit 1
fi
