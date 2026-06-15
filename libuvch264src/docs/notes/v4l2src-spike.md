# SPIKE: native `v4l2src` capture vs the libuvc element

**Task:** libuvc-modernization Task 8
**Question:** Can the kernel's `v4l2src` (UVC payloads exposed through
`/dev/videoN` by the in-tree `uvcvideo` driver) replace, or partly replace, this
element's userspace libuvc capture path for the CeraLive pipeline? What would it
cost us, and what would it break?
**Method:** Code analysis of this element (`uvc_device.c` V4L2 probe,
`frame_pipeline.c` SPS/PPS injection, PTZ/control surface, disconnect/reconnect
behavior), the documented DJI UVC descriptor pathology
(`dwMaxVideoFrameBufferSize=0` → `VIDIOC_REQBUFS` failure), and the Rockchip
kernel/decoder matrix (README + AGENTS). Report only — **no code, no build
changes**.
**Scope:** Viability + problem inventory + recommendation. This spike writes one
markdown file and nothing else.

---

## VERDICT: NOT-VIABLE

Native `v4l2src` **cannot** capture from the DJI action cameras that are this
element's primary target, on **either** kernel (5.10 or 6.6), because the
device's UVC descriptors make `uvcvideo` fail buffer setup before a single frame
is delivered. It *could* in principle capture from a spec-compliant generic UVC
H.264/H.265 camera, but doing so would forfeit the in-band SPS/PPS injection,
the XU/vendor PTZ control mapping, the silence-based disconnect detection, the
in-element reconnect, and the running-time PTS contract that `cerastream`
currently relies on — replacing one well-understood element with a
device-class-dependent split path and a pile of downstream regressions. The
inherited "REPLACE rejected" conclusion holds. Keep libuvc as the single
capture path.

---

## 1. Why kernel `uvcvideo` cannot capture DJI frame-based H.264

### 1.1 The descriptor pathology

DJI action cameras advertise their H.264 stream as a **frame-based** UVC format
(`UVC_VS_FORMAT_FRAME_BASED` / `UVC_VS_FRAME_FRAME_BASED`, GUID
`H264`/`0x34363248`) but populate the frame descriptor with
**`dwMaxVideoFrameBufferSize = 0`**. That field is the device's own declaration
of the largest compressed frame it will ever emit. It is the number the host
uses to size capture buffers.

The kernel `uvcvideo` driver reads `dwMaxVideoFrameBufferSize` into the format's
`maxVideoFrameSize` and propagates it to `v4l2_format.fmt.pix.sizeimage` when
user space negotiates the format. With the device reporting `0`, the negotiated
`sizeimage` is `0` (or is clamped to a value the driver will not honour for a
compressed stream).

### 1.2 Where it fails: `VIDIOC_REQBUFS`

`v4l2src` follows the mandatory V4L2 streaming I/O ritual:

```
VIDIOC_ENUM_FMT      -> H264 frame-based format is enumerated (looks fine)
VIDIOC_TRY/S_FMT     -> kernel returns sizeimage = 0  (the trap is set here)
VIDIOC_REQBUFS       -> request N buffers of sizeimage bytes  *** FAILS ***
```

`VIDIOC_REQBUFS` (and the subsequent `VIDIOC_QUERYBUF` / `mmap`) cannot allocate
zero-length capture buffers for a variable-size compressed stream. The driver
has no valid upper bound to allocate against, so buffer setup fails (or yields
unusably small buffers that the first real frame overruns). Streaming never
starts; `v4l2src` errors out at `REQBUFS`/`STREAMON` time, before any payload is
demuxed into NAL units.

This is **not** a `v4l2src` bug and **not** tunable from GStreamer caps. The
defect is in the device's descriptor, and `uvcvideo` faithfully reports what the
device declares. `v4l2src` has no property to override `sizeimage` against the
kernel's negotiated value — the buffer size is the kernel's to decide from the
descriptor.

### 1.3 Why both 5.10 and 6.6 fail identically

`dwMaxVideoFrameBufferSize=0` is consumed the same way by the `uvcvideo` buffer
path on both kernels in the CeraLive matrix:

| Kernel | uvcvideo behavior on DJI `dwMaxVideoFrameBufferSize=0` | Result |
|--------|-------------------------------------------------------|--------|
| 5.10   | `sizeimage` derived as 0; `REQBUFS` cannot size compressed buffers | capture fails |
| 6.6    | same descriptor-driven sizing; same `REQBUFS` failure | capture fails |

