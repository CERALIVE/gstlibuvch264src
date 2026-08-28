# Camera Compatibility Matrix

**Status:** informational, updated as devices get validated
**Date:** 2026-07-03 (§2 Steps 5-6 updated 2026-08-27)
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
a single flag:

- `QUIRK_MAX_PIXEL_RATE` — for cameras that advertise frame intervals they
  cannot deliver. The row carries the highest `width x height x fps` the element
  is willing to select, and negotiation drops every advertised rate above it.

The table holds **one** row: the DJI Osmo Pocket 3 (`2ca3:0023`), capped at
`3840x2160x30` = 248 832 000 px/s.

Stream-control probing is **no longer a quirk**. Since the 2026-08-27 probe-policy
campaign, every device gets the same bounded retry: one normal probe, and exactly
one retry when libuvc returns `UVC_ERROR_INVALID_MODE`. A successful first probe
still stops after one attempt, and every other libuvc error propagates untouched.
The old `QUIRK_DOUBLE_PROBE` flag that used to carry this per-device is gone from
the table, the header, and `negotiate()`; the paragraphs below are the dated
evidence that led there.

**Why the stale-readback retry exists (the "camera is not detected" symptom).**
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

**2026-08-27 probe-policy campaign.** A fresh Rock 5B+ run reproduced the same
directional failure at larger counts. The harness emitted:

```text
CLASS_COUNTS: class=increase n=10 failures=10 inconclusive=0
CLASS_COUNTS: class=same n=10 failures=0 inconclusive=0
CLASS_COUNTS: class=decrease n=10 failures=0 inconclusive=0
RULE_A_BRANCH: REPRODUCED policy=single n=10 failures=10 rate=100.0%
```

The behavior-triggered `retry` policy then ran the same three classes at N=10
per class with no failures or inconclusive attempts. Its aggregate decision was:

```text
RULE_G: G1 retry_runs=1 failures=0 undersized_classes=0
```

An unconditional two-probe diagnostic arm also passed every class, but the
selected general policy is the narrower retry, and it now applies to **every**
device rather than to a quirked row: make one normal probe, retry once only when
libuvc returns `UVC_ERROR_INVALID_MODE`, and propagate every other error. That is
what retired `QUIRK_DOUBLE_PROBE` — the workaround became the default, so the
per-device flag had nothing left to switch on. See
[UVC probe negotiation](uvc-probe-negotiation.md) for the method, provenance, and
design rationale.

A later shipping-build run on 2026-08-27 re-confirmed it end to end: the
production `.so`, built with no development probe-policy override compiled in,
passed the increase, same, and decrease transition classes at N=10 each with zero
negotiation failures.

**Why it also carries a pixel-rate cap.** Its H.264 descriptor advertises
3840x2160 at 60/50/48 fps, and `negotiate()` prefers the largest area at the
highest fps, so 4K@60 was selected by construction. Those rates were originally
recorded negotiating cleanly and then delivering **zero** frames, with the 5 s
silence watchdog reporting a disconnect that never happened.

> **The zero-frame premise did NOT reproduce on 2026-07-30.** On libuvc `4868e57`
> the unquirked binary negotiated 4K@60 and delivered real, sustained 4K —
> 600 access units in 10.6 s (~56 fps) twice over, with `h264parse` reading
> `3840x2160`, `high` profile, level `5.2` straight out of the SPS.

**4K@30 is settled: the cap was raised, and the capture came first.** The cap now
sits at `3840x2160x30` = 248 832 000 px/s, measured through this element on board
`192.168.78.131` on 2026-07-30 with the plugin `.so` deployed alone (the board's
libuvc untouched at `4868e57`, so the cap was the only variable). Two runs of
`num-buffers=300 ! video/x-h264,width=3840,height=2160,framerate=30/1`:

| run | access units | duration | `h264parse` caps from the SPS | errors |
|---|---|---|---|---|
| 1 | clean EOS, exit 0 | 10.71 s | `3840x2160`, `high`, level `5.2` | 0 |
| 2 | **300/300** (counted at an `identity` probe) | 10.79 s | `3840x2160`, `high`, level `5.2`, `4:2:0` | 0 |

