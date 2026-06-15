# INVESTIGATION: DJI camera UVC Extension Unit (XU) descriptor + control surface

**Task:** gstlibuvch264src-hardening Task 7
**Question:** What does the DJI camera's UVC Extension Unit (XU) descriptor and
control-selector surface look like, how would XU control get/set work through the
vendored libuvc, and what would a follow-up plan need to build to drive
DJI-specific controls (gimbal / image / encoder) from this element?
**Method:** Code analysis of the element's existing standard-UVC control path
(`ptz_control.c`), the frame pipeline (`frame_pipeline.c`), and the GObject
property surface (`gstlibuvch264src.c`); cross-referenced against the public
USB-IF UVC 1.5 + UVC H.264 payload specifications and the libuvc v0.0.7 public
API. **No DJI hardware was available** for this investigation, so every
device-specific identifier below (XU GUID, `bUnitID`, control selectors) is
labelled **UNVERIFIED** and must be captured from a physical camera before any
code is written.
**Scope:** Investigation + verdict only. **No working control code, no new
GObject property, no new `.c`/`.h` files, no build entries.** The compile-gated
stub in Appendix A is an illustrative markdown snippet only — it is **not** added
to the build.

---

## VERDICT: **READY-TO-IMPLEMENT (needs camera + chosen intent)**

The mechanism is sound and already half-built: the element uses libuvc's
*standard* UVC Camera-Terminal helpers today (`uvc_get_pantilt_abs` /
`uvc_set_zoom_abs`, capability-gated — `ptz_control.c:238-338`). XU control is the
same control-transfer machinery one layer down: libuvc exposes generic
`uvc_get_ctrl` / `uvc_set_ctrl` / `uvc_get_ctrl_len` primitives that address an
arbitrary `(bUnitID, selector)`. A follow-up can mirror the existing PTZ
probe→gate→clamp→set idiom against an XU unit with **low** structural risk.

What blocks *implementation* is not the mechanism but the **inputs**: the XU
GUID, `bUnitID`, the selector table, and each control's wire layout
(length / signedness / endianness / GET_MIN/MAX/RES) are device-specific and are
**not derivable without a physical DJI camera**. The work is therefore
"ready-to-implement" in the sense that the design is settled and the gating
question is purely a hardware-capture + chosen-intent question — *which* control
(force-IDR? bitrate? gimbal?) does CeraLive actually want to drive. Until a
descriptor capture lands and an intent is chosen, no selector may be hardcoded.

---

## 1. Descriptor capture methodology (hardware-available steps)

The goal of this phase is to produce a **confirmed** table:
`{ XU GUID, bUnitID, controlInterfaceNumber, [selector → name, wLength, GET_INFO
caps, GET_MIN/MAX/RES/DEF] }`. None of this can be guessed; all of it is read off
the device.

### 1.1 Read the static descriptor (`lsusb -v`)

```bash
# Find the DJI device first (vendor:product), then dump its full config descriptor.
lsusb                                   # note the bus/dev and ID, e.g. 2ca3:xxxx (DJI)
sudo lsusb -v -d 2ca3:                  # 2ca3 is DJI's USB vendor ID; confirm on the unit
```

In the VideoControl (VC) interface, locate every **Extension Unit Descriptor**.
Its layout (UVC 1.5 §3.7.2.6, `bDescriptorType = CS_INTERFACE = 0x24`,
`bDescriptorSubType = VC_EXTENSION_UNIT = 0x06`):

| Field | Bytes | Meaning |
|-------|-------|---------|
| `bLength` | 1 | total descriptor length |
| `bDescriptorType` | 1 | `0x24` (CS_INTERFACE) |
| `bDescriptorSubType` | 1 | `0x06` (VC_EXTENSION_UNIT) |
| `bUnitID` | 1 | **the unit address passed to `uvc_get/set_ctrl`** |
| `guidExtensionCode` | 16 | **the XU GUID** identifying the vendor control set |
| `bNumControls` | 1 | number of controls in this XU |
| `bNrInPins` | 1 | input pin count |
| `baSourceID[]` | bNrInPins | upstream unit/terminal IDs |
| `bControlSize` | 1 | size in bytes of the `bmControls` bitmap |
| `bmControls[]` | bControlSize | **bitmap: which control selectors exist** |
| `iExtension` | 1 | string-descriptor index (often 0) |