The Rockchip **decoder** generation differs across these kernels (`mppvideodec`
on 5.10; codec-specific `v4l2slh264dec`/`v4l2slh265dec` on 6.6 — README/AGENTS
decoder matrix), but that is the *decode* leg, downstream of capture. The
capture-side `uvcvideo`→`v4l2src` buffer-allocation failure is upstream of the
decoder and is identical on both. Changing the decoder does nothing for it.

### 1.4 What our own V4L2 probe already tells us

The element does **not** depend on V4L2 for capture, but it already performs a
cheap, read-only V4L2 sanity probe at `start()`
(`uvc_device.c:gst_libuvc_h264_src_v4l2_probe`):

```c
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
ioctl(fd, VIDIOC_TRY_FMT, &fmt);
gboolean available = (ret == 0 && fmt.fmt.pix.sizeimage > 0 &&
                      fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_H264);
```

The probe explicitly gates on **`sizeimage > 0`**. That is exactly the field DJI
poisons to `0`. So the element's existing instrumentation already encodes the
failure mode: on a DJI device this probe logs `V4L2 native H.264: unavailable`
precisely because `TRY_FMT` returns `sizeimage == 0` — the same `0` that would
sink `REQBUFS` under `v4l2src`. The probe is the cheap front-door check; the
`REQBUFS` failure is the same wall one step further in. The probe is non-fatal
and non-destructive — it issues exactly one `VIDIOC_TRY_FMT` and closes — so it
costs nothing and is the natural place any future "is v4l2src usable here?"
decision would read from.

> libuvc sidesteps this entirely: it does its own USB bulk/iso transfers in user
> space, reassembles frame-based payloads, and never asks the kernel to size a
> buffer from `dwMaxVideoFrameBufferSize`. The bad descriptor field is simply
> never used as an allocation bound. That is *why* userspace libuvc is the only
> reliable DJI path.

---

## 2. Where `v4l2src` COULD work — and what a split path would cost

### 2.1 The narrow window of viability

`v4l2src` is viable **only** for spec-compliant generic UVC cameras that:

1. advertise an H.264 or H.265 format `uvcvideo` recognizes
   (`V4L2_PIX_FMT_H264` / `V4L2_PIX_FMT_HEVC`), and
2. report a **non-zero, honest** `dwMaxVideoFrameBufferSize`, so `REQBUFS` can
   allocate correctly sized buffers, and
3. need none of the vendor-specific control or stream behavior this element
   provides for DJI hardware.

For such a camera a pipeline like
`v4l2src device=/dev/videoN ! video/x-h264 ! h264parse ! ...` will stream. The
element's own probe would log `available` for it (`sizeimage > 0`).

### 2.2 The cost of "v4l2src for generic, libuvc for DJI" routing

A hypothetical dual-path design — `v4l2src` for compliant cameras, libuvc for
DJI — buys nothing and adds real cost:

- **A new capability-detection front end.** Something must decide, per device,
  which path to take. The only reliable signal is essentially the probe we
  already have (`TRY_FMT` → `sizeimage > 0`), plus VID/PID heuristics. That
  decision logic is new surface that can misroute a device into the path that
  cannot drive it.
- **Two capture code paths to maintain, test, and harden.** Today there is one
  element with a hardware-independent mock suite (NAL parsing, PTS monotonicity,
  disconnect, reconnect, teardown, negotiation). A `v4l2src` leg means a second
  capture mechanism with its own failure modes (REQBUFS sizing, V4L2 buffer
  starvation, format-enum quirks) and its own test matrix — none of which the
  current mock-backed ctest suite covers.
- **Feature divergence between paths.** Everything in §3 (SPS/PPS injection,
  XU/PTZ mapping, silence-based disconnect, reconnect, PTS policy) exists in the
  libuvc path and is **absent** from a bare `v4l2src`. A device routed to
  `v4l2src` silently loses all of it, so the two paths are not behaviorally
  interchangeable from `cerastream`'s point of view.
- **No upside for the actual fleet.** The primary capture hardware is DJI, which
  is structurally excluded from `v4l2src` (§1). For the generic-UVC minority,
  the existing libuvc element **already** captures them — it is not DJI-only. So
  `v4l2src` would, at best, duplicate a path libuvc already serves while
  dropping that path's features. The net is strictly negative.

