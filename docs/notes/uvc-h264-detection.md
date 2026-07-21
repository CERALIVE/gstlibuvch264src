# UVC H.264 Detection — Decision Tree and the RØDE Case Study

**Status:** durable reference — answers "does this USB device have hardware
H.264 over UVC?" from first principles, so the question is never re-litigated
per device.
**Date:** 2026-07-21
**Scope:** how to decide, from a device's own USB descriptors, whether it can
deliver H.264 (or H.265) over UVC — what mechanism it would use, how to prove
presence or absence, and where the honest limits of descriptor evidence are.

This note exists because the question "surely this capture dongle *really* does
H.264 in hardware, our probe just missed it?" recurs. It does not. A UVC device
advertises every video format it can produce in its USB descriptors, and those
descriptors are readable offline without a driver. This note gives the general
decision tree, then walks the RØDE HDMI-to-USB-C (`19f7:0080`) through it as a
worked example whose verdict is final: no descriptor-advertised H.264 mechanism.

---

## 1. The decision tree

Given a UVC device, walk its VideoStreaming (VS) interface descriptors and its
VideoControl (VC) unit descriptors. Exactly one of four outcomes holds.

```
                    ┌─────────────────────────────────────────────┐
                    │ Read the device's USB descriptors            │
                    │ (device + ALL configuration descriptors)     │
                    └───────────────────────┬─────────────────────┘
                                            │
              ┌─────────────────────────────┴─────────────────────────────┐
              │ Does a VS format descriptor carry a Frame-Based            │
              │ H.264/H.265 GUID?                                          │
              │   VS_FORMAT_FRAME_BASED (0x10) whose 16-byte guidFormat    │
              │   begins 'H','2','6','4' (…00001000800000aa00389b71)      │
              │   or 'H','2','6','5' / 'H','E','V','C'                     │
              └───────────────┬───────────────────────────┬───────────────┘
                         YES  │                            │ NO
                              ▼                            ▼
        ┌──────────────────────────────┐   ┌──────────────────────────────────┐
        │ STANDARD PATH                │   │ Does the VC interface expose an   │
        │ Frame-Based Payload H.264.   │   │ Extension Unit (VC_EXTENSION_UNIT │
        │ libuvc parses it; the plugin │   │ 0x06) whose GUID is the UVC H.264 │
        │ selects it by fourcc "H264"/ │   │ XU GUID                            │
        │ "H265". This is the ONLY     │   │ A29E7641-DE04-47E3-8B2B-          │
        │ path the element implements. │   │ F4341AFF003B, alongside an MJPEG  │
        └──────────────────────────────┘   │ auxiliary stream?                 │
                                           └──────────┬───────────────┬────────┘
                                                 YES  │               │ NO
                                                      ▼               ▼
                              ┌──────────────────────────────┐  ┌──────────────────────────┐
                              │ LEGACY XU PATH               │  │ Any OTHER Extension Unit  │
                              │ Muxed H.264 via the UVC H.264 │  │ GUID present (vendor XU)? │
                              │ payload XU (Logitech C920-era │  └──────┬──────────────┬─────┘
                              │ "uvcx"). H.264 rides inside   │    YES  │              │ NO
                              │ the MJPEG stream, extracted    │         ▼              ▼
                              │ via XU probe/commit controls.  │  ┌───────────────┐ ┌──────────────────────────┐
                              │ UNIMPLEMENTED BY DESIGN — see  │  │ VENDOR XU     │ │ NO DESCRIPTOR-ADVERTISED  │
                              │ camera-compat.md:20-24         │  │ Needs the     │ │ H.264 MECHANISM FOUND      │
                              │ (Logitech row, "Unsupported by │  │ vendor's XU   │ │ The device advertises only │
                              │ design").                      │  │ spec to know  │ │ uncompressed/MJPEG. Host   │
                              └──────────────────────────────┘  │ what the unit │ │ re-encodes. Not a probe    │
                                                                │ does; opaque   │ │ bug — there is nothing to  │
                                                                │ without it.    │ │ select.                    │
                                                                └───────────────┘ └──────────────────────────┘
```

### Branch 1 — Frame-Based GUID → standard path

The USB-IF UVC 1.1/1.5 Frame-Based Payload spec is the *only* standards-class
way a UVC device advertises hardware H.264/H.265 as a first-class video format.
The device carries a `VS_FORMAT_FRAME_BASED` (subtype `0x10`) descriptor whose
16-byte `guidFormat` is the standard codec GUID:

- H.264: `{'H','2','6','4', 00 00 10 00 80 00 00 aa 00 38 9b 71}`
- H.265: `{'H','2','6','5', …}` / HEVC `{'H','E','V','C', …}`

