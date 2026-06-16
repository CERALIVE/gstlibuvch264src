# ADR: Migrate from build-time patches to CeraLive/libuvc fork

**Status:** ACCEPTED  
**Date:** 2026-06-15  
**Authors:** CeraLive  
**Supersedes:** `patches/README.md` (patch-at-build-time workflow)

---

## Decision Fields

| Field | Value |
|-------|-------|
| `fork-visibility` | **PUBLIC** |
| `scope` | **MIGRATE-NOW** (Tasks 11/17/22/23 execute this plan) |
| `upstream-sync` | **NONE — HARD DIVERGENCE** |

**fork-visibility: PUBLIC** means no CI credential handling is required for Task 23. The fork repo will be publicly readable; `FetchContent` and Dockerfile can clone it without authentication.

**scope: MIGRATE-NOW** means Tasks 11/17/22/23 are gated on this ADR. Task 11 creates the fork and writes the real URL back. Tasks 17 and 22 update the build files. Task 23 validates the full pipeline. None of those tasks may hardcode a fork URL before Task 11 completes.

**upstream-sync: NONE — HARD DIVERGENCE** means CeraLive/libuvc is the canonical source going forward. No tracking branch, no periodic rebase onto upstream, no backport pulls. The upstream repo (`libuvc/libuvc`) has had 2 commits in the 15 months since v0.0.7; it is effectively moribund. CeraLive owns all future fixes. The tradeoff is accepted: we gain a stable, auditable base at the cost of carrying divergence permanently.

---

## Context

`gstlibuvch264src` depends on libuvc for UVC device access. The current build applies two patch files at build time:

- `patches/uvc15-support.patch` (two hunks)
- `patches/libuvc-h265-support.patch` (one hunk)

Both the `Dockerfile` and the top-level `CMakeLists.txt` fetch libuvc at a pinned SHA and apply these patches before compiling. This works, but it has costs:

- Patches are opaque blobs with no per-change history or rationale.
- Every consumer of the build must re-apply patches; there's no shared pre-patched artifact.
- The `auto_detach_kernel_driver` change is unconditional ON, which is opinionated and blocks making it configurable without touching the patch file.
- CI must carry patch tooling (`patch(1)`) and re-run it on every build.

A fork eliminates all of these: patches become commits with messages, the fork is the source of truth, and Task 11 can add a CMake option to make `auto_detach_kernel_driver` configurable without touching a patch file.

---

## Fork Base

**Exact SHA:** `68d07a00e11d1944e27b7295ee69673239c00b4b`

This is the commit that the upstream `v0.0.7` tag pointed to at the time of adoption. Tags are mutable; the SHA is not. All build files (Dockerfile line 41, CMakeLists.txt line 57) already pin this SHA. The fork branches from this exact commit.

Do not use upstream `main` or re-tag from a later upstream commit. The fork base is frozen.

---

## Fork URL (filled by Task 11)

- **Fork URL:** `https://github.com/CeraLive/libuvc`
- **Clone (HTTPS):** `https://github.com/CeraLive/libuvc.git`
- **Visibility:** PUBLIC (no auth required to clone)
- **Default branch:** `main`
- **Release tag:** `ceralive-v0.0.7.1` (base of the hardening branch; no new tag cut yet)
- **Hardening branch:** `harden/2026.6`
- **Tag/HEAD commit SHA:** `90cc67993047cd61d0b0a6c5bb62c4d61125cf80`
- **Base SHA (provenance only):** `68d07a00e11d1944e27b7295ee69673239c00b4b` — confirmed ancestor of HEAD.

**Pin downstream builds by SHA**, not by branch or tag name (tags/branches are mutable):

```
GIT_REPOSITORY https://github.com/CeraLive/libuvc.git
GIT_TAG        90cc67993047cd61d0b0a6c5bb62c4d61125cf80   # harden/2026.6 (was ceralive-v0.0.7.1 @ 21bc89ab)
```

Commit history on the fork (base → HEAD):

