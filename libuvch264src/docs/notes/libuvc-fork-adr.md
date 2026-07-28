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

> **CURRENT PIN (authoritative):** the build pins the fork at `main` commit
> `f3eda76` (libuvc PR #7 — device teardown). The `ceralive-v0.0.7.9` block is
> retained just below as the previous pin; the v0.0.7.2 block further down is the
> historical record of how the fork first reached a hardening release, and the
> v0.0.7.3 and v0.0.7.4→v0.0.7.8 addenda record the subsequent hardening wave and
> the five emergency UAF/lifetime hotfix rounds. This callout is the pin the CI
> guard (`scripts/check-libuvc-fork.sh`) reads.

- **Current release tag:** _none — pinned by SHA on `main`._ `f3eda76` is untagged; cut
  `ceralive-v0.0.7.10` on it when convenient and update this block. Pinning by SHA is
  already the rule here (tags are mutable), so the absence of a tag does not weaken the pin.
- **Tag/HEAD commit SHA:** `f3eda761b69acdfa6c0ffc02119b2bda172b9d46`
- **Base SHA (provenance only):** `68d07a00e11d1944e27b7295ee69673239c00b4b` — confirmed ancestor of HEAD.
- **Supersedes:** `ada082b5009e38a89eb7cd6176683b508cd99ff5` (`ceralive-v0.0.7.9`), which is an
  ancestor of this SHA — no work is dropped by moving the pin forward.

**Why this pin moved.** `uvc_close()` never stopped the VideoControl status interrupt
transfer and released only the control interface, so a close could leave the kernel
`uvcvideo` driver detached and the camera's `/dev/videoN` gone. That is one of the ways a
UVC device ends up **wedged** — enumerated and answering control transfers while
delivering nothing on its streaming endpoint (see AGENTS.md → DISCONNECT / RECONNECT
BEHAVIOR). Until this pin moved, a built `.deb` shipped the unfixed teardown even though
the fix was merged in the fork.

**Pin downstream builds by SHA** (the value `FORK_SHA` carries at `scripts/build-libuvc.sh:39`):

```
GIT_REPOSITORY https://github.com/CeraLive/libuvc.git
GIT_TAG        f3eda761b69acdfa6c0ffc02119b2bda172b9d46   # main, untagged (PR #7 device teardown)
```

### Previous pin — `ceralive-v0.0.7.9`

- **Release tag:** `ceralive-v0.0.7.9` (descriptor scanner length bounds on top of the UAF/lifetime hotfix rounds)
- **Tag/HEAD commit SHA:** `ada082b5009e38a89eb7cd6176683b508cd99ff5`

See the "v0.0.7.8 Addendum" section below for the full v0.0.7.4→v0.0.7.8
hotfix chain. The v0.0.7.2 record follows.

---

- **Fork URL:** `https://github.com/CeraLive/libuvc`
- **Clone (HTTPS):** `https://github.com/CeraLive/libuvc.git`
- **Visibility:** PUBLIC (no auth required to clone)
- **Default branch:** `main`
- **Release tag:** `ceralive-v0.0.7.2` (hardening release: CVE-2026-1991 guard + robustness backports)
- **Hardening branch:** `harden/2026.6` (rebase-merged into `main`)
- **Tag/HEAD commit SHA:** `eae7f49c2978b6cdb21edc61fde006195588fec7`
- **Base SHA (provenance only):** `68d07a00e11d1944e27b7295ee69673239c00b4b` — confirmed ancestor of HEAD.

**Pin downstream builds by SHA**, not by branch or tag name (tags/branches are mutable):

```
GIT_REPOSITORY https://github.com/CeraLive/libuvc.git
GIT_TAG        eae7f49c2978b6cdb21edc61fde006195588fec7   # main, tag ceralive-v0.0.7.2 (rebased from harden/2026.6; was 90cc679 pre-rebase)
```

Commit history on the fork (base → HEAD):

| SHA | Commit |
|-----|--------|
| `68d07a00` | `version 0.0.7` (upstream base, provenance only) |
| `2f32812`  | `feat(uvc): add UVC 1.5 support and configurable auto-detach kernel driver` |
| `d460f97`  | `feat(uvc): add H.265/HEVC format support` |
| `21bc89a`  | `docs(ceralive): record fork provenance and document auto-detach option` (tag `ceralive-v0.0.7.1`) |
| `f4af02a`  | `fix(stream): accept smaller max payloads than requested (backport upstream 047920b)` |
| `eae7f49`  | `fix(security): guard uvc_scan_streaming NULL-deref (CVE-2026-1991) + backport e001f04` (current pin on `main`, tag `ceralive-v0.0.7.2`; was `90cc679` on `harden/2026.6` pre-rebase) |

The `LIBUVC_AUTO_DETACH_KERNEL_DRIVER` CMake option (Hunk A caveat) is implemented
in commit `2f32812` (default `ON`); the UVC 1.5 header (Hunk B) and H.265 support
(Hunk C) are unconditional. `LICENSE.txt` is byte-identical to the base (BSD-3-Clause
preserved verbatim). Standalone build verified (`cmake . && make`).

Standalone build and license parity were verified before pinning this fork SHA.

Tasks 17/22/23 may now pin the SHA above.

---

## v0.0.7.3 Addendum (Task 9/13, 2026-07-02)

This section records the next release on top of `ceralive-v0.0.7.2` above. It
does not modify any row in the "Fork URL" or "Commit history" sections above;
those remain the historical record of how the fork reached `v0.0.7.2`.

- **New release tag:** `ceralive-v0.0.7.3` (hardening release: 8 additional robustness backports)
- **Tag/HEAD commit SHA:** `6210f2f64965af532440be357e6971b9b618797f`
- **Hardening branch:** `hardening/v0.0.7.3` (true fast-forward merge into `main`, so the merged main SHA, branch tip, and tag target are identical)
- **Current pin:** `scripts/build-libuvc.sh:39` `FORK_SHA` in `gstlibuvch264src`

```
GIT_REPOSITORY https://github.com/CeraLive/libuvc.git
GIT_TAG        6210f2f64965af532440be357e6971b9b618797f   # main, tag ceralive-v0.0.7.3
```

New commits landed on `hardening/v0.0.7.3` since `ceralive-v0.0.7.2` (`eae7f49`):

| SHA | Commit | Backlog ID(s) |
|-----|--------|-----|
| `3195bbc` | `fix(stream): retry alt-setting on failure; free metadata buf in stream_close (upstream #293, #295)` | A1, A3 |
| `001e8d3` | `feat(stream): runtime-configurable transfer buffer count; fail loudly on zero submitted transfers (#291)` | A2 |
| `5df5401` | `fix(device): repair degenerate frame descriptors (zero buffer size / bad default interval)` | A4 |
| `ab49e21` | `fix(stream): bounded wait in uvc_stream_stop; return TIMEOUT instead of hanging` | A5 |
| `69c7da8` | `fix(stream): zero GET_MAX payload fallback; corrupt/oversized payload guards (#277/#184/#212 + saki 9e95b8a)` | A7, A9 (A8 folded into A9) |
| `9874f4c` | `fix(device,stream): preserve VC-header dwClockFrequency (A12); A13 confirmed no-op` | A12 |
| `6210f2f` | `chore(release): finalize CHANGELOG for ceralive-v0.0.7.3` (doc-only) | — |

Skip-equivalent items in this wave (A6, A10, A11, A13) and the plugin-only
item (A14, a `gstlibuvch264src` quirk seam rather than a fork patch) are
detailed in `libuvch264src/docs/notes/camera-compat.md` §3, which is the
authoritative provenance table for this hardening wave. Ancestry gates
(`68d07a00e11d1944e27b7295ee69673239c00b4b` and `eae7f49` both ancestors of
`6210f2f`) hold; `LICENSE.txt` remains byte-identical to the base.

Full detail: see `libuvch264src/docs/notes/camera-compat.md` §3 and the fork's
`CHANGELOG.ceralive.md`.

---

## v0.0.7.8 Addendum (post-release UAF/lifetime hotfix rounds)

This section records five emergency hotfix rounds landed on `main` on top of
`ceralive-v0.0.7.3` (`6210f2f`). They were surfaced by an adversarial
code-quality review of the new bounded `uvc_stream_stop()`/A5 teardown path,
which caught a use-after-free chain reachable when a stop-timeout leaves an
event/handler thread alive while teardown proceeds. Each round is a single,
narrowly-scoped commit; none change default behavior. This addendum does not
modify the v0.0.7.2 or v0.0.7.3 records above.

- **Current release tag:** `ceralive-v0.0.7.8`
- **Tag/HEAD commit SHA:** `71588dbc23c5204e07c575c3b2ae6ac7ee9bf90d`
- **Current pin:** `scripts/build-libuvc.sh:39` `FORK_SHA` in `gstlibuvch264src`

```
GIT_REPOSITORY https://github.com/CeraLive/libuvc.git
GIT_TAG        71588dbc23c5204e07c575c3b2ae6ac7ee9bf90d   # main, tag ceralive-v0.0.7.8
```

Hotfix chain (v0.0.7.3 → v0.0.7.8), each on top of the previous:

| Tag | Fix |
|-----|-----|
| `ceralive-v0.0.7.4` | UAF hotfix: quarantine `strmh` on the A5 stop-timeout path in `uvc_stream_close` so a late transfer callback cannot deref freed stream state. |
| `ceralive-v0.0.7.5` | `devh` lifetime hotfix: quarantine `devh` in `uvc_close` so a late `LIBUSB_TRANSFER_COMPLETED` callback deref of `strmh->devh->is_isight` is lifetime-safe. |
| `ceralive-v0.0.7.6` | Context lifetime hotfix: quarantine `uvc_context` so `uvc_exit` skips `libusb_exit`/free and `uvc_open_internal` skips a duplicate handler thread while a stop-timeout event thread still runs on `ctx->usb_ctx`. |
| `ceralive-v0.0.7.7` | Handler-thread guard: gate `uvc_close`'s last-device `kill_handler_thread`/`pthread_join` on `!has_quarantined_device` so a normal close of a device reopened on a quarantined ctx never kills the surviving event thread. |
| `ceralive-v0.0.7.8` | Safe iteration: `uvc_exit` iterates `ctx->open_devices` with `DL_FOREACH_SAFE` so `uvc_close` freeing the current device does not leave the loop increment reading a freed node's `next` pointer (pre-existing general defect, reproducible with 2+ devices open, with or without quarantine). |

Ancestry gates (`68d07a00e11d1944e27b7295ee69673239c00b4b`, `eae7f49`, and
`6210f2f` all ancestors of `71588dbc23c5204e07c575c3b2ae6ac7ee9bf90d`) hold;
`LICENSE.txt` remains byte-identical to the base. Full per-round detail lives in
the fork's own `CHANGELOG.ceralive.md` and in
`libuvch264src/docs/notes/camera-compat.md`.

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