The CeraLive libuvc fork parses these (`UVC_VS_FORMAT_FRAME_BASED = 0x10` /
`UVC_VS_FRAME_FRAME_BASED = 0x11`, `libuvc/src/device.c:1772-1814`), preserves
the 16-byte GUID (`device.c:1459-1487`), and byte-matches the standard H.264/
H.265 GUIDs (`stream.c:138-171`), surfacing `UVC_FRAME_FORMAT_H264` /
`_H265` (`libuvc.h:59-93`). `gstlibuvch264src` then selects the format purely by
`memcmp(fourccFormat, "H264", 4) == 0` (`gstlibuvch264src.c:467-473`) — no GUID
gymnastics needed, because libuvc already set `fourccFormat` from the GUID.
**This is the one path the element implements**, and it detects any device that
genuinely exposes standard UVC Frame-Based H.264/H.265.

### Branch 2 — UVC H.264 XU GUID + MJPEG aux → legacy XU path (unimplemented by design)

Before Frame-Based Payload existed, Logitech C920-era webcams shipped hardware
H.264 through a vendor-registered **Extension Unit**: the UVC H.264 payload XU,
GUID `A29E7641-DE04-47E3-8B2B-F4341AFF003B` (the "uvcx" control set), with the
H.264 bitstream muxed inside the device's MJPEG stream and extracted via XU
probe/commit controls. A device on this path has both an MJPEG format AND a
VC Extension Unit carrying that GUID.

Neither libuvc nor this plugin implements XU-muxed H.264 extraction:
`negotiate()` walks only standard VS format descriptors and has no XU probing
path (`gst_libuvc_h264_negotiate()`, `gstlibuvch264src.c:437`). This is a
deliberate scope decision, documented in
[`../../libuvch264src/docs/notes/camera-compat.md`](../../libuvch264src/docs/notes/camera-compat.md)
lines 20-24 (the Logitech C920-era row, **"Unsupported by design"**) — adding
XU-based extraction is a substantial new mechanism, not a compat fix.

### Branch 3 — Vendor XU → needs vendor spec

If the VC interface carries an Extension Unit with a GUID that is neither the
UVC H.264 XU nor anything else recognised, the unit's function is opaque. It
*might* gate a proprietary encoded mode, but nothing in the descriptors says so.
Establishing what it does requires the vendor's XU specification. Absent that,
no claim about H.264 can be made — the honest state is "unknown, and not
establishable from descriptors alone" (see §4).

### Branch 4 — else → no descriptor-advertised H.264 mechanism found

No Frame-Based H.264/H.265 GUID, no UVC H.264 XU, no vendor XU: the device
advertises only uncompressed (YUY2/NV12) and/or MJPEG formats. It has no
descriptor-advertised H.264 mechanism; the host must re-encode its raw or MJPEG
output. This is the correct, final classification — **not** a probe bug, because
there is no format descriptor to select.

---

## 2. Why the USB descriptors are a complete oracle

The decision tree is decisive because a UVC device is required by the USB Video
Class spec to advertise every video format and frame it can produce in its
VideoStreaming interface's altsetting-0 `extra` descriptor buffer, and every
control unit (including Extension Units) in its VideoControl interface. There is
nowhere else for an encoded format to hide:

- Format/frame descriptors live **only** in the VS interface's altsetting-0
  `extra` buffer. Higher altsettings vary endpoint bandwidth, never formats.
- All of this is captured in the device's configuration descriptors, for **all**
  configurations, at enumeration time.

Crucially, the raw descriptors are readable offline, without a driver, without
detaching anything, and without disturbing a running capture. The Linux kernel
exposes the cached raw device descriptor followed by **all** configuration
descriptors verbatim via the `descriptors` sysfs binary attribute — see
`drivers/usb/core/sysfs.c:853-893` (`read_descriptors()`), which copies from the
kernel's `rawdescriptors` cache captured during enumeration. So:

```sh
# Complete, driver-free, service-safe descriptor dump for bus-device 10-1:
od -An -v -tx1 /sys/bus/usb/devices/10-1/descriptors
```

returns the full byte-exact configuration set. If a descriptor-advertised H.264
mechanism existed in **any** configuration, it would appear in that blob. A GUID
hunt (ASCII `"H264"`/`"H265"`/`"HEVC"`) and a subtype scan
(`VS_FORMAT_FRAME_BASED 0x10`, `VC_EXTENSION_UNIT 0x06`) over the blob is
therefore a complete test for branches 1–3.

---

## 3. Case study: RØDE HDMI to USB-C (`19f7:0080`)

A recurring "does the RØDE really do hardware H.264?" question, run all the way
down to the raw descriptor bytes. Verdict: **branch 4 — no descriptor-advertised
H.264 mechanism.** The MJPEG classification the pipeline reports is correct.

**Device:** `19f7:0080` "RØDE HDMI to USB-C", USB 3.2 @ 5000 Mbps,
`bNumConfigurations = 1`, single configuration `wTotalLength = 1320`, 5
interfaces (VideoControl, VideoStreaming, AudioControl, AudioStreaming, HID).

**VideoControl (interface 0, `bcdUVC = 01.00` → UVC 1.0):** descriptors present
are `VC_HEADER`, `VC_INPUT_TERMINAL`, `VC_PROCESSING_UNIT`, `VC_OUTPUT_TERMINAL`.
There is **no `VC_EXTENSION_UNIT` (0x06)** — so no XU of any kind, which rules
out branches 2 and 3 outright. It is a UVC **1.0** device, predating the
Frame-Based Payload that hardware-H.264 UVC requires.