| SHA | Commit |
|-----|--------|
| `68d07a00` | `version 0.0.7` (upstream base, provenance only) |
| `2f32812`  | `feat(uvc): add UVC 1.5 support and configurable auto-detach kernel driver` |
| `d460f97`  | `feat(uvc): add H.265/HEVC format support` |
| `21bc89a`  | `docs(ceralive): record fork provenance and document auto-detach option` (tag `ceralive-v0.0.7.1`) |
| `26ec74a`  | `fix(stream): accept smaller max payloads than requested (backport upstream 047920b)` |
| `90cc679`  | `fix(security): guard uvc_scan_streaming NULL-deref (CVE-2026-1991) + backport e001f04` (current pin, branch `harden/2026.6`) |

The `LIBUVC_AUTO_DETACH_KERNEL_DRIVER` CMake option (Hunk A caveat) is implemented
in commit `2f32812` (default `ON`); the UVC 1.5 header (Hunk B) and H.265 support
(Hunk C) are unconditional. `LICENSE.txt` is byte-identical to the base (BSD-3-Clause
preserved verbatim). Standalone build verified (`cmake . && make`).

Evidence: `.omo/evidence/task-11-fork-build.txt`, `.omo/evidence/task-11-license.txt`.

Tasks 17/22/23 may now pin the SHA above.

---

## Patches as Commits

Each patch hunk becomes one commit on the fork, in application order. Commit messages carry the rationale that the patch files currently lack.

### Hunk A — `uvc15-support.patch` hunk 1: auto-detach kernel driver

**File:** `src/device.c`, function `uvc_wrap()`  
**Change:** Calls `libusb_set_auto_detach_kernel_driver(usb_devh, 1)` immediately after the handle is validated.

**Rationale:** Without this, `uvc_open_internal` fails when the `uvcvideo` kernel driver is already bound to the device's UVC interfaces. The libusb auto-detach API (available since libusb 1.0.20) detaches the kernel driver when an interface is claimed and re-attaches it on release. This is a portable, userspace-only fix that doesn't require `CAP_SYS_ADMIN` or manual `libusb_detach_kernel_driver` calls.

**Upstream candidacy:** Strong. This is a general correctness fix for any libuvc consumer on Linux. Recommended for upstreaming later; actually submitting is out of scope for this task.

**Caveat / Task 11 action:** The current patch applies this unconditionally (always ON). That's opinionated: some consumers may want to manage kernel driver detach themselves. Task 11 must make this configurable via a CMake option (e.g., `LIBUVC_AUTO_DETACH_KERNEL_DRIVER`, default `ON`) so the behavior is explicit and overridable. The fork commit for this hunk should implement the configurable form, not the unconditional form from the patch file.

### Hunk B — `uvc15-support.patch` hunk 2: accept UVC 1.5 headers

**File:** `src/device.c`, function `uvc_parse_vc_header()`  
**Change:** Adds `case 0x0150: break;` to the `bcdUVC` switch, alongside the existing `0x0100` and `0x0110` cases.

**Rationale:** Newer UVC devices (including some DJI action cameras) report `bcdUVC == 0x0150` in their VideoControl interface descriptor. Stock libuvc rejects these with `UVC_ERROR_NOT_SUPPORTED`. This is a pure additive parser fix with no behavioral side effects for existing devices.

**Upstream candidacy:** Strong. Pure additive, no regressions possible. Recommended for upstreaming later; out of scope here.

**Caveat:** None. Apply unconditionally in the fork commit.

### Hunk C — `libuvc-h265-support.patch`: HEVC format support

**Files:** `include/libuvc/libuvc.h`, `src/stream.c`  
**Changes (four additions):**
1. `UVC_FRAME_FORMAT_H265` added to `enum uvc_frame_format` in the header.
2. `UVC_FRAME_FORMAT_H265` added to the `UVC_FRAME_FORMAT_COMPRESSED` abstract group (count bumped from 2 to 3).
3. `FMT(UVC_FRAME_FORMAT_H265, {'H','2','6','5',...})` entry added to the format table in `src/stream.c`.
4. `case UVC_FRAME_FORMAT_H265: frame->step = 0; break;` added to `_uvc_populate_frame()`.