There is one legitimate, *non-capture* reason to keep V4L2 in view: the Rockchip
**decode** side genuinely is V4L2 (`v4l2slh264dec`/`v4l2slh265dec` on 6.6,
`mppvideodec` via MPP on 5.10). That is orthogonal to this spike — it is
downstream of `h264parse` and unaffected by how frames are *captured*.

---

## 3. Problems `v4l2src` would cause (capabilities lost vs the libuvc element)

Even on a camera where `v4l2src` *can* stream, switching to it forfeits
element-resident behavior that `cerastream` and the SRT delivery chain depend
on. Each item below is a concrete regression.

### 3.1 Loss of in-band SPS/PPS (and VPS) injection

`frame_pipeline.c` does work `v4l2src` does not. On every IDR it re-prepends the
cached parameter sets so each keyframe is independently decodable:

```c
case UNIT_FRAME_IDR:
    if (!self->had_idr || self->send_sps_pps) {
        buffer_offset = self->sps_length + self->pps_length;        // H.264
        if (self->frame_format == UVC_FRAME_FORMAT_H265)
            buffer_offset += self->vps_length;                      // + VPS for H.265
        ... gst_buffer_fill(VPS) ... fill(SPS) ... fill(PPS) ...
    }
```

It also caches SPS/PPS/VPS to disk on change (`store_spspps`, write-gated so a
GOP-repeated parameter set does not rewrite the flash each keyframe) and **gates
output on the first IDR** (`had_idr`) so the stream never starts mid-GOP with an
undecodable P-frame.

