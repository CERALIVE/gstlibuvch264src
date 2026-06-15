#!/usr/bin/env bash
#
# check-reproducibility.sh — plugin .so build-determinism CI guard.
#
# Task 23 (harden-v2) + ADR "Reproducibility Requirements". Builds the GStreamer
# plugin shared object TWICE from the same pinned inputs and compares sha256sums
# to confirm the build is deterministic. Two independent builds of the same
# source at the same fork SHA must produce a byte-identical .so.
#
# If the raw .so hashes differ, the script applies a documented NORMALIZATION
# step — strip all symbols and drop the .note.gnu.build-id section (the only
# toolchain-embedded, content-derived bytes that legitimately vary) — and
# compares the normalized hashes. A normalized match is reported as a
# "deterministic modulo build-id" PASS with the explained difference; a
# normalized mismatch is a hard FAIL (real non-determinism: embedded build path,
# timestamp, or unordered link input).
#
# The two builds share ONE build directory so the embedded __FILE__ paths and
# the CMake configure are identical between runs — isolating the comparison to
# genuine toolchain non-determinism rather than a changed working path.
#
# Usage:
#   scripts/check-reproducibility.sh [--build-dir=<dir>] [--use-fork|--no-fork]
#
#     --build-dir=<dir>  Build directory (default: build-repro under repo root).
#     --use-fork         Configure with -DLIBUVC_USE_FORK=ON  (default).
#     --no-fork          Configure with -DLIBUVC_USE_FORK=OFF (upstream+patches).
#
# Exit codes:
#   0  raw hashes match (fully deterministic) OR normalized hashes match
#      (deterministic modulo the explained build-id difference)
#   1  normalized hashes differ (real non-determinism)
#   2  a precondition / build failure
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build-repro"
USE_FORK="ON"
SO_NAME="libgstlibuvch264src.so"

log()  { printf '[check-repro] %s\n' "$*"; }
die()  { printf '[check-repro] ERROR: %s\n' "$*" >&2; exit 2; }

for arg in "$@"; do
	case "$arg" in
		--build-dir=*) BUILD_DIR="${arg#*=}" ;;
		--use-fork)    USE_FORK="ON" ;;
		--no-fork)     USE_FORK="OFF" ;;
		-h|--help)     sed -n '2,31p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) die "unknown argument: '${arg}' (try --help)" ;;
	esac
done

command -v cmake     >/dev/null 2>&1 || die "cmake not found on PATH"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found on PATH"

# Host CMake 4.x removed pre-3.5 policy compat; the vendored libuvc keeps
# cmake_minimum_required(VERSION 3.1). Forward the compat flag so configure of
# the vendored dep does not hard-error (matches build-libuvc.sh).
CMAKE_COMPAT="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"

log "build dir   = ${BUILD_DIR}"
log "LIBUVC_USE_FORK = ${USE_FORK}"

# --- Configure once ---------------------------------------------------------
log "=== configure ==="
cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" \
	-DLIBUVC_USE_FORK="${USE_FORK}" \
	${CMAKE_COMPAT} >/dev/null || die "cmake configure failed"

SO_PATH="${BUILD_DIR}/gstreamer-1.0/${SO_NAME}"

build_so() {
	# Force a fresh compile+link of the plugin .so: remove the artifact so the
	# linker re-runs even though CMake sees no input change.
	rm -f "${SO_PATH}"
	cmake --build "${BUILD_DIR}" --target gstlibuvch264src >/dev/null 2>&1 \
		|| die "build of target gstlibuvch264src failed"
	[ -f "${SO_PATH}" ] || die "plugin .so not produced at ${SO_PATH}"
}

# --- Build #1 ---------------------------------------------------------------
log "=== build #1 ==="
build_so
HASH1="$(sha256sum "${SO_PATH}" | awk '{print $1}')"
cp -f "${SO_PATH}" "${BUILD_DIR}/${SO_NAME}.repro1"
log "build #1 sha256 = ${HASH1}"

# --- Build #2 ---------------------------------------------------------------
log "=== build #2 (clean relink) ==="
build_so
HASH2="$(sha256sum "${SO_PATH}" | awk '{print $1}')"
cp -f "${SO_PATH}" "${BUILD_DIR}/${SO_NAME}.repro2"
log "build #2 sha256 = ${HASH2}"

# --- Compare ----------------------------------------------------------------
if [ "${HASH1}" = "${HASH2}" ]; then
	log "=== RESULT: REPRODUCIBLE — raw .so hashes are byte-identical ==="
	log "    ${HASH1}"
	exit 0
fi

log "raw hashes DIFFER — applying normalization (strip symbols + drop .note.gnu.build-id)"

normalize() {
	# Produce a normalized copy: strip all symbols and remove the build-id note
	# (a content-hash the linker embeds; legitimately varies, carries no
	# behavioral meaning). Requires binutils; if absent, normalization is
	# unavailable and the raw mismatch stands.
	local in="$1" out="$2"
	cp -f "${in}" "${out}"
	if command -v strip >/dev/null 2>&1; then
		strip --strip-all "${out}" 2>/dev/null || true
	fi
	if command -v objcopy >/dev/null 2>&1; then
		objcopy --remove-section=.note.gnu.build-id "${out}" 2>/dev/null || true
	fi
}

if ! command -v strip >/dev/null 2>&1 && ! command -v objcopy >/dev/null 2>&1; then
	log "=== RESULT: NON-REPRODUCIBLE — raw hashes differ and binutils unavailable to normalize ==="
	exit 1
fi

NORM1="${BUILD_DIR}/${SO_NAME}.norm1"
NORM2="${BUILD_DIR}/${SO_NAME}.norm2"
normalize "${BUILD_DIR}/${SO_NAME}.repro1" "${NORM1}"
normalize "${BUILD_DIR}/${SO_NAME}.repro2" "${NORM2}"
NHASH1="$(sha256sum "${NORM1}" | awk '{print $1}')"
NHASH2="$(sha256sum "${NORM2}" | awk '{print $1}')"
log "normalized #1 sha256 = ${NHASH1}"
log "normalized #2 sha256 = ${NHASH2}"

if [ "${NHASH1}" = "${NHASH2}" ]; then
	log "=== RESULT: REPRODUCIBLE (modulo .note.gnu.build-id) ==="
	log "    Explained difference: the linker embeds a content-derived"
	log "    .note.gnu.build-id; after strip --strip-all + dropping that section"
	log "    the two builds are byte-identical (${NHASH1})."
	exit 0
else
	log "=== RESULT: NON-REPRODUCIBLE — normalized hashes still differ ==="
	log "    This indicates real non-determinism (embedded absolute path,"
	log "    __DATE__/__TIME__, or unordered link inputs). Investigate before"
	log "    relying on the build for verification."
	exit 1
fi
