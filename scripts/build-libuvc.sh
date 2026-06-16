#!/usr/bin/env bash
#
# build-libuvc.sh — single source of truth for the libuvc dependency.
#
# Both the Dockerfile (production image) and the top-level CMakeLists.txt (test
# build) call THIS script so the pinned SHAs, the CMake options, and the
# (fallback) patch steps live in exactly one place. Do not duplicate the
# fetch/patch/build logic anywhere else — change it here.
#
# Two modes (see libuvch264src/docs/notes/libuvc-fork-adr.md):
#
#   fork     (default): clone the CeraLive/libuvc fork at the pinned
#            ceralive-v0.0.7.1 SHA. The UVC 1.5 / H.265 / configurable
#            auto-detach changes are commits on the fork, so NO patch(1) step
#            is needed.
#   upstream (rollback): clone upstream libuvc v0.0.7 at its pinned SHA and
#            apply the patches from patches/ (the pre-fork path). Selected by
#            --mode=upstream or LIBUVC_USE_FORK=OFF/0.
#
# Mode resolution precedence: --mode=<m>  >  LIBUVC_USE_FORK env  >  default fork.
#
# Usage:
#   scripts/build-libuvc.sh [--mode=fork|upstream] [--prefix=<dir>]
#                           [--src-dir=<dir>] [--jobs=<n>]
#                           [--no-install] [--ldconfig]
#
#   --mode=<m>      fork (default) or upstream+patches fallback.
#   --prefix=<dir>  install prefix (default /usr/local). Produces
#                   <prefix>/lib/libuvc.so* and <prefix>/include/libuvc/*.h.
#   --src-dir=<dir> checkout/build directory (default: a fresh mktemp dir).
#   --jobs=<n>      parallel make jobs (default: nproc).
#   --no-install    build only; skip `make install`.
#   --ldconfig      run ldconfig after install (needs root; for the Docker image).
#
set -euo pipefail

# --- Pinned coordinates — the ONLY place these SHAs/URLs live ----------------
FORK_URL="https://github.com/CeraLive/libuvc.git"
FORK_SHA="eae7f49c2978b6cdb21edc61fde006195588fec7"      # main (hardened: CVE-2026-1991 guard + 047920b + e001f04; rebased onto main from harden/2026.6, tag ceralive-v0.0.7.2)
UPSTREAM_URL="https://github.com/libuvc/libuvc.git"
UPSTREAM_SHA="68d07a00e11d1944e27b7295ee69673239c00b4b"  # v0.0.7 base

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PATCH_DIR="${REPO_ROOT}/patches"

log() { printf '[build-libuvc] %s\n' "$*"; }
die() { printf '[build-libuvc] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
	sed -n '3,34p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# --- Defaults ----------------------------------------------------------------
MODE=""
PREFIX="/usr/local"
SRC_DIR=""
JOBS="$(nproc 2>/dev/null || echo 2)"
DO_INSTALL=1
RUN_LDCONFIG=0

# LIBUVC_USE_FORK env contract (CMake passes ON/OFF, Docker passes 1/0). Maps to
# a mode that an explicit --mode can still override below.
case "${LIBUVC_USE_FORK:-}" in
	0|OFF|off|false|FALSE|no|NO)   ENV_MODE="upstream" ;;
	1|ON|on|true|TRUE|yes|YES)     ENV_MODE="fork" ;;
	"")                            ENV_MODE="" ;;
	*) die "LIBUVC_USE_FORK has unrecognised value '${LIBUVC_USE_FORK}' (expected 1/0/ON/OFF)";;
esac

# --- Parse args --------------------------------------------------------------
for arg in "$@"; do
	case "$arg" in
		--mode=*)     MODE="${arg#*=}" ;;
		--prefix=*)   PREFIX="${arg#*=}" ;;
		--src-dir=*)  SRC_DIR="${arg#*=}" ;;
		--jobs=*)     JOBS="${arg#*=}" ;;
		--no-install) DO_INSTALL=0 ;;
		--ldconfig)   RUN_LDCONFIG=1 ;;
		-h|--help)    usage; exit 0 ;;
		*) die "unknown argument: '${arg}' (try --help)" ;;
	esac
done

# Precedence: explicit --mode > LIBUVC_USE_FORK env > default fork.
if [ -z "$MODE" ]; then
	MODE="${ENV_MODE:-fork}"
fi

case "$MODE" in
	fork)     URL="$FORK_URL";     SHA="$FORK_SHA";     APPLY_PATCHES=0 ;;
	upstream) URL="$UPSTREAM_URL"; SHA="$UPSTREAM_SHA"; APPLY_PATCHES=1 ;;
	*) die "invalid --mode='${MODE}' (expected 'fork' or 'upstream')" ;;
esac

if [ -z "$SRC_DIR" ]; then
	SRC_DIR="$(mktemp -d)/libuvc"
fi

command -v git   >/dev/null 2>&1 || die "git not found on PATH"
command -v cmake >/dev/null 2>&1 || die "cmake not found on PATH"
if [ "$APPLY_PATCHES" -eq 1 ]; then
	command -v patch >/dev/null 2>&1 || die "patch not found on PATH (needed for --mode=upstream)"
	[ -d "$PATCH_DIR" ] || die "patches directory not found: ${PATCH_DIR}"
fi

log "mode=${MODE} prefix=${PREFIX} src=${SRC_DIR} jobs=${JOBS}"
log "source ${URL} @ ${SHA}"

# --- Fetch (SHA-pinned; a bare SHA cannot go to `git clone --branch`, so use
#     the git init + fetch --depth 1 + checkout FETCH_HEAD idiom — it avoids the
#     detached-HEAD and mutable-tag traps). Idempotent: a re-run force-resets the
#     tree to the pinned commit so a prior patch application is discarded. ------
mkdir -p "$SRC_DIR"
cd "$SRC_DIR"
if [ ! -d .git ]; then
	git init -q .
fi
if git remote get-url origin >/dev/null 2>&1; then
	git remote set-url origin "$URL"
else
	git remote add origin "$URL"
fi
git fetch --depth 1 origin "$SHA"
git checkout -q -f FETCH_HEAD

# --- Patches (upstream / fallback only) -------------------------------------
if [ "$APPLY_PATCHES" -eq 1 ]; then
	for p in uvc15-support.patch libuvc-h265-support.patch cve-2026-1991-scan-streaming-nullguard.patch; do
		patch_file="${PATCH_DIR}/${p}"
		[ -f "$patch_file" ] || die "patch file missing: ${patch_file}"
		if patch -p1 --dry-run -R <"$patch_file" >/dev/null 2>&1; then
			log "patch already applied, skipping: ${p}"
		else
			log "applying patch: ${p}"
			patch -p1 <"$patch_file"
		fi
	done
fi

# --- Configure + build + install --------------------------------------------
# CMAKE_POLICY_VERSION_MINIMUM=3.5: both the fork and upstream v0.0.7 keep
# cmake_minimum_required(VERSION 3.1), which CMake 4.x hard-errors on without
# this. CMAKE_BUILD_TYPE=Release matches the production image's optimisation.
cmake . \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DBUILD_EXAMPLE=OFF \
	-DBUILD_TEST=OFF \
	-DCMAKE_INSTALL_PREFIX="$PREFIX"
make -j"$JOBS"

if [ "$DO_INSTALL" -eq 1 ]; then
	make install
	log "installed libuvc to ${PREFIX}"
fi
if [ "$RUN_LDCONFIG" -eq 1 ]; then
	ldconfig
fi

log "done (mode=${MODE})"