No `RESOURCE/READ`, no silence-watchdog disconnect on either run. Both logged
`max pixel rate 248832000` and `quirk: dropped 3 non-deliverable rate(s) at
3840x2160` — 60/50/48 stayed excluded while the capped rate streamed. The value
is 4K@30 *exactly* rather than the whole 4K@60 descriptor range, which is what
makes the capture conclusive: `negotiate()` prefers max area then max fps, so
3840x2160@30 is the surviving top mode **by construction** and the runs cannot
have silently measured something else.

> **The caveat below still stands for every FUTURE raise, including 4K@60.** The
> original zero-frame observation was real and was never explained (one candidate:
> the same stale-readback defect the bounded `UVC_ERROR_INVALID_MODE` retry exists
> for can leave a bound-but-silent stream when the readback compares equal), so
> 4K@60/50/48 remain capped out. Raising the cap further is the one-number change
> described in `quirks.c` — do not make it on the strength of a descriptor, a
> datasheet, a successful `uvc_get_stream_ctrl_format_size()`, or this note alone.
> It takes advancing frames on real hardware: a bounded access-unit count,
> SPS-verified geometry, reproduced.

**2026-08-27 cap adjudication.** The shipping-candidate `retry` arm did not
revive the higher advertised rates. The harness emitted the same failures for
the unconditional-double diagnostic arm, and the reduced single-probe control
also failed at 4K@60:

```text
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@60 failures=45/45
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@50 failures=45/45
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@48 failures=45/45
VERDICT_CELL: PASS arm=after-smaller mode=3840x2160@30 passes=5/5
RULE_C_FINAL: C2 rate=248832000 provenance=edge-aggregate-contiguous-ceiling
```

The failed high-rate runs reported `Unable to negotiate common caps` followed
by `not-negotiated (-4)`, with no `Invalid mode` signal and no delivered access
units. That is a caps-negotiation rejection, not the stale-readback condition the
bounded retry fixes. The cold-start and after-replug cells were formally recorded
as `SKIPPED reason=unattended`; they were not counted as passes. The conservative
aggregate therefore retains the verified 4K@30 ceiling.

**Shipping-build timing correction (2026-08-28).** Three initial production-build
confirmation cells each emitted `VERDICT: FAIL` at 9/10 because the harness
computed fps as AU count divided by whole-process wall time, including process
spawn, negotiation, and USB/libuvc startup. The harness's `run_rate_metrics()`
now scores sustained delivery rate from
`(AU count - 1) / (last AU PTS - first AU PTS)` instead. The corrected
function re-scored all 30 preserved captures as passing at 27.725–29.976 fps, and
a fresh live shipping-build cell then emitted `VERDICT: PASS` at 10/10 with
`fps_source=pts-span` on every replicate (27.859–29.969 fps). Every capture
delivered 300/300 access units at SPS-verified `3840x2160`, with zero element
errors. The original three FAIL outputs remain historical facts about the buggy
wall-clock scorer; the corrected live script verdict is the shipping confirmation.
The retained 248 832 000 px/s ceiling is confirmed.

**Verdict scope:** these 2026-08-27 probe and cap results apply to the tested DJI
Osmo Pocket 3 `2ca3:0023`, `bcdDevice 5.04`, firmware/product string
`DJIPocket3`, serial `123456789ABCDEF`, on a Radxa ROCK 5B+ running
`7.2.0-ceralive-rk3588`. They do not establish behavior for another firmware,
serial, camera model, or host stack.

See `quirks.c` for the full evidence and for how to raise the cap.

If you find another camera that advertises rates it cannot deliver, that's a
signal to add a table entry — not something the field-triage steps above can
toggle at the command line today. A camera that needs the stale-readback retry
needs no entry at all: it already gets it.

### Step 6: over USB-C to USB-C, check the Type-C role BEFORE blaming the element

**Verdict scope:** measured 2026-08-26/27 on a DJI Osmo Pocket 3 (`2ca3:0023`,
`bcdDevice=0504`) against two RK3588 boards (Radxa ROCK 5B+ and Orange Pi 5 Plus)
running locally built mainline-track kernels. It is a statement about that camera
and those boards, not a general claim about USB-C cameras.

When the camera is attached to an RK3588 board with a C-to-C cable, a whole class of
"the camera isn't detected" reports never reaches this element at all. Both ends are
dual-role, so the port's role is settled by CC-line arbitration, and when the board
loses that arbitration it is running as a USB *peripheral* — the camera's bus is
absent from `/sys/bus/usb/devices/` entirely, and `lsusb` in Step 1 shows nothing.
No plugin-side change can repair that. Check the role first:

```bash
cat /sys/class/typec/port0/port_type      # expect: [dual] source sink
cat /sys/class/typec/port0/power_role
cat /sys/class/typec/port0/data_role      # the camera needs: [host] device
cat /sys/class/typec/port0/../port0-partner/... 2>/dev/null   # partner presence
```

`data_role` reading `host [device]` with a partner present is the signature. On a
CeraLive image the on-board `ceralive-typec-policy` requests a bounded data-role
swap to `host` automatically on a settled sink/device attach; the useful diagnostic
is `journalctl -u ceralive-typec-policy.service`. Do not "fix" this by pinning
`port_type` to `source` — that was tried and retired: with a forced-source port the
Osmo never presented Rd and **no attachment formed at all, 3 of 3 physical
replicates**.

**Camera-side preflight, and it matters more than it looks.** The Osmo has an
on-camera Type-C mode selector, and the camera must be in its **webcam / UVC** mode
before any of the above is meaningful — a camera sitting in a file-transfer or
charge-only mode is not a UVC device and will not enumerate one however the roles
land. Confirm the on-screen mode before escalating.

**Battery-state caveat.** A DJI Osmo Pocket 3 with a critically low battery has been
observed negotiating defensively over Type-C, so a "camera not detected" report from
a nearly flat camera is not trustworthy evidence of anything. The 2026-08-27
adjudication above was deliberately run at a confirmed **88% battery** for exactly
this reason. Charge the camera before treating a negotiation failure as a finding.

**What the healthy-battery adjudication actually found** (2026-08-27, verdict
`MIXED`, all three legs machine-computed by a hardware drill rather than read off a
log by hand):

- Under a **forced-source** port the camera **never presents Rd** — 3/3 replicates,
  no attach — which is the conclusive part and the reason the force-source approach
  is gone.
- Under a genuine **dual-role** port the natural arbitration is *not* deterministic
  toward sink as had been assumed: across 3 replicates the board landed
  source/host **once** and sink/device twice. So "the camera is always Rp-only" is
  an oversimplification — it is Rp-only in the forced-source arm, and merely
  *usually* the source-side winner in free arbitration.
- The **data-role swap works cleanly**: first-attempt success on both replicates
  where it was needed, after which the camera enumerated normally as
  `2ca3:0023` and this element negotiated as usual.
- A **power-role swap request** is not production-grade against this camera:
  4 attempts across the session gave 1 clean success, 1 timeout, and 2 rejections,
  so the shipped board policy never issues one.

None of this changes anything inside this element. It is recorded here because the
symptom — "the camera isn't detected" — is identical to a negotiation failure, and
Step 2's descriptor inventory will print nothing at all when the device was never on
the bus to begin with.

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
| A14 | libuvc upstream issue #242 (double-probe workaround) | plugin-only, not a fork patch | `3d5003e` (plugin repo, not the fork) | Originally shipped as the `QUIRK_DOUBLE_PROBE` vid:pid quirk flag, set on the DJI Osmo Pocket 3 row — board-measured 23/23 `Invalid mode` failures on a mode increase with a single probe, 0 with two (§2 Step 5, 2026-07-30). **Superseded 2026-08-27:** the workaround is now the universal default, a bounded single retry on `UVC_ERROR_INVALID_MODE` inside `negotiate()`, and the flag was removed. The behaviour A14 asked for still ships; it is simply no longer per-device. |

**Plugin-side commits that consume the fork's hardening:**

| Plugin commit | What it did |
|----------------|-------------|
| `94a7c21` | Bumped `FORK_SHA` in `scripts/build-libuvc.sh` to `6210f2f...` (ceralive-v0.0.7.3) and fixed stale `v0.0.7.1`-era prose comments. |
| `c46daee` | Added the opt-in `transfer-buffers` property (consumes fork A2's `uvc_set_transfer_buffers()`). |
| `3d5003e` | Added the negotiation-failure descriptor-inventory diagnostics (§2 above) and the `QUIRK_DOUBLE_PROBE` quirk seam (A14). The quirk flag was later retired on 2026-08-27 when its behaviour became the universal probe default; the diagnostics are unchanged. |
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