`v4l2src` emits the raw elementary stream exactly as the device sends it. For a
camera that only sends SPS/PPS once at stream start (common), a mid-stream SRT
join, packet loss, or a downstream decoder reset leaves the receiver with no
parameter sets and a stream it cannot decode. Recovering this would mean bolting
on extra downstream elements (e.g. relying on `h264parse config-interval=-1`/
`=1` to re-insert SPS/PPS) and an IDR-gating element — i.e. re-implementing in
the pipeline what the source already does in one place, and only partially
(h264parse can only re-send sets it has already *seen*; it cannot cache across
restarts the way the element's disk cache does).

### 3.2 Loss of XU / vendor PTZ control mapping

The element exposes `pan`/`tilt`/`zoom` GObject properties and a `set-ptz`
action signal, plus an opt-in Unix-domain control socket, all routed through
capability-gated libuvc control calls (`ptz_control.c`). DJI/UVC PTZ and many
vendor features live in **UVC Extension Units (XU)**, which `v4l2src` does not
surface at all. Driving them through V4L2 would require:

- `UVCIOC_CTRL_MAP` ioctls to map each vendor XU control onto a V4L2 control ID
  (a separate, error-prone registration step, often needing exact GUID + unit ID
  + selector + size, frequently root-only), and
- a parallel re-implementation of the clamping, the capability gate (silently
  ignore an axis the device does not report), the shared pan/tilt control
  re-send, and the JSON control-socket surface —

none of which `v4l2src` provides. The current programmatic control surface used
by cerastream/CeraUI (`g_object_set` / `set-ptz`) would simply stop existing and
would have to be rebuilt against `VIDIOC_S_CTRL` plus `UVCIOC_CTRL_MAP`.

### 3.3 Disconnect handling — worse, and kernel-version-dependent

The element treats sustained frame silence as a disconnect: it counts
consecutive `g_async_queue_timeout_pop` timeouts (1 s each) and after
`DISCONNECT_TIMEOUT_COUNT` (5) in a row infers the device is gone, posts
`GST_ELEMENT_ERROR(RESOURCE, READ)`, and lets cerastream react. This works
because libuvc in callback mode goes silent on unplug (confirmed by the
reconnect spike) — the element does not rely on any explicit kernel disconnect
signal.

`v4l2src` disconnect behavior is the kernel's, and it differs by version: on
**5.10** UVC mid-stream disconnect is historically unreliable (dequeue can hang
or return ambiguous errors rather than a clean EOS/error), whereas **6.6** is
more dependable. Adopting `v4l2src` makes our disconnect semantics
kernel-dependent again — re-opening exactly the kind of version-specific
fragility the element's own silence-based detector was written to neutralize.

### 3.4 Loss of in-element reconnect

`reconnect=true` performs a verified-safe native teardown
(`uvc_stop_streaming` → `uvc_close` → `uvc_unref_device`, never
`force_usb_release` before `uvc_close` — the double-close vector from the
reconnect spike), re-enumerates, re-resolves the `index` selector against a
fresh device list (so a `vid:pid`/`serial:` selector survives a replug even
though bus/address changed), reopens, re-runs
`uvc_get_stream_ctrl_format_size`, restarts streaming, and re-arms the IDR gate
and PTS baseline so the resumed stream waits for a fresh IDR with valid
parameter sets.

`v4l2src` has none of this. A replug that re-creates `/dev/videoN` (possibly
under a *different* N) means the element is bound to a stale node; recovery would
require external orchestration (udev watch → pipeline rebuild) and would still
lose the IDR-gate/PTS-rebaseline guarantees on resume.

### 3.5 PTS behavior diverges from the running-time contract

The element stamps PTS as **pipeline running-time** at frame arrival
(`ts = gst_clock_get_time(clock) - base_time`), keeps DURATION a nominal 1/fps
hint, never coerces a device's real cadence onto an idealized grid, and clamps
non-monotonic timestamps to `prev_pts + 1` across clock swap / relatch /
reconnect. This is the behavior the README A/V-sync note documents and that the
mux/SRT chain is tuned against.

`v4l2src` timestamps come from V4L2 buffer timestamps (monotonic or, depending
on driver, realtime), which is a *different* time base and a different
monotonicity story. Swapping it in changes the PTS contract for the whole
downstream chain — A/V sync against the Bluetooth-mic branch, mux behavior, SRT
latency budgeting — and would need re-validation end to end.

### 3.6 Consumer (`cerastream`) impact

`cerastream` consumes this element for both `InputKind::UvcH264` (negotiated to
`video/x-h264`) and `InputKind::UvcH265` (`video/x-h265`) via the
`libuvch26xsrc` dual-codec factory. It depends on the element's properties
(`index` device selection by ordinal/`vid:pid`/`serial`/`bus`, `pan`/`tilt`/
`zoom`, `reconnect`, the control socket) and on the `RESOURCE, READ` disconnect
error. A `v4l2src`-based source would change the element factory, the device
selection contract (V4L2 node index vs libuvc enumeration/serial), the control
surface, the error semantics, and the PTS base — i.e. a breaking interface
change for the consumer, in exchange for a path that still cannot drive the DJI
hardware (§1).

---

## 4. Recommendation

**Do not adopt `v4l2src` for capture.** Keep userspace libuvc as the single,
unified capture path for both DJI and generic UVC H.264/H.265 devices.

1. **DJI is structurally excluded.** `dwMaxVideoFrameBufferSize=0` sinks
   `VIDIOC_REQBUFS` on both 5.10 and 6.6. No GStreamer-side or caps-side knob
   fixes a bad device descriptor. This alone rejects `v4l2src` as a replacement.
2. **A split "generic→v4l2src, DJI→libuvc" path is net-negative.** libuvc
   already captures the compliant cameras too, so `v4l2src` would only duplicate
   a working path while dropping that path's features and adding a routing
   front-end and a second test matrix.
3. **The feature loss is severe and load-bearing:** in-band SPS/PPS/VPS
   injection, IDR gating, XU/vendor PTZ via properties + socket, silence-based
   disconnect, in-element reconnect, and the running-time PTS contract — all
   relied on by `cerastream` and the SRT chain — vanish with a bare `v4l2src`.
4. **Keep V4L2 in its correct role.** V4L2 stays exactly where it already is:
   the cheap, non-fatal `VIDIOC_TRY_FMT` capability probe at `start()`
   (`uvc_device.c`), and the **decode** leg downstream
   (`v4l2slh264dec`/`v4l2slh265dec` on 6.6, `mppvideodec`/MPP on 5.10). Neither
   is capture, and neither is changed by this verdict.

**Hardware-test caveat (non-blocking).** The DJI failure mode is established by
the descriptor analysis and the element's own `sizeimage > 0` probe gate, and is
sufficient to reject `v4l2src` as a capture replacement. If a future task wanted
to *quantify* the narrow generic-UVC window (which compliant cameras report an
honest `dwMaxVideoFrameBufferSize` and stream cleanly under `v4l2src`), that
specific sub-question would need a bench test with real compliant hardware. It
does not change this verdict — it would only characterize the small set of
devices for which `v4l2src` is even technically possible, and for which libuvc
already works.

---

VERDICT: NOT-VIABLE