`lsusb -v` prints these as `VideoControl Extension Unit Descriptor` with
`guidExtensionCode`, `bUnitID`, `bNumControls`, and `bmControls`. Record **all**
XUs — a DJI camera may expose more than one (e.g. one for gimbal, one for the
H.264 encoder).

**Reading `bmControls`:** it is a little-endian bit array. Bit *n* set ⇒ control
**selector `n+1`** is present (selectors are 1-based on the wire). `bNumControls`
is the count of set bits. The bitmap tells you *which selectors exist* but **not**
what they do — naming requires §1.3 (live capture) or vendor docs.

> If `lsusb -v` shows the XU GUID as raw bytes, remember the GUID wire order:
> the first three fields (Data1 u32, Data2 u16, Data3 u16) are little-endian, the
> last 8 bytes are byte-order-as-written. Mis-ordering the GUID is the single most
> common XU-identification error.

### 1.2 Programmatic descriptor dump (no GUI)

libuvc can print the parsed control interface, including extension units, via
`uvc_print_diag(devh, stderr)` (a tiny throwaway harness, *not* part of this
element). It walks `devh->info->ctrl_if.extension_unit` and prints `bUnitID`,
the GUID, and `bNumControls`. This is the fastest way to confirm that the
**vendored libuvc actually parsed** the XU the way `lsusb` sees it — a useful
sanity check before writing any access code, because libuvc's parser, not
`lsusb`, is what the element will rely on.

### 1.3 Live control-transfer capture (`usbmon` / Wireshark)