**VideoStreaming (interface 1, single altsetting 0, BULK endpoint 0x83):**
`VS_INPUT_HEADER` reports `bNumFormats = 2`:

- Format 1: `VS_FORMAT_UNCOMPRESSED (0x04)`, GUID
  `5955593200001000800000aa00389b71` = **"YUY2"**, 11 frame descriptors.
- Format 2: `VS_FORMAT_MJPEG (0x06)`, 11 frame descriptors.

**No `VS_FORMAT_FRAME_BASED (0x10)` and no `VS_FRAME_FRAME_BASED (0x11)`
descriptors exist** — the only subtype under which UVC hardware H.264/H.265 can
be advertised is absent, ruling out branch 1.

**GUID hunt across the entire 1320-byte configuration blob:** ASCII `"H264"`,
`"H265"`, and `"HEVC"` are all **absent**; the only ASCII format GUID present is
`"YUY2"`. There is exactly 1 USB configuration, 1 VS interface, and altsetting 0
only — nowhere else for an H.264 format to hide.

**Cross-checks (all agree with the descriptor truth):**

- `v4l2-ctl -d /dev/video1 --list-formats-ext` → exactly `YUYV` + `MJPG`
  (`/dev/video2` is the metadata node).
- The CeraLive libuvc fork *would* surface Frame-Based H.264 if the device
  advertised it (§1, branch 1) — it doesn't advertise it.
- The plugin would post `UVC_ERROR_NOT_SUPPORTED` after logging the full format
  inventory on a no-H.264/H.265 device (`gstlibuvch264src.c:548-559`).

**Vendor evidence:** RØDE's own product page and datasheet call it a "1080p60
Capture Card" with UVC input / plug-and-play, and publish **no** codec/format
claim — no mention of H.264, H.265, HEVC, bitstream, or "hardware encoding".
There is no vendor claim of hardware H.264 to reconcile against, and **no
firmware/vendor channel** for it (no Extension Unit exists to carry one). The
VID/PID `19f7:0080` does not match the MacroSilicon capture-stick IDs
(`345f:2130`/`345f:2131`), so "it's an MS2130" is unproven — the device merely
*behaves* like a MacroSilicon-class raw+MJPEG dongle (YUY2+MJPEG, host
re-encodes). The descriptor evidence stands on its own regardless of chip
identity.

**Verdict:** the RØDE `19f7:0080` genuinely does **not** support hardware H.264
(or H.265) over UVC. This is the exact profile of a plain HDMI-capture dongle —
raw + MJPEG, host re-encodes — not a purpose-built hardware-H.264 UVC camera.

---

## 4. Honest limitation

Descriptors are a complete oracle for anything **standards-class or
XU-advertised** (branches 1–3): if a device exposes H.264 through a Frame-Based
format, the UVC H.264 payload XU, or any Extension Unit, the descriptors prove
it. What descriptors **cannot** rule out is a fully **hidden vendor mode** — a
proprietary encoded pathway a device only enters via out-of-band vendor control
(a companion app, a firmware handshake, a nonstandard bulk protocol) that
advertises **nothing** in its UVC descriptors and exposes **no** Extension Unit.

Such a mode is, by construction, not establishable from descriptors. In
practice its absence is corroborated by the vendor publishing no such claim and
the device carrying no XU to gate it (as with the RØDE), but the strictly honest
statement is: **"no descriptor-advertised H.264 mechanism found; a hidden
vendor-only mode cannot be proven present or absent from descriptors alone."**
The classification the pipeline makes — encode-from-MJPEG — is the correct and
safe behavior for exactly such a device: it is an accurate description of what
the hardware advertises, not a limitation of the software.

---

## 5. References

- **Standard-path parse:** CeraLive libuvc fork — `libuvc/src/device.c:1772-1814`
  (Frame-Based subtype parse), `device.c:1459-1487` (16-byte GUID preserved),
  `stream.c:138-171` (H.264/H.265 GUID match), `libuvc/include/libuvc/libuvc.h:59-93`
  (`UVC_FRAME_FORMAT_H264`/`_H265`).
- **Plugin selection + no-format diagnostics:** `gstlibuvch264src.c:437`
  (`negotiate()` walks standard format descriptors only), `:467-473` (fourcc
  "H264" select), `:548-559` (inventory log + `UVC_ERROR_NOT_SUPPORTED`).
- **Legacy XU path (unimplemented by design):**
  [`../../libuvch264src/docs/notes/camera-compat.md`](../../libuvch264src/docs/notes/camera-compat.md)
  lines 20-24 (Logitech C920-era row).
- **Descriptors-are-complete fact:** Linux kernel
  `drivers/usb/core/sysfs.c:853-893` (`read_descriptors()` — raw device + all
  configuration descriptors via the `descriptors` sysfs attribute).
- **Related XU research:** `libuvch264src/docs/notes/dji-xu-investigation.md`
  (DJI XU control investigation — same class of "XU present, opaque without the
  vendor spec" limitation, branch 3).
