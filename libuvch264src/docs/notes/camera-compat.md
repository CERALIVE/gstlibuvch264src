# Camera Compatibility Matrix

**Status:** informational, updated as devices get validated
**Date:** 2026-07-03
**Scope:** what `libuvch264src` can talk to today, how, and how confident we are about each device family

This note answers three questions a field technician or on-call engineer asks
when a camera doesn't stream: which mechanism does this device use, what
commands narrow down the fault, and which fork fix (if any) already covers
the failure mode. It does not promise support for hardware nobody has tested.
Where we say "unvalidated," read that literally: the mechanism should work
per the UVC spec, but no CeraLive test hardware has confirmed it.

---

## 1. Mechanism-Per-Family Matrix

| Family | Mechanism | Status | Notes |
|--------|-----------|--------|-------|
| **DJI Osmo / Action series** | Frame-based UVC H.264/H.265 (`UVC_VS_FRAME_FRAME_BASED`), with historically degenerate frame descriptors (zero `dwMaxVideoFrameBufferSize`, bad `dwDefaultFrameInterval`) | **Supported** | Primary target device family. The degenerate-descriptor problem is now repaired in the fork (A4, `5df5401`); see §3. Confirmed working via `test_mock_smoke`/`test_negotiate` against a DJI-shaped descriptor set and, per plan history, real hardware validation upstream of this todo. |
| **Insta360 X3 / X4 / Ace Pro** | Webcam-mode UVC with on-camera H.264/H.265 codec select (device switches into a UVC-compliant mode via its own menu/app) | **Expected-compatible / unvalidated** | These cameras expose a standard UVC interface once switched into webcam mode, so the same `negotiate()` path that finds DJI's H.264/H.265 format descriptors should find theirs too. CeraLive has **no Insta360 test hardware**. Treat this as "should work per the UVC descriptors these devices are documented to expose," not as a tested claim. Field-triage with the `GST_DEBUG` incantation in §2 before assuming it's broken; if the inventory shows an H264/H265 `uvc_format_desc_t`, the negotiate path should pick it up the same way it picks up DJI's. |
| **Logitech C920-era webcams** | XU-controlled (extension-unit) H.264 encode, not a plain UVC frame-based format descriptor | **Unsupported by design** | These webcams encode H.264 through vendor extension-unit (XU) controls rather than exposing an `H264`/`H265` `uvc_format_desc_t`. `negotiate()` only walks standard format descriptors (`gst_libuvc_h264_negotiate()`, `gstlibuvch264src.c:437`) and has no XU probing path, so it will report "device exposes no H264/H265 format" (see the `dji-xu-investigation.md` note for the related DJI XU control research, which found the same class of limitation). This is a scope decision, not a bug: adding XU-based H.264 extraction would be a substantial new mechanism, not a compat fix. |
| **GoPro (HERO-series, non-UVC-webcam models)** | Non-UVC, HTTP-over-USB (GoPro's own "USB webcam mode" and control API run over a network-over-USB gadget interface, not USB Video Class) | **Out of scope** | `libuvch264src` is a `libuvc`-backed UVC source element. A device that never enumerates as a UVC Video Streaming interface is invisible to `uvc_find_devices()`/`uvc_get_device_list()` regardless of anything this plugin does. Some GoPro models do expose a genuine UVC webcam mode; if a specific unit does, it falls under the same "expected-compatible/unvalidated" bucket as Insta360 above, not this row. |
| **HDMI capture sticks (generic UVC-HDMI-to-USB dongles)** | Raw/uncompressed UVC formats (YUY2, NV12, MJPEG); no on-device H.264/H.265 encode | **Out of scope (path bypasses element entirely)** | Per the parent manifest and this repo's own AGENTS.md ("HDMI capture paths bypass this element entirely"), HDMI capture is handled by a different source element (`v4l2src` or similar) elsewhere in the cerastream pipeline. Even if such a dongle enumerates over `libuvc`, `negotiate()` requires an H264/H265 format descriptor and will reject a raw-format-only device the same way it rejects Logitech's XU-only devices. |
| **PTZ webcams (UVC+V4L2/XU pan-tilt-zoom units)** | Standard UVC with a Camera Terminal / Processing Unit PTZ control surface | **Expected-compatible for streaming; PTZ properties apply where the device exposes the controls** | If a PTZ webcam also exposes an H264/H265 format descriptor, streaming follows the same path as any other UVC H.264/H.265 device. The `pan`/`tilt`/`zoom` properties and the `set-ptz` action signal are capability-gated (see AGENTS.md PTZ CONTROL SURFACE): a set on an axis the device doesn't report is silently ignored, so pointing this element at a PTZ webcam that lacks H.264/H.265 output will still let PTZ controls work over the opt-in control socket even though the media path won't negotiate. No dedicated PTZ webcam test hardware; treat streaming support as unvalidated the same way as Insta360, while the PTZ control surface itself is unit-tested against the mock (`test_ptz.c`, `test_socket.c`) independent of any specific camera model. |

**Reading the status column:** "Supported" means real or mock-validated end-to-end. "Expected-compatible/unvalidated" means the mechanism matches what the element already handles, but nobody has run it against that hardware. "Unsupported by design" and "out of scope" mean the element's architecture (UVC frame-based format descriptors only, no XU probing, no HTTP-over-USB) cannot reach that device class without new code, not that a bug is blocking it.

---

## 2. Field-Triage Steps

Work top-down. Each step narrows the fault before you touch code.

### Step 1: confirm the device is on the USB bus and enumerates as UVC

```bash
lsusb
```

If the camera doesn't show up here, this is a USB/cabling/power problem, not a plugin problem.

### Step 2: capture the negotiation-diagnostics descriptor inventory

When `negotiate()` finds no H264/H265 format descriptor, it logs a full
inventory of every format and frame descriptor the device DID advertise
(fourcc, GUID, resolution, frame-interval range) immediately before posting
the bus error. This is the single most useful piece of field-triage data for
an unfamiliar camera: it tells you exactly what the device offered instead
of the codec you expected.

```bash
GST_DEBUG=libuvch264src:3 gst-launch-1.0 libuvch264src index=0 ! fakesink 2>&1 | grep -A2 "format fourcc"
```

`libuvch264src:3` selects up through the FIXME level for this element's debug
category only (ERROR, WARNING, and FIXME lines), which is enough to see both
the inventory warnings and the resulting bus error without the volume of a
full `:5` DEBUG trace. Raise to `libuvch264src:5` if you need the INFO-level
caps-negotiation trace as well (`caps of src`, `caps of peer`, `caps
intersection`).

Sample output shape (see `gst_libuvc_h264_src_log_format_inventory()`,
`gstlibuvch264src.c:414`):

```
WARNING ... device exposes no H264/H265 format; advertised formats follow:
WARNING ...   format fourcc 'MJPG' guid ...
WARNING ...     1920x1080 frame interval [333333..333333] (100ns units)
WARNING ...   format fourcc 'YUY2' guid ...
WARNING ...     1280x720 frame interval [333333..666666] (100ns units)
```

If every logged fourcc is `MJPG`/`YUY2`/`NV12`/etc. with no `H264`/`H265`
entry, the device genuinely doesn't expose a UVC-native H.264/H.265 format
descriptor. That matches the Logitech/HDMI-dongle/raw-format rows in §1;
it is not something a plugin-side fix can repair, because there's no format
descriptor to select.

### Step 3: try the other `index` selector forms

`index=0` (ordinal) picks whatever `libuvc` enumerates first, which is
unreliable on a multi-camera bus. Narrow the selection:

```bash
# By USB vendor:product ID (hex)
gst-launch-1.0 libuvch264src index="1234:5678" ! fakesink

# By USB serial number (exact string match)
gst-launch-1.0 libuvch264src index="serial:CAM-001" ! fakesink

# By USB bus and device address (decimal)
gst-launch-1.0 libuvch264src index="bus:1:5" ! fakesink
```

A malformed selector fails `start()` loudly with `RESOURCE/SETTINGS` rather
than silently falling back to device 0 (see AGENTS.md PROPERTIES). `vid:pid`
and `serial:` selectors survive a replug; `bus:` and ordinal selectors may
resolve to a different physical device after one.

### Step 4: if the device drops out mid-stream, enable `reconnect`

A camera that streams fine at start but disappears after a few
seconds/minutes (loose USB connection, power-save on a hub, thermal
shutdown) is a disconnect, not a negotiation failure. Confirm by watching
for `RESOURCE/READ` on the bus with `reconnect` left at its default `false`,
then opt into auto-reconnect:

```bash
gst-launch-1.0 libuvch264src reconnect=true index="serial:CAM-001" ! video/x-h264 ! fakesink
```

See AGENTS.md DISCONNECT / RECONNECT BEHAVIOR for the exact detection window
(~5 s of silence) and backoff schedule (1, 2, 4, 8, 16 s, five attempts).

### Step 5: check for a vid:pid quirk match

The element carries a vid:pid quirk table (`libuvch264src/src/quirks.{c,h}`) with
two flags:

- `QUIRK_DOUBLE_PROBE` — for cameras that need
  `uvc_get_stream_ctrl_format_size()` called twice before the negotiated format
  sticks (libuvc upstream issue #242).
- `QUIRK_MAX_PIXEL_RATE` — for cameras that advertise frame intervals they
  cannot deliver. The row carries the highest `width x height x fps` PROVEN to
  stream, and negotiation drops every advertised rate above it.

The table currently holds **one** row: the DJI Osmo Pocket 3 (`2ca3:0023`), which
sets **both** flags, capped at `1920x1080x30` = 62 208 000 px/s.

**Why it needs the double probe (the "camera is not detected" symptom).**
`uvc_probe_stream_ctrl()` SET_CURs the control it wants, GET_CURs it back, and
rejects the mode if the readback disagrees. The Osmo answers that first GET_CUR
from the mode it PREVIOUSLY committed, so a negotiation asking for a mode LARGER
than the currently committed one fails with `Unable to get stream control:
Invalid mode` about 170 ms into `start()`. Measured on hardware
(`192.168.78.131`, 2026-07-30), one probe versus two:

| requested transition | 1 probe | 2 probes |
|---|---|---|
| `1280x720@30` → `1920x1080@30` | 3/3 **FAIL** | 3/3 pass |
| `1920x1080@30` → `3840x2160@60` | 20/20 **FAIL** | 4/4 pass |
| same mode again, or a smaller one | pass | pass |

Deterministic and direction-specific — 23/23 on a mode increase, never
otherwise. The `720p → 1080p` row matters most: the element's own shipping mode
is affected, so this is not a 4K-only concern. It presents as intermittent in the
field only because whether it fires depends on what mode the device last
committed.

**Why it also carries a pixel-rate cap.** Its H.264 descriptor advertises
3840x2160 at 60/50/48 fps, and `negotiate()` prefers the largest area at the
highest fps, so 4K@60 was selected by construction. Those rates were originally
recorded negotiating cleanly and then delivering **zero** frames, with the 5 s
silence watchdog reporting a disconnect that never happened.

> **The zero-frame premise did NOT reproduce on 2026-07-30.** On libuvc `4868e57`
> the unquirked binary negotiated 4K@60 and delivered real, sustained 4K —
> 600 access units in 10.6 s (~56 fps) twice over, with `h264parse` reading
> `3840x2160`, `high` profile, level `5.2` straight out of the SPS. So the cap's
> stated justification is not currently observable, and the cap costs the
> operator 4K. It is deliberately left in place pending an owner decision,
> because the original zero-frame observation was real and was never explained
> (one candidate: the same stale-readback defect can also leave a bound-but-silent
> stream when the readback happens to compare equal). Raising it is the one-number
> change described in `quirks.c`; do not make it on the strength of this note
> alone.

See `quirks.c` for the full evidence and for how to raise the cap.

If you find another camera that needs either workaround, that's a signal to add a
table entry — not something the field-triage steps above can toggle at the
command line today.

---

## 3. Fork-Backport Provenance Table

The CeraLive `libuvc` fork (tag `ceralive-v0.0.7.9`, SHA
`ada082b5009e38a89eb7cd6176683b508cd99ff5`) carries a tiered backlog of
robustness backports, audited item-by-item against the fork's pre-hardening
state and finalized in the fork's `CHANGELOG.ceralive.md`. Each backlog ID
(A1-A14) maps to either a landed fork commit or a skip-equivalent reason.

| ID | Source | Verdict | Fork commit | What it fixes / why it's skipped |
|----|--------|---------|--------------|-----------------------------------|
| A1 | upstream PR #293 | pick | `3195bbc` (shared with A3) | Retries `libusb_set_interface_alt_setting` up to 3 times on failure instead of failing on the first transient error. Relevant to any device that flakes on interface claim, DJI included. |
| A2 | upstream PR #291 (adapted API) | adapt + pick | `001e8d3` | Runtime-configurable USB transfer-buffer count (`uvc_set_transfer_buffers()`, devh-level, not PR #291's global setter) and a fail-loud fix for a bug where zero submitted transfers silently reported success. Backs the plugin's `transfer-buffers` property (§4 below). |
| A3 | upstream PR #295 (identical to #275, #295 picked) | pick | `3195bbc` (shared with A1) | Frees `frame.metadata` in `uvc_stream_close()`, closing a per-stream-close leak. |
| A4 | saki4510t `328d14d` (adapted scope) | pick | `5df5401` | Repairs degenerate frame descriptors: zero `dwMaxVideoFrameBufferSize`, bad/zero `dwDefaultFrameInterval`. **This is the DJI fix.** DJI's H.264/H.265 streams are frame-based (`uvc_parse_vs_frame_frame`, not the uncompressed parser saki's original patch targeted), so the fork extends the repair to both parsers via a shared helper. Guards strictly on the zero/degenerate case; sane descriptors are untouched. |
| A5 | pupil-labs `c534e3d` + upstream PR #59 (bounded-wait half only) | adapt + pick | `ab49e21` | Replaces `uvc_stream_stop()`'s unbounded `pthread_cond_wait` with a bounded `pthread_cond_timedwait` (5 attempts, ~1 s each), returning `UVC_ERROR_TIMEOUT` instead of hanging forever on a device that never completes its transfer cancellation. |
| A6 | pupil-labs `9004351` | skip-equivalent | none (already in v0.0.7 base) | Composite-device control-interface routing (real `bInterfaceNumber` instead of hardcoded 0) was already present in the fork's upstream base; nothing to backport. |
| A7 | upstream PR #277 | pick (shared commit with A9) | `69c7da8` | Falls back to the frame descriptor's `dwMaxBitRate` when a device reports `dwMaxPayloadTransferSize == 0` from its GET_MAX probe, instead of leaving the payload size at zero. |
| A8 | upstream PR #212 (whole-frame suppression) | folded into A9 | `69c7da8` | The per-payload/per-packet error discards this PR wanted were already present; only the whole-frame `frame_had_errors` suppression was new, and that landed as piece 3 of the A9 superset patch rather than as a standalone commit. |
| A9 | upstream PR #184 + PR #212 + saki `9e95b8a` (superset) | pick | `69c7da8` (shared commit with A7) | Corrupt/oversized-frame superset: PTS/SCR bounds guards before reading payload-header fields (prevents an out-of-bounds read on a short/malformed header), a safer `_uvc_populate_frame()` realloc that preserves DJI's zero-`step` compressed-frame tolerance, and the `frame_had_errors` whole-frame suppression from A8. |
| A10 | upstream master `e001f04` | skip-equivalent (verify-only) | already in `eae7f49` (pre-dates this hardening wave; the CVE-2026-1991 fix commit's message says "+ backport e001f04") | Confirmed byte-equivalent by diff; not re-picked. |
| A11 | upstream PR #224 | skip-equivalent | already in `2f32812` (pre-dates this hardening wave) | "Only detach an actually-active kernel driver" is already covered by the fork's `libusb_set_auto_detach_kernel_driver` call plus `uvc_claim_if`'s tolerance of the no-active-driver error codes. |
| A12 | pupil-labs `92d2f82` + `74e7a96` (clock half only) | adapt + pick | `9874f4c` | Preserves `dwClockFrequency` from the VideoControl header for `bcdUVC` 0x0110 and 0x0150 (previously only 0x0100/0x010a set it). Plumbing only; per the SCR-ABSENT verdict in `scr-investigation.md`, this value is never surfaced on frames, so it has no PTS behavior impact. |
| A13 | saki4510t `2596242` | skip-equivalent | none (confirmed no-op) | The libuvc-portion of this commit is comment-only for ref/unref (already correct in the fork) plus an Android-JNI-only function absent from this codebase entirely. Nothing to land. |
| A14 | libuvc upstream issue #242 (double-probe workaround) | plugin-only, not a fork patch | `3d5003e` (plugin repo, not the fork) | Implemented as the `QUIRK_DOUBLE_PROBE` vid:pid quirk seam in `libuvch264src/src/quirks.{c,h}`, wired into `negotiate()`. The DJI Osmo Pocket 3 row sets it alongside `QUIRK_MAX_PIXEL_RATE` — board-measured 23/23 `Invalid mode` failures on a mode increase with a single probe, 0 with two (§2 Step 5). |

**Plugin-side commits that consume the fork's hardening:**

| Plugin commit | What it did |
|----------------|-------------|
| `94a7c21` | Bumped `FORK_SHA` in `scripts/build-libuvc.sh` to `6210f2f...` (ceralive-v0.0.7.3) and fixed stale `v0.0.7.1`-era prose comments. |
| `c46daee` | Added the opt-in `transfer-buffers` property (consumes fork A2's `uvc_set_transfer_buffers()`). |
| `3d5003e` | Added the negotiation-failure descriptor-inventory diagnostics (§2 above) and the `QUIRK_DOUBLE_PROBE` quirk seam (A14). |
| `3cbab94` | Test-only fix scoping the fork-only transfer-buffers test cases behind the `TB_API_AVAILABLE` build-time guard, so the `-DLIBUVC_USE_FORK=OFF` (upstream) build stays green. |

For the full 8-commit-plus-changelog fork history, see
`libuvc-fork-adr.md`'s v0.0.7.3 addendum and the fork's own
`CHANGELOG.ceralive.md`.

---

## 4. Related properties for camera-specific tuning

Two opt-in properties exist specifically to work around device quirks
uncovered by this hardening wave. Neither changes default behavior:

- **`max-payload`** (uint, default `0`): USB payload transfer size hint. See
  `bmaxpayload-analysis.md` for tuning guidance on bandwidth-constrained
  links.
- **`transfer-buffers`** (uint, default `0`): USB transfer buffer count hint,
  backed by fork item A2. See AGENTS.md PROPERTIES for the full contract
  (sentinel default, `[2, 100]` clamp, requires the CeraLive fork).

Both are no-ops at their default value and require no device-specific
configuration for the common case; they exist for the rare camera that
benefits from a nonstandard USB transfer shape.