**Rationale:** `gstlibuvch264src` supports both H.264 and H.265 UVC streams (dual-codec, `libuvch26xsrc` alias). Without this patch, libuvc has no H.265 format enum, no GUID registration, and no `frame->step` handling, so H.265 frames can't flow through the pipeline at all. This change was ported from upstream BELABOX and is required for `InputKind::UvcH265` in cerastream.

**Upstream candidacy:** Reasonable. Additive, no regressions. The HEVC GUID used here should be verified against the UVC specification before submitting upstream. Recommended for upstreaming later; out of scope here.

**Caveat:** None. Apply unconditionally in the fork commit.

---

## Rollback Mechanism

The current fetch-and-patch path is preserved as a CMake fallback via the `-DLIBUVC_USE_FORK` option.

When `LIBUVC_USE_FORK=ON` (the new default after Task 17), `FetchContent` points at the fork URL.  
When `LIBUVC_USE_FORK=OFF`, `FetchContent` falls back to the upstream SHA + patch commands, exactly as today:

```cmake
FetchContent_Declare(libuvc
  GIT_REPOSITORY https://github.com/libuvc/libuvc.git
  GIT_TAG 68d07a00e11d1944e27b7295ee69673239c00b4b
  PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/patches/uvc15-support.patch
    && patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/patches/libuvc-h265-support.patch
  UPDATE_DISCONNECTED 1
)
```

The Dockerfile gets a parallel `ARG LIBUVC_USE_FORK=1` guard (Task 22). Setting `LIBUVC_USE_FORK=0` in the Docker build reverts to the current fetch+patch path.

The `patches/` directory and both patch files are **not deleted** until the fork is confirmed stable in CI (Task 23 sign-off). They remain as the rollback artifact.

---

## License

libuvc is BSD-3-Clause. The fork must preserve the original `LICENSE` file and all copyright notices verbatim. CeraLive additions are also BSD-3-Clause. No license change is permitted.

---

## CI Guard Requirements

Task 23 must verify:

1. **SHA pinning:** The fork's default branch HEAD must be pinned in build files by commit SHA, not by branch name. Branch names are mutable.
2. **Reproducibility:** Two independent builds from the same SHA must produce byte-identical libuvc shared objects (modulo build timestamps embedded by the toolchain, if any).
3. **Fallback path:** A CI job with `-DLIBUVC_USE_FORK=OFF` must build and pass the full test suite, confirming the patch-based fallback still works.
4. **No credential requirement:** Because the fork is PUBLIC, no `GIT_CREDENTIALS` or deploy keys are needed in CI. Task 23 must confirm the clone succeeds without authentication.
5. **Test suite green:** All existing ctest targets must pass against the fork-sourced libuvc, including sanitizer variants.

---

## Reproducibility Requirements

- The fork's commit history is append-only after Task 11 creates it. No force-pushes to the branch used by build files.
- The SHA recorded in `CMakeLists.txt` and `Dockerfile` after Tasks 17/22 must match the fork commit that carries all three hunks as commits.
- The `patches/` directory remains in the repo as a human-readable record of what changed, even after the fork path is the default.

---

## Divergence Tradeoff

CeraLive owns ALL future fixes to libuvc. There is no upstream-backport path. If upstream libuvc ships a fix we need, we cherry-pick it manually into the fork. If upstream ships a breaking change, we ignore it.

This is acceptable because:
- Upstream libuvc has had 2 commits in 15 months since v0.0.7. It's effectively unmaintained.
- The three changes in this ADR cover the full set of modifications we need. The surface area is small.
- A fork gives us a stable, auditable, CI-tested base with no surprise upstream changes.

---

## Task Gate

This ADR is the gate for the following tasks. None may proceed until this document is merged:

| Task | Blocked action |
|------|---------------|
| Task 11 | Create `CeraLive/libuvc` fork repo; write real URL back to this ADR |
| Task 17 | Update `CMakeLists.txt` to use fork URL with `LIBUVC_USE_FORK` guard |
| Task 22 | Update `Dockerfile` to use fork URL with `LIBUVC_USE_FORK` guard |
| Task 23 | CI validation: fork clone, reproducibility, fallback path, test suite |

Tasks 11/17/22/23 must not hardcode a fork URL until Task 11 writes it back to the placeholder above.