The static descriptor gives you GUID + `bUnitID` + which selectors exist. It does
**not** tell you each selector's *meaning* or *wire format*. To get those, capture
the DJI vendor app / SDK (or DJI's own UVC host tool) actually driving the camera:

```bash
sudo modprobe usbmon
# Identify the bus from lsusb, then capture (Wireshark uses the usbmonN interface):
sudo wireshark            # capture interface: usbmon<BUS>, filter: usb.transfer_type==2 (CONTROL)
# or headless:
sudo cat /sys/kernel/debug/usb/usbmon/<BUS>u > dji-xu.usbmon
```

For each XU control transfer, decode the SETUP packet:

| SETUP field | XU GET/SET meaning |
|-------------|--------------------|
| `bmRequestType` | `0xA1` = class/interface IN (GET); `0x21` = class/interface OUT (SET) |
| `bRequest` | request code: `0x81` GET_CUR, `0x82` GET_MIN, `0x83` GET_MAX, `0x84` GET_RES, `0x86` GET_INFO, `0x87` GET_DEF, `0x88` GET_LEN, `0x01` SET_CUR |
| `wValue` | **selector** in the high byte (`selector << 8`), low byte 0 |
| `wIndex` | **`bUnitID` in the high byte**, VC interface number in the low byte (`unit << 8 | interface`) |
| `wLength` | payload length (cross-check against GET_LEN) |

By correlating an app action ("tap record start" / "force keyframe" /
"pan left") with the selector + payload that goes out, you build the
selector→semantics map that the static descriptor cannot provide. **This is the
authoritative step** — everything downstream depends on it.

### 1.4 Deliverable of phase 1

A confirmed, written table (one row per usable selector) with: selector number,
human name (from §1.3 correlation), `wLength`, GET_INFO capability bits
(supports GET? SET? auto?), and observed MIN/MAX/RES/DEF where the control is a
ranged value. Without this table, no SET write is safe.

---

## 2. libuvc XU access model

### 2.1 What libuvc gives you (public API, v0.0.7 — the pinned SHA)

libuvc exposes generic, unit-agnostic control primitives in `libuvc.h`:

```c
/* Discover the wire length of a control (issues GET_LEN). */
int uvc_get_ctrl_len(uvc_device_handle_t *devh, uint8_t unit, uint8_t ctrl);

/* Generic GET: req_code ∈ {UVC_GET_CUR, _MIN, _MAX, _RES, _DEF, _INFO, _LEN}. */
int uvc_get_ctrl(uvc_device_handle_t *devh, uint8_t unit, uint8_t ctrl,
                 void *data, int len, enum uvc_req_code req_code);

/* Generic SET_CUR. */
int uvc_set_ctrl(uvc_device_handle_t *devh, uint8_t unit, uint8_t ctrl,
                 void *data, int len);
```

These are the **exact** primitives the typed CT/PU helpers (`uvc_set_zoom_abs`,
`uvc_get_pantilt_abs`, etc.) are built on. `unit` is the `bUnitID` from §1.1;
`ctrl` is the 1-based selector from §1.1/§1.3. libuvc fills in `bmRequestType`,
`wValue = ctrl << 8`, `wIndex = unit << 8 | interface`, and submits the control
transfer — i.e. it does §1.3's SETUP packet for you. **The plugin only needs the
two integers `(unit, ctrl)` and a correctly-sized/typed `data` buffer.**

The return value is the libusb byte count (≥0) on success or a negative
`uvc_error_t`, so it maps cleanly onto the element's existing
`gst_libuvc_h264_src_post_error()` helper (`ptz_control.c:298-300`).

### 2.2 What libuvc does *not* give you cleanly

- **Public XU enumeration.** v0.0.7's public header has no stable getter that
  hands back the parsed extension-unit list (GUID/`bUnitID`/`bmControls`) as a
  public struct. The data exists in `devh->info->ctrl_if.extension_unit`
  (`libuvc_internal.h`) and is printed by `uvc_print_diag`, but a clean public
  accessor is **not** guaranteed. **Implication:** the follow-up either (a) hard-
  configures `bUnitID`/selector from the §1 capture (simplest, matches how vendor
  XU code is normally written), or (b) adds a small parser/helper. It must **not**
  assume a public libuvc XU-discovery call exists without confirming it against
  the pinned SHA first. This is the one libuvc-capability fact that needs
  verification before scoping the helper.
- **Endianness / signedness.** `uvc_get_ctrl`/`uvc_set_ctrl` move raw bytes. UVC
  multi-byte controls are **little-endian** on the wire; the caller is responsible
  for packing/unpacking and for signedness (cf. how `pan`/`tilt` are signed
  arcseconds but `zoom` is unsigned — `gstlibuvch264src.c:103-113`). Each XU
  control's layout comes from §1.3, not from libuvc.

### 2.3 What the plugin would need to add (described, not implemented)

Mirroring the existing PTZ surface, a follow-up would add, in the **element**
(never in libuvc):

1. **An XU probe** at `start()` after `uvc_open()` — call `uvc_get_ctrl_len` /
   `uvc_get_ctrl(..., UVC_GET_INFO)` for each selector of interest, store
   supported flag + range, exactly like `ptz_probe` stores `pan_supported` /
   `pan_min` / `pan_max` (`ptz_control.c:230-286`). A control absent or
   GET_INFO-without-SET ⇒ gated off, silently, same policy as PTZ.
2. **Typed get/set helpers** that clamp to the probed range and pack LE bytes,
   one per chosen control, structurally identical to
   `gst_libuvc_h264_src_ptz_set_zoom` (`ptz_control.c:323-338`): lock
   `control_mutex`, clamp, `uvc_set_ctrl`, update cached current on success, post
   error on failure.
3. **Capability-gated GObject surface** — but only *after* §1 confirms a real
   selector. See §4 for why this is the last step, not the first.

No change to `frame_pipeline.c` is required for *transport* of XU controls; the
only interaction is semantic (see §3.3 on force-IDR).

---

## 3. Candidate control surfaces

Three distinct families, with very different standardization and risk profiles.
The element today drives **only** the first half of family A (standard CT PTZ).

### 3.1 Gimbal / motor control — DJI-proprietary XU (likely)

Physical gimbal articulation (motorized pan/tilt of the camera head, gimbal mode,
recenter, follow/FPV mode) is **not** the same as UVC Camera-Terminal PanTilt.
Standard CT `PanTilt(Absolute)` (which the element already drives,
`ptz_control.c:288-321`) is an *optical/sensor* pan within the imager; a DJI
gimbal's motor control is almost certainly a **vendor XU** with DJI-specific
selectors (mode switch, angle set, recenter, calibration).

- **Standardization:** proprietary. Expect a DJI XU GUID, not a UVC-defined one.
- **Verification needed:** GUID + `bUnitID` + selector semantics, all from §1.3.
- **Caveat:** some DJI UVC modes may expose gimbal pan/tilt *through* the standard
  CT PanTilt control (which is why the existing capability gate may already report
  `pan_supported`/`tilt_supported` on some units). Phase 1 must distinguish
  "CT PanTilt works" from "a separate gimbal XU exists" before choosing a surface.

### 3.2 Camera / image controls — mostly standard UVC CT + PU

Exposure, ISO/gain, white balance, focus, brightness, contrast, sharpness are
**standard** UVC controls split across the **Camera Terminal (CT)** (exposure
time, focus, iris) and the **Processing Unit (PU)** (white balance, brightness,
gain, contrast). libuvc already has typed helpers for most of these
(`uvc_set_exposure_abs`, `uvc_set_white_balance_temperature`, `uvc_set_gain`, …),
so a chunk of "image control" could be added **without any XU at all** — same
probe→gate→set pattern as PTZ, just different libuvc calls.

- **Standardization:** standard UVC; no XU/GUID needed for the common ones.
- **Risk:** **low** — these reuse the proven PTZ idiom against documented helpers.
- **Caveat:** a *DJI-specific* image control (e.g. a colour profile / D-Log /
  scene mode) with no UVC PU equivalent would fall back to an XU and re-enter the
  §1 capture requirement. Don't assume every image knob is standard.

### 3.3 Encoder controls (bitrate / IDR / GOP / rate-control) — XU, two flavours

This is the family with the **highest value** to the cerastream→srtla→SRT path
(on-demand keyframe after packet loss; adaptive bitrate) and the **most
uncertainty**. The element today is purely a *receiver* of the encoded bitstream:
`frame_pipeline.c` parses NALs, runs the IDR gate (`had_idr`,
`frame_pipeline.c:273-301`), and re-prepends SPS/PPS/VPS — but it has **no
control** over how the camera's encoder produces that stream. Bitrate, GOP length,
rate-control mode, and "emit an IDR now" are all encoder-side knobs reachable only
via a control transfer.

Two possible transports, in preference order:

1. **Standardized UVC H.264 payload XU (preferred if present).** The USB-IF
   "UVC 1.5 / H.264 video payload" spec defines a *standard* extension unit for
   encoder control, with a fixed GUID and a defined selector set, e.g.:
   - `UVCX_VIDEO_CONFIG_PROBE` / `UVCX_VIDEO_CONFIG_COMMIT` (negotiate encoder
     config),
   - `UVCX_RATE_CONTROL_MODE`,
   - `UVCX_BITRATE_LAYERS`,
   - `UVCX_PICTURE_TYPE_CONTROL` (**force an IDR / key-frame on demand**),
   - `UVCX_QP_STEPS`, `UVCX_LTR_*`, etc.

   The standardized GUID for this XU is documented as
   `{A29E7641-DE04-47E3-8B2B-F4341AFE7B2A}` (USB-IF UVC H.264 payload).
   **UNVERIFIED that any DJI camera implements it** — many H.264 UVC cameras do,
   some don't, and DJI may substitute its own XU. Phase 1's `lsusb -v` GUID dump
   answers this immediately: if the camera advertises this GUID, encoder control
   is a *standard*, well-documented surface; if not, it's proprietary.

2. **DJI-proprietary encoder XU (fallback).** If the standardized GUID is absent,
   bitrate/IDR live behind a DJI XU and require the full §1.3 live-capture to map
   selectors. Higher effort, no public spec.

**Interaction with `frame_pipeline.c` (important for an IDR-on-demand feature):**
a `force-IDR` XU write would make the camera emit a fresh IDR + parameter sets.
The element's existing logic already handles a fresh IDR correctly — the IDR gate
(`had_idr`) and the `send_sps_pps` re-prepend (`frame_pipeline.c:273-296`) ensure
the new keyframe is forwarded with SPS/PPS/VPS in front. A follow-up should
**reuse** that path: after issuing the XU force-IDR, no special-casing in the
pipeline is needed; the next IDR flows through the gate normally. (If the feature
also wants to *re-arm* the gate — e.g. drop inter frames until the forced IDR
arrives — that mirrors the reconnect re-arm already noted in the reconnect spike.)

| Surface | Standard? | Transport | Risk | Pre-req |
|---------|-----------|-----------|------|---------|
| CT PanTilt / Zoom (done) | Standard UVC CT | typed libuvc helpers | — | already shipped |
| Gimbal motor / mode | DJI-proprietary | XU `uvc_set_ctrl` | high | §1.3 capture |
| Exposure / ISO / WB / focus | Standard UVC CT+PU | typed libuvc helpers | low | none (helpers exist) |
| DJI image/colour profile | Proprietary | XU `uvc_set_ctrl` | med | §1.3 capture |
| Bitrate / rate-control / GOP | Standardized H.264 XU **or** proprietary | XU `uvc_set_ctrl` | med–high | §1.1 GUID check |
| Force-IDR (key-frame) | Standardized H.264 XU **or** proprietary | XU `uvc_set_ctrl` | med | §1.1 GUID check |

---

## 4. Recommended future implementation scope

### 4.1 What a follow-up plan should build

1. **Pick ONE intent first.** Do not build a generic XU framework. The
   highest-leverage single feature for the SRT path is **force-IDR on demand**
   (recover from loss without a full reconnect). Bitrate control is second.
   Gimbal control is a separate product decision. Scope the follow-up to one
   concrete control so the §1 capture is bounded.
2. **Phase 1 (this report's §1) is a hard prerequisite task** — produce the
   confirmed descriptor + selector table from a physical DJI camera. No code
   merges before this artifact exists.
3. **XU probe + typed helper**, in `uvc_device.c` / `ptz_control.c`-style modules,
   capability-gated exactly like the PTZ probe: store `<ctrl>_supported` + range
   from GET_INFO/GET_LEN/GET_MIN/MAX, gate every SET on it, clamp, lock
   `control_mutex`, post errors via the existing helper. **Zero libuvc edits** —
   use only the public `uvc_get_ctrl`/`uvc_set_ctrl`/`uvc_get_ctrl_len`.
4. **GObject surface, added last:** e.g. a `force-idr` **action signal**
   (mirrors `set-ptz`, `gstlibuvch264src.c:139-142`) for a momentary command, or a
   `bitrate` int property (mirrors `zoom`, `gstlibuvch264src.c:111-113`) for a
   ranged value. Each capability-gated and silently ignored when unsupported,
   identical to the PTZ contract. **Not added in this task.**

### 4.2 Mock-test strategy (hardware-independent, mirrors the existing suite)

The suite is mock-backed (`tests/mock_libuvc.c`, ~16 fns) and must stay
hardware-free:

- **Extend `mock_libuvc.c`** with `uvc_get_ctrl` / `uvc_set_ctrl` /
  `uvc_get_ctrl_len` mocks that accept a configurable `(unit, selector)` table,
  return programmable GET_INFO/GET_LEN/MIN/MAX, and **record the last SET payload**
  (so a test can assert the exact bytes + LE packing that went out).
- **New `tests/test_xu.c`** (a follow-up file, not this task) asserting:
  capability gate (unsupported selector ⇒ SET is a silent no-op, like the PTZ
  gate in `test_ptz.c`); range clamp; LE byte packing round-trip; GET_LEN
  mismatch handled; error mapping on a mocked `uvc_set_ctrl` failure.
- **Sanitizer parity:** ASan/TSan variants like the rest of the suite; the
  `control_mutex` discipline is the same blind-spot profile already baselined.
- **Param-spec range discipline:** if a `bitrate` property is added, tests must
  stay inside the param-spec range — an out-of-range `g_object_set` triggers the
  GObject range-warning longjmp that hangs gst-check (a guardrail already learned
  for PTZ).

The mock proves *plumbing and gating*; it can **never** prove the selector
numbers or wire formats are correct — only hardware can.

### 4.3 Hardware sign-off gate (non-negotiable)

Modelled on the reconnect spike's SAFE/UNSAFE gate, no XU **write** code merges
until:

1. The §1 descriptor + selector table is captured from a **physical DJI camera**
   and recorded in a note (GUID, `bUnitID`, per-selector `wLength` + range).
2. The chosen control is **driven successfully on that physical camera** (e.g. a
   force-IDR XU SET demonstrably produces an IDR in the captured stream; a bitrate
   SET demonstrably changes the encoded rate) — observed, not assumed.
3. The behaviour is confirmed on at least the specific DJI model(s) CeraLive
   ships against; a selector confirmed on one DJI model is **not** assumed
   portable to another without re-capture.

Until all three pass, the surface stays unimplemented and this report's verdict
("needs camera + chosen intent") holds.

---

## Appendix A — illustrative compile-gated stub (NOT in the build)

> **This snippet is documentation only.** It is **not** added to any `.c`/`.h`
> file, `meson.build`, or `CMakeLists.txt`. It exists to show the *shape* a
> follow-up would take, mirroring `ptz_control.c`. The `#warning` and the
> `#if 0` make explicit that the identifiers are placeholders and that the code
> must **never** compile into a shipped artifact until §4.3 is satisfied.

```c
#if 0  /* ILLUSTRATIVE ONLY — do not compile, do not add to build */
#warning "DJI XU: unverified on hardware — bUnitID/selector/GUID are PLACEHOLDERS"

/* PLACEHOLDERS — every value here MUST come from a §1 hardware capture.
 * These are NOT real DJI values and must not be treated as confirmed. */
#define DJI_XU_UNIT_ID        0xFF   /* <- bUnitID from `lsusb -v` (UNVERIFIED) */
#define DJI_XU_SEL_FORCE_IDR  0x01   /* <- selector from usbmon capture (UNVERIFIED) */

/* Mirrors gst_libuvc_h264_src_ptz_set_zoom (ptz_control.c:323-338):
 * gate on a probed capability flag, lock, issue the control transfer via the
 * PUBLIC libuvc primitive, map errors through the existing helper. */
static gboolean dji_xu_force_idr(GstLibuvcH264Src *self) {
    if (!self->uvc_devh || !self->xu_force_idr_supported) return FALSE;  /* capability gate */

    uint8_t payload = 1;  /* wire layout is UNVERIFIED — confirm wLength + meaning from capture */

    g_mutex_lock(&self->control_mutex);
    /* uvc_set_ctrl(devh, unit, selector, data, len) — the SAME primitive the
     * typed CT helpers wrap; libuvc fills bmRequestType/wValue/wIndex. */
    uvc_error_t res = uvc_set_ctrl(self->uvc_devh,
                                   DJI_XU_UNIT_ID, DJI_XU_SEL_FORCE_IDR,
                                   &payload, (int)sizeof(payload));
    g_mutex_unlock(&self->control_mutex);

    if (res < 0) {  /* uvc_set_ctrl returns negative uvc_error_t on failure */
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "forcing IDR (XU)");
        return FALSE;
    }
    /* The forced IDR then flows through the existing IDR gate + SPS/PPS
     * re-prepend in frame_pipeline.c:273-296 with no pipeline change. */
    return TRUE;
}
#endif
```

---

## Summary

| Question | Answer |
|----------|--------|
| Can libuvc address an arbitrary XU control? | **Yes** — public `uvc_get_ctrl`/`uvc_set_ctrl`/`uvc_get_ctrl_len(unit, selector, …)` |
| Is the access pattern already proven in this element? | **Yes** — the PTZ path (`ptz_control.c`) is the same machinery one layer up (typed CT helpers) |
| What's missing to implement? | The **device-specific** GUID + `bUnitID` + selector table + wire formats — capturable **only** from a physical DJI camera (§1) |
| Are gimbal/image/encoder controls the same kind of thing? | **No** — image controls are mostly *standard* CT/PU (no XU); gimbal is *proprietary* XU; encoder (bitrate/force-IDR) is a *standardized H.264 XU OR* proprietary — `lsusb -v` GUID check decides |
| Does force-IDR need pipeline changes? | **No** — the existing IDR gate + SPS/PPS re-prepend (`frame_pipeline.c:273-296`) already handles the resulting fresh IDR |
| Can the mock suite cover it? | **Plumbing + gating yes** (extend `mock_libuvc.c`); **selector/wire-format correctness no** — hardware sign-off only |
| Blocking gate before any write code? | **§4.3** — captured descriptor + driven-on-real-camera + per-model confirmation |

**VERDICT: READY-TO-IMPLEMENT (needs camera + chosen intent)**
