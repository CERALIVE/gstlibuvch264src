# gstlibuvch264src

GStreamer source element that pulls H.264 frames directly from DJI action cameras and UVC devices via libuvc. Developed by UnlimitedIRL; forked/maintained under CeraLive.

> **Security:** CVE-2026-1991 (null-deref in scan-streaming path) is fixed in the CeraLive fork at commit `eae7f49` (first shipped in tag `ceralive-v0.0.7.2`, carried forward in `ceralive-v0.0.7.9`, SHA `ada082b5009e38a89eb7cd6176683b508cd99ff5`) and also carried as `patches/cve-2026-1991-scan-streaming-nullguard.patch` for the upstream fallback path. Upstream libuvc is effectively dead (last commit 2024); the CeraLive fork at `https://github.com/CeraLive/libuvc.git` is the canonical dependency.

Parent manifest: [`../AGENTS.md`](../AGENTS.md)

---

## ROLE IN THE GROUP

Capture source element — feeds raw H.264 bitstream from DJI/UVC cameras into the cerastream pipeline. **Optional device-image component**: the image build may or may not include this plugin depending on capture hardware. HDMI capture paths bypass this element entirely.

Data flow position:
```
libuvch264src (this) → cerastream → srtla → irl-srt-server
```

---

## STRUCTURE

```
gstlibuvch264src/
├── libuvch264src/           # GStreamer plugin source (Meson build — canonical)
│   ├── src/                 # C source — split into cohesive modules
│   │   ├── gstlibuvch264src.c          # GObject boilerplate, properties, vmethods, plugin_init
│   │   ├── gstlibuvch264src.h          # Public element type/cast macros
│   │   ├── gstlibuvch264src_internal.h # Instance struct + GST_CAT_DEFAULT (shared across TUs)
│   │   ├── gstlibuvch264src_error.{c,h}# uvc_error_t → GST_ELEMENT_ERROR mapping helper
│   │   ├── frame_pipeline.{c,h}        # NAL parsing, frame_callback, PTS estimation
│   │   ├── spspps_cache.{c,h}          # SPS/PPS/VPS disk cache (path safety, resolution key)
│   │   ├── spspps_path.h               # Pure path-builder (no GObject dep, unit-testable)
│   │   ├── ptz_control.{c,h}           # PTZ probe/set helpers + control socket bind/unbind/thread
│   │   ├── uvc_device.{c,h}            # USB teardown helper + V4L2 capability probe
│   │   └── quirks.{c,h}                 # vid:pid quirk table: DOUBLE_PROBE + MAX_PIXEL_RATE; one row (Osmo Pocket 3, sets both)
│   ├── docs/notes/
│   │   ├── reconnect-spike.md          # Spike verdict: libuvc dead-handle teardown is SAFE
│   │   ├── bmaxpayload-analysis.md     # max-payload bandwidth tuning analysis
│   │   ├── dji-xu-investigation.md     # DJI XU control investigation (report only; no code shipped)
│   │   ├── v4l2src-spike.md            # v4l2src evaluation spike (report only; no code shipped)
│   │   ├── scr-investigation.md        # SCR-based PTS investigation (verdict: SCR-ABSENT; no code change)
│   │   ├── libuvc-fork-adr.md          # ADR: CeraLive fork as canonical libuvc dependency
│   │   └── camera-compat.md            # Mechanism-per-family compat matrix + field-triage + fork provenance
│   └── meson.build                     # Canonical production build
├── tests/                   # Hardware-independent ctest suite (mock-backed)
│   ├── mock_libuvc.{c,h}    # libuvc mock (~16 fns); env/API config; PTZ + descriptor support
│   ├── mock_libusb.{c,h}    # libusb mock for teardown double-close tests
│   ├── test_plugin_load.c   # Smoke: registration, factories, pads, index default
│   ├── test_mock_smoke.c    # gst-check: 10-buffer pipeline via mock
│   ├── test_device_select.c # Device selection: ordinal/vid:pid/serial/bus + index validation
│   ├── test_ptz.c           # PTZ properties + capability gate
│   ├── test_socket.c        # Control socket: default-off, per-instance path, mode 0600
│   ├── test_negotiate.c     # Caps negotiation: leak (LSAN), zero-format, framerate edge cases, inventory log
│   ├── test_usb_teardown.c  # USB teardown: single libusb_close, real interface count
│   ├── test_pts_thread_safety.c # PTS/clock race + frame throughput
│   ├── test_pts_monotonic.c # PTS monotonicity + restart IDR gate
│   ├── test_live_source.c   # LATENCY query, buffer OFFSET, SPS/PPS write-on-change
│   ├── test_sps_bounds.c    # SPS/PPS/VPS NAL copy bounds (heap overflow guard)
│   ├── test_nal_parse.c     # NAL parser: multi-slice, 3+4-byte start codes, size_t bounds
│   ├── test_au_alignment.c  # alignment=au contract: one buffer per access unit (AUD + AUD-less)
│   ├── test_cache.c         # SPS/PPS cache path safety + resolution key
│   ├── test_error_map.c     # uvc_error_t → GST_ELEMENT_ERROR mapping
│   ├── test_v4l2_probe.c    # V4L2 VIDIOC_TRY_FMT probe (non-fatal)
│   ├── test_compat.c        # API compatibility: property existence + type assertions
│   ├── test_cve_2026_1991.c # CVE-2026-1991 regression: null-deref guard in scan-streaming path
│   ├── test_cache_race.c    # SPS/PPS cache concurrent read/write race (TSan)
│   ├── test_transfer_buffers.c # transfer-buffers property: sentinel/clamp/reconnect re-arm, fork-only gated
│   ├── test_quirks.c        # vid:pid quirk lookup/limits, QUIRK_DOUBLE_PROBE, Osmo pixel-rate cap + double probe
│   ├── board/               # MANUAL, hardware-only; never registered with ctest
│   │   └── wedge-recovery.sh# Gated-SIGKILL wedge + real-libusb_reset_device recovery timing
│   ├── fuzz_nal.c           # NAL parser fuzz harness (libFuzzer entry point)
│   ├── tsan.suppressions    # TSan suppressions for third-party + baselined GMutex blind spots
│   └── tsan_pts.suppressions# TSan suppressions for PTS/clock GMutex (permanent blind spot)
├── patches/                 # libuvc patches for the upstream fallback path (LIBUVC_USE_FORK=OFF)
│   ├── cve-2026-1991-scan-streaming-nullguard.patch  # CVE-2026-1991 null-deref fix (upstream fallback)
│   ├── uvc15-support.patch  # UVC 1.5 support
│   ├── libuvc-h265-support.patch  # H.265 stream format support
│   └── README.md
├── CMakeLists.txt           # TEST-ONLY build: compiles plugin + full ctest suite
├── Dockerfile               # Reproducible build environment (pinned Debian bookworm + libuvc SHA)
└── README.md
```

> `libuvc/` is no longer vendored in-tree. By default (`LIBUVC_USE_FORK=ON`),
> `scripts/build-libuvc.sh` clones the CeraLive fork at the hardened SHA
> (`f3eda76` on `main`, PR #7 — the `uvc_close()` status-transfer + interface-release fix; supersedes tag `ceralive-v0.0.7.9`/`ada082b`, which is an ancestor) — no patch step needed. With
> `LIBUVC_USE_FORK=OFF`, it falls back to upstream v0.0.7
> (`68d07a00e11d1944e27b7295ee69673239c00b4b`) and applies the patches from
> `patches/` (including the CVE-2026-1991 null-guard). The Dockerfile and the
> top-level `CMakeLists.txt` both delegate to this script.

---

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Plugin element logic | `libuvch264src/src/gstlibuvch264src.c` |
| NAL parsing / PTS / frame callback | `libuvch264src/src/frame_pipeline.c` |
| Access-unit aggregation (`alignment=au`) | `libuvch264src/src/frame_pipeline.c` → `split_access_units()` |
| PTZ probe/set + control socket | `libuvch264src/src/ptz_control.c` |
| USB teardown + V4L2 probe | `libuvch264src/src/uvc_device.c` |
| Wedged-device USB port-reset recovery | `libuvch264src/src/gstlibuvch264src.c` → `gst_libuvc_h264_src_reset_silent_device()` |
| SPS/PPS cache | `libuvch264src/src/spspps_cache.c` |
| Error mapping helper | `libuvch264src/src/gstlibuvch264src_error.c` |
| vid:pid quirk seam (table + lookup) | `libuvch264src/src/quirks.c` |
| Meson build config | `libuvch264src/meson.build` |
| Build environment | `Dockerfile` |
| Reconnect feasibility verdict | `libuvch264src/docs/notes/reconnect-spike.md` |
| max-payload tuning analysis | `libuvch264src/docs/notes/bmaxpayload-analysis.md` |
| DJI XU investigation (report only) | `libuvch264src/docs/notes/dji-xu-investigation.md` |
| v4l2src evaluation spike (report only) | `libuvch264src/docs/notes/v4l2src-spike.md` |
| SCR/PTS investigation (verdict: SCR-ABSENT) | `libuvch264src/docs/notes/scr-investigation.md` |
| libuvc fork ADR | `libuvch264src/docs/notes/libuvc-fork-adr.md` |
| Camera compat matrix + field triage + fork provenance | `libuvch264src/docs/notes/camera-compat.md` |
| Example pipelines | `README.md` |

---

## PROPERTIES

All properties are on the `libuvch264src` (and `libuvch26xsrc` alias) element.

### `index` (string, default `"0"`)

Selects one device from the libuvc enumeration. Accepts four forms:

| Form | Example | Meaning |
|------|---------|---------|
| `"N"` | `"0"` | Ordinal into the enumerated list (default, backward-compatible) |
| `"vid:pid"` | `"1234:5678"` | Hex USB vendor:product ID |
| `"serial:<sn>"` | `"serial:CAM-001"` | Exact USB serial-number string |
| `"bus:<b>:<a>"` | `"bus:1:5"` | Decimal USB bus number and device address |

A malformed selector posts `GST_ELEMENT_ERROR(RESOURCE, SETTINGS)` and fails `start()` loudly — the old `atoi()` silent-select-0 trap is gone. `vid:pid` and `serial:` selectors survive a device replug (bus/address can change); `bus:` and ordinal selectors may resolve to a different physical device after replug.

### `pan` / `tilt` (int, range ±648000, default 0)

Absolute pan/tilt position in UVC arcseconds. Capability-gated: a set on an axis the device does not report is silently ignored. Pan and tilt share one UVC control, so setting one axis re-sends the other from its cached value. Readable at any time; returns the last successfully applied value.

### `zoom` (int, range 0..65535, default 0)

Absolute zoom as a UVC focal length. Capability-gated the same way as pan/tilt.

### `control-socket` (boolean, default `false`)

Enables the opt-in Unix-domain PTZ control socket. Default is **off** — nothing binds unless you set this to `true`. The old world-accessible `/tmp/libuvc_control` path is gone.

### `control-socket-path` (string, default `null`)

Explicit path for the control socket. When `null` (the default), the element auto-selects a per-instance path under `$XDG_RUNTIME_DIR`:

```
$XDG_RUNTIME_DIR/libuvch264src-<pid>-<seq>.sock
```

The `<seq>` counter is per-process-atomic, so two instances in the same process never collide. The socket is created with mode `0600`. If `XDG_RUNTIME_DIR` is unset and no explicit path is given, the bind fails non-fatally (a warning is logged; the media path continues).

Read this property back after `PAUSED` to discover the resolved path.

### `reconnect` (boolean, default `false`)

Opt-in in-element auto-reconnect on a mid-stream disconnect. Default is **off**: a disconnect always posts `GST_ELEMENT_ERROR(RESOURCE, READ)` and ends the stream. When set to `true`, the element first attempts a bounded-backoff teardown/reopen (see DISCONNECT / RECONNECT BEHAVIOR) and only errors out if every retry is exhausted. Gated on the Task 4 spike verdict (`libuvch264src/docs/notes/reconnect-spike.md`).

### `max-payload` (uint, range 0..4194304, default `0`)

USB payload transfer size hint in bytes (`dwMaxPayloadTransferSize`). `0` (the default) leaves the device-negotiated value unchanged. A nonzero value is clamped to `[512, 4194304]`, applied via UVC probe/commit with read-back, and falls back to the device-negotiated value if the device refuses it. Read-back reports the effective committed value. See `libuvch264src/docs/notes/bmaxpayload-analysis.md` for tuning guidance.

### `transfer-buffers` (uint, range 0..255, default `0`)

USB transfer buffer count hint: the number of USB transfer buffers `libuvc` submits per stream. `0` (the default, and the sentinel) leaves the library's default count unchanged — no device write at all. A nonzero value is clamped to `[2, 100]` and applied via the CeraLive fork's `uvc_set_transfer_buffers()` right before streaming starts, in both the initial `start()` and on every reconnect re-arm (the fork API rejects the call mid-stream, so it must precede `uvc_start_streaming()`). Read-back reports the effective (clamped) value once applied; before that it reports the requested value. Requires the CeraLive libuvc fork (backs fork item A2, `libuvch264src/docs/notes/camera-compat.md` §3); on upstream libuvc (`LIBUVC_USE_FORK=OFF`) a nonzero request is a no-op with one warning, and the property itself is otherwise harmless to set on either build.

### `reset-settle-max-ms` (uint, range 0..120000, default `8000`)

Budget, in milliseconds, for the element's **own** readiness loop after a port reset — re-enumeration polling, the reopen retries, and the wait for the first real frame. It is a budget, not a delay: the recovery returns the instant frames are actually flowing, so a device that comes back quickly is not made to wait. When it is spent the element stops starting new attempts and falls through to the usual `RESOURCE/READ` disconnect error. Also bounds the re-enumeration poll on a `start()` that had to force-clean a previous session.

**It does not bound the total.** `uvc_stop_streaming()` and `uvc_close()` are synchronous and libuvc exposes no interruption seam, so on a device that is still re-enumerating the teardown between attempts can push the total well past this value — measured **21880 ms and 21893 ms against an 8000 ms budget** on two independent hardware runs. Size it for the fast path; the worst-case tail is teardown-bound, not policy-bound. See DISCONNECT / RECONNECT BEHAVIOR.

### `reset-rearm-frames` (uint, range 1..100000, default `30`)

Frames the device must deliver after a recovery before the one-shot port reset re-arms for a LATER wedge. The default is ~1 s at 30 fps. Re-arming on the first frame back would let a device that emits one frame and immediately re-wedges reset the port in an endless loop.

### Action signal: `set-ptz(pan, tilt, zoom)` → boolean

Drives all three PTZ axes in one emission. Each axis is applied only when the device reports it. Returns `TRUE` if at least one supported axis was driven and every attempted set succeeded.

---

## PTZ CONTROL SURFACE

Two independent surfaces, both capability-gated:

**Native GObject properties (always available, no socket needed)**
Set `pan`, `tilt`, `zoom` via `g_object_set()` or `gst-launch-1.0 ... pan=N`. The `set-ptz` action signal drives all three in one call. These are the preferred interface for programmatic control from cerastream/CeraUI.

**Opt-in Unix-domain socket (default off)**
Set `control-socket=true` to enable. The socket accepts JSON commands for `PAN_TILT`, `ZOOM`, `GET_POSITION`, and `GET_CAPABILITIES`. Routes through the same `ptz_set_pan/tilt/zoom` helpers as the native props — same clamping, same capability gate, same locking. A consumer must read the resolved `control-socket-path` property (or set an explicit path) after enabling the socket.

---

## DISCONNECT / RECONNECT BEHAVIOR

**Wedged-device recovery (always on, one-shot per silence episode).** Sustained silence has TWO causes that are indistinguishable from inside `create()`: the device was unplugged, or it is still fully present but **wedged** — enumerated, answering every control transfer (descriptors, probe/commit, PTZ), yet delivering nothing on the streaming endpoint. Measured on a DJI Osmo Pocket 3 (`2ca3:0023`) after the holding process died without `uvc_close()`; reproduced identically through libuvc AND through the kernel `uvcvideo` driver, which is what proves it is device state and not an element bug. **A close/reopen does not clear it — only a USB port reset does.** So before claiming a disconnect (which is factually wrong for a device still on the bus), `create()` runs `gst_libuvc_h264_src_recover_wedged_device()`: ONE `libusb_reset_device()` on the live handle, then a **readiness-driven** return to streaming — poll `uvc_find_devices()` on a micro-backoff until the device re-enumerates, reopen, restart, and require an **actual delivered frame**. Notes:

- `LIBUSB_ERROR_NOT_FOUND` counts as **success** — libusb returns it when the reset re-enumerated the device, which is the outcome we want. Any other non-zero status means the port reset did not happen, so the device really is unreachable and the disconnect error surfaces as before.
- **A successful `uvc_start_streaming()` is not proof of recovery.** A reopen issued too soon after the reset returns OK from both `uvc_open()` and `uvc_start_streaming()` and then delivers zero frames — indistinguishable from the wedge just cleared. libuvc exposes no readiness API, so a **delivered frame is the readiness signal**; the recovery waits for one and, if the reopen came too early, tears it down and tries again. The proving frame is pushed back to the queue front, so it is never dropped.
- **No device-measured timing constant is encoded anywhere.** The recovery ends the moment frames actually flow, so a device that comes back in 200 ms costs 200 ms. `reset-settle-max-ms` (default 8000) budgets the element's own readiness loop, not the synchronous libuvc teardown between attempts (see the property docs — measured worst case ~22 s against an 8 s budget). The re-enumeration poll backs off 25 → 200 ms and is interruptible, so a NULL/PAUSED transition never waits it out.
- **The libuvc context is re-created on the recovery path.** MEASURED: after a real port reset the device re-enumerates, and a context held open across that reset can no longer open it — a freshly started process streamed again 14.4 s in while the element, on its original context, could not reopen at all inside a 30 s budget. `uvc_exit()` + `uvc_init()` is what makes the recovery equivalent to the fresh process that demonstrably works; it took the measured recovery from *never* to **288 ms**.
- It is **still one port reset per silence episode, not the `reconnect` ladder**. Retries inside the budget are reopens only — the reset itself never repeats. `reconnect` remains the property that buys the 1/2/4/8/16 s retry schedule; when `reconnect=true` that path runs instead and is unchanged.
- The one-shot re-arms only after the device has PROVEN it recovered (`reset-rearm-frames`, default 30 — ~1 s at 30 fps). Re-arming on the first frame back lets a device that emits one frame and re-wedges reset the port forever.
- Cost to a genuinely absent device: nothing — the reset fails and the error surfaces immediately, without spending the budget.
- Real-hardware coverage: `tests/board/wedge-recovery.sh` (manual, board-only, never registered with ctest) induces a real wedge with a gated SIGKILL and measures reset-to-advancing-frames against the bound.

**Disconnect detection (always on):** When the UVC device is unplugged mid-stream, libuvc stops delivering frames silently — in callback mode it does **not** invoke the callback with a NULL frame, it simply goes quiet (Task 4 spike). `create()` therefore infers a disconnect from sustained silence: it counts consecutive `g_async_queue_timeout_pop` timeouts (each `TIMEOUT_DURATION` = 1 s), and after `DISCONNECT_TIMEOUT_COUNT` (5) in a row — i.e. ~5 s with no frame — it treats the device as gone. The counter resets on every real frame and in `start()`, so an isolated gap never trips it. On a confirmed disconnect with `reconnect=false` (the default), it posts `GST_ELEMENT_ERROR(RESOURCE, READ)` and returns `GST_FLOW_ERROR`; downstream (cerastream) handles the error.

**Reconnect (opt-in, default off):** With the `reconnect` property set to `true`, a confirmed disconnect first triggers an in-element reconnect before any error is posted. The path uses the spike's verified **native** teardown — `uvc_stop_streaming()` → `uvc_close()` → `uvc_unref_device()` (the callback thread joins cleanly and the libusb handle is closed exactly once) — then re-enumerates and re-resolves the `index` selector against a fresh device list (bus/address can change across a replug; a `vid:pid`/`serial:` selector survives it, a `bus:`/ordinal one may resolve to a different device), reopens, re-runs `uvc_get_stream_ctrl_format_size` with the negotiated geometry, and restarts streaming. Retries use bounded exponential backoff (1, 2, 4, 8, 16 s; `RECONNECT_MAX_RETRIES` = 5); the backoff is interruptible so a state change to NULL/PAUSED tears down promptly. If every retry is exhausted, it falls back to the disconnect error above. On success the IDR gate and PTS baseline are re-armed so the resumed stream waits for a fresh IDR.

**Critical teardown constraint:** `force_usb_release()` must NOT be called before `uvc_close()` — including on the reconnect path. The spike proved `force_usb_release()` + `uvc_close()` double-closes the libusb handle. The element's teardown (in `stop()` and reconnect) lets `uvc_close()` own the single `libusb_close()` call; `force_usb_release()` only drops interface claims on the still-open handle.

---

## OUTPUT BUFFER CONTRACT (`alignment=au`)

Both pad templates advertise `alignment=(string)au`, so **every `GstBuffer` the element pushes is exactly one access unit** — one displayed picture, however many NAL units the device split it into. Downstream (`h264parse`, `v4l2slh264dec`/`mppvideodec`, the muxers) trusts that claim to find frame boundaries; the element must therefore honour it rather than merely assert it.

`frame_callback()` parses one libuvc delivery into NAL units, partitions those units into access units (`split_access_units()`), and emits ONE buffer per access unit. Boundary detection, in priority order:

- **AUD present** — an Access Unit Delimiter (H.264 `nal_unit_type` 9, H.265 `AUD_NUT` 35, both mapped to `UNIT_AUD`) *is* by definition the first NAL of its access unit, so it is an exact boundary. No heuristic involved.
- **AUD absent** — the standard fallback: a slice NAL that opens a new picture ends the access unit that already holds one. "Opens a new picture" is read from the first bit of the slice payload — H.264's `first_mb_in_slice` is `ue(v)`, whose value 0 is the single bit `1`, and H.265's `first_slice_segment_in_pic_flag` is a raw `u(1)` — so a set top bit on the first payload byte means first-slice. Emulation prevention cannot disturb that byte (a `0x03` is only inserted after two `0x00` bytes, and the preceding NAL header is non-zero for every slice).
- Any non-VCL run (SEI, parameter sets) immediately preceding a new picture's first slice belongs to the **following** access unit, so the cut is placed at the head of that run.
- A device that emits neither an AUD nor a decodable first-slice bit never splits: the whole delivery becomes one access unit. That is the same grouping a single-picture delivery gets, and it is never a mid-picture cut.

**Behaviour that did NOT change:**

- Single-slice 1080p — the overwhelmingly common case — is byte-identical to the old per-NAL path: its access unit is one slice, so the one emitted buffer holds exactly the delivered bytes. Pinned by the `au_single_slice_characterization` ctest case, which was written and proven green BEFORE the aggregation landed.
- Parameter sets are still consumed from the wire and re-prepended from the cache. They are written immediately **before** the access unit's first IDR slice, never at the head of the buffer, so an AUD stays the very first NAL of its access unit.
- The pre-first-IDR gate, the SPS/PPS/VPS bounds clamp, and the write-on-change cache policy are unchanged.

**PTS convention.** An aggregated access unit carries the arrival running-time of the delivery it came from — identically the PTS its **first** slice would have been stamped with under the per-NAL path, since every NAL of one delivery shares a single arrival instant. `GST_BUFFER_OFFSET` is now an access-unit counter rather than a NAL counter; for the single-slice case the sequence is unchanged. The `prev_pts` monotonic clamp no longer fires for slices of one picture (they are aggregated); it still covers a delivery that carried more than one access unit, whose AUs share an arrival `ts`.

Regression-guarded by `tests/test_au_alignment.c` (`au_single_slice_characterization`, `au_multi_slice_aud`, `au_aud_less_fallback`).

---

## V4L2 CAPABILITY PROBE

At `start()`, after `uvc_open()` succeeds, the element issues one `VIDIOC_TRY_FMT` ioctl against `/dev/video<N>` (where N is the device ordinal). This is a cheap, non-destructive probe — it does not change any device state. The result is logged via `GST_INFO_OBJECT`:

- `"V4L2 native H.264: available"` — kernel V4L2 driver reports H.264 support
- `"V4L2 native H.264: unavailable"` — driver present but H.264 not reported
- `"V4L2 probe unavailable: cannot open /dev/videoN"` — no V4L2 node at that index

The probe is **non-fatal** in all cases. A mismatch between the UVC ordinal and the V4L2 node index just logs "unavailable" and the element continues normally.

---

## BUILD

### Production build (Meson, canonical)

```bash
# 1. Build libuvc (CeraLive fork, default) — no patch step needed
scripts/build-libuvc.sh

# To use upstream v0.0.7 + patches fallback instead:
# LIBUVC_USE_FORK=OFF scripts/build-libuvc.sh

# 2. Build plugin
meson setup build libuvch264src/
cd build && meson compile && meson install

# 3. Move .so to system GStreamer path (multiarch-aware)
MULTIARCH=$(gcc -print-multiarch)
sudo mv /usr/local/lib/${MULTIARCH}/gstreamer-1.0/libgstlibuvch264src.so \
        /lib/${MULTIARCH}/gstreamer-1.0/
sudo cp /usr/local/lib/libuvc.* /usr/lib/${MULTIARCH}/
```

`$(gcc -print-multiarch)` resolves to `aarch64-linux-gnu` on arm64, `x86_64-linux-gnu` on amd64, etc. Do not hardcode the arch string.

Rockchip decoder/encoder reference:

| Kernel | H.264 decoder | H.265 decoder | Encoder (both codecs) |
|--------|---------------|---------------|-----------------------|
| 5.10   | `mppvideodec` | `mppvideodec` | `mpph264enc` / `mpph265enc` |
| 6.6    | `v4l2slh264dec` | `v4l2slh265dec` | `mpph264enc` / `mpph265enc` |

On kernel 5.10, `mppvideodec` handles both H.264 and H.265 via the Rockchip MPP layer. On kernel 6.6, the V4L2 stateless decoders are codec-specific.

### Reproducible Docker build

The `Dockerfile` pins both the base image and the libuvc source:

```
FROM debian:bookworm-slim@sha256:60eac759739651111db372c07be67863818726f754804b8707c90979bda511df
```

libuvc is fetched via `scripts/build-libuvc.sh` (fork mode by default, SHA `f3eda76` on `main`). The arch matrix fails loudly on unknown `TARGETARCH` values — no silent fallback.

**Two stages: pinned Debian bookworm `build`, then `FROM scratch` `runtime`.** The release recipe (`publish-release.yml`) exports the *final* stage wholesale (`buildx --output type=local,dest=build` → `fpm build/usr/=/usr/`). The `runtime` stage MUST stay `FROM scratch`, carrying ONLY the plugin payload that the `build` stage stages under `/out`: `usr/lib/<triplet>/gstreamer-1.0/libgstlibuvch264src.so` + `usr/lib/<triplet>/libuvc.so*` (the symlink chain; `libuvc.a`/`.pc` are build-only and excluded). Do NOT switch `runtime` back to a distro base to add runtime deps — that exports the entire distro `/usr` and produced a ~56 MB `.deb` that dpkg-file-conflicts with `coreutils`/`libc` on install. GStreamer/libusb/libjpeg are runtime deps from the target system (`Depends: libgstreamer1.0-0`, `libusb-1.0-0`, `libjpeg62-turbo`, `libc6 (>= 2.36)`), not bundled in the image. The base image intentionally matches the device image's Debian bookworm ABI so the plugin cannot pick up Ubuntu 24.04-only symbols such as `GLIBC_2.38` or `libjpeg.so.8`.

---

## TEST

Hardware-independent ctest suite. Two build shapes:

**Mock-backed plugin (`.so` loaded via `GST_PLUGIN_PATH`):** `test_plugin_load`, `test_mock_smoke` (+ `_asan`, `_tsan` variants). The mock plugin links the element TUs against `mock_libuvc.c` instead of real libuvc.

**Static-registration (element TUs + mock linked into one exe):** all other test targets. Mock state is in-process, so counters and config are directly readable without env vars.

```bash
# Run the full suite (with sanitizers)
cmake -B build -DENABLE_SANITIZERS=ON && cmake --build build && ctest --test-dir build --output-on-failure

# Run without sanitizers (faster)
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure

# Run a specific target
ctest --test-dir build -R "ptz_properties|ptz_capability_gate"
```

**TSan note:** `GST_OBJECT_LOCK` is a `GMutex` implemented with a raw futex in uninstrumented GLib. Under `ignore_noninstrumented_modules=1`, TSan cannot see the happens-before relationship, so it reports correctly-locked PTS/clock accesses as races. These are permanent TSan blind spots (not bugs), baselined in `tsan_pts.suppressions`. The behavioral deadlock/throughput tests (`pts_thread_safety`, `frame_throughput`) provide real regression coverage that the suppressions cannot mask.

**ASAN note:** `detect_leaks=0` is set for the mock-smoke variants (GStreamer one-time global allocs are noisy). The negotiate LSAN test uses `detect_leaks=1` with a targeted `__lsan_do_recoverable_leak_check()` after a warm-up window that swallows GStreamer's one-time globals.

**Dual-codec status [EXISTS].** Both H.264 and H.265 pad templates are present and asserted by the test suite. `cerastream` uses this element for both `InputKind::UvcH264` (negotiated to `video/x-h264`) and `InputKind::UvcH265` (negotiated to `video/x-h265`). The `libuvch26xsrc` factory alias reflects this dual-codec capability.

### Hardware-Independent Test Scope

The entire ctest suite is **mock-backed** — `tests/mock_libuvc.c` stands in for libuvc and `tests/mock_libusb.c` for libusb, so CI needs no UVC camera. This bounds what the suite can and cannot prove:

**The suite proves (in software, deterministically):**
- Element registration, pad templates, property/signal surface, and caps negotiation (`test_plugin_load`, `test_compat`, `test_functional`, `test_negotiate`).
- The pure logic that does NOT depend on a real device: the Annex-B NAL parser and its count/overflow bounds (`test_nal_parse` — including the `overflow` truncation-warning and `count_bound` suites), the SPS/PPS path builder, cache-key snapshot, and the cache file-open NULL/missing-file path (`test_cache`, `test_live_source` `spspps_key_snapshot`/`cache_open_null_path`).
- Concurrency/teardown invariants observable in-process under sanitizers: the PTS/clock lock (`test_pts_thread_safety` TSan), the SPS/PPS-bounds clamp and cache index race (ASan/TSan), USB single-`libusb_close` teardown (`test_usb_teardown`), and the CVE-2026-1991 null-guard against the vendored libuvc.
- Frame-callback-driven behavior fed by crafted access units through the mock: PTS monotonicity, IDR gating, write-on-change caching, disconnect/unlock lifecycle.
- The `alignment=au` output-buffer contract (`test_au_alignment`): a multi-slice picture aggregates into ONE buffer both with and without an AUD in the bitstream, a second picture in the same delivery starts a new buffer, and single-slice 1080p stays byte-identical to the pre-aggregation path.
- The `transfer-buffers` property contract (`test_transfer_buffers`: sentinel/clamp/reconnect re-arm, fork-only cases gated behind `TB_API_AVAILABLE` so the same test binary stays green on both `LIBUVC_USE_FORK=ON` and `OFF`) and the vid:pid quirk table (`test_quirks`: pure lookup/limits resolution, `QUIRK_DOUBLE_PROBE` probe count, the shipped Osmo `QUIRK_MAX_PIXEL_RATE` cap, and a red/green pair driving `negotiate()` against the Osmo's real advertised H.264 ladder — one case pins that an UNquirked device still picks the top mode 3840x2160@60, the other that the quirked Osmo lands on its capped ceiling 3840x2160@30 — plus `quirks_osmo_row_double_probes`, which pins that the shipped Osmo row probes TWICE and the cap still lands the same mode) and the negotiation-failure descriptor inventory (`test_negotiate`'s `negotiate_inventory_logged` case).

**Hardware-only, run by hand (NOT in ctest):** `tests/board/wedge-recovery.sh` induces a real wedge on a board — a gated SIGKILL of a holder that is provably streaming, matching the kill discipline the wedge investigation used — then measures reset-to-advancing-frames through the REAL `libusb_reset_device()` path and asserts it against `reset-settle-max-ms`, with a second USB port as a negative control. It is deliberately absent from `tests/CMakeLists.txt` so it can never run in CI, and skips (exit 77) unless `CERALIVE_BOARD_TEST=1`.

**The suite does NOT prove (requires real hardware — out of scope here):**
- Actual USB enumeration, `uvc_open()`/streaming against a physical DJI/UVC camera, real bandwidth at a given `max-payload`, or real PTZ motion on a device.
- Whether a given camera actually emits multi-slice pictures or Access Unit Delimiters. The AU-alignment cases prove the element's grouping POLICY against crafted bitstreams; which shape a real DJI/UVC device puts on the wire at 1080p30 vs 2160p30 comes only from a board capture.
- The V4L2 `VIDIOC_TRY_FMT` probe result for a real `/dev/videoN` (the test only asserts the probe is non-fatal when the node is absent).
- Mid-stream physical replug/reconnect timing (the backoff schedule is asserted via a test hook, not a real unplug).
- Real reset-to-advancing-frames timing. The mock pins the readiness POLICY (poll, reopen, require a frame, honour the bound) via the `MOCK_UVC_FRAME_SILENT` mode and the reset/poll hooks; the actual recovery duration on hardware comes only from `tests/board/wedge-recovery.sh`.

When adding tests, keep them inside the mock-coverable boundary above — assert software behavior the mock can deterministically drive, never a hardware outcome the mock cannot model. Real-hardware validation tracks separately (see `cerastream/docs/notes/hardware-validation.md` for the device-class profiles).

---

## VERSION SCHEME

**CalVer derivation: git tag only (no source file).**

The `.deb` version is derived **purely from git tags** at publish time via the `publish-release.yml` workflow. There is no separate `VERSION` file by design.

**Authoritative version source:** `.github/workflows/publish-release.yml` (job `calculate-version`)

**Scheme:** `YYYY.MINOR.PATCH` where:
- `YYYY` = current year (UTC)
- `MINOR` = current month (UTC, no zero-pad; e.g., `6` for June)
- `PATCH` = monotonic counter per month (incremented from git tag history)

**Example:** `2026.6.2` (June 2026, patch 2 — the hardening release)

**Tag format:** `v<VERSION>` (stable) or `v<VERSION>-beta.<N>` (beta)
- Stable: `v2026.6.2`
- Beta: `v2026.6.3-beta.1`

**FPM .deb version:** The `VERSION` env var from `calculate-version` is passed directly to FPM's `-v` flag (line 99 in `publish-release.yml`), producing `.deb` packages with CalVer versions like `gstreamer1.0-libuvch264src_2026.6.2_arm64.deb`.

**No version file needed.** The workflow calculates the version at publish time from the git tag history; there is no tracked `VERSION` file in the repo. This is intentional — the single source of truth is the git tag namespace (`v*`).

---

## ANTI-PATTERNS

- Do NOT link against system libuvc if it exists; the pinned fork/upstream copy is intentional for version pinning.
- Do NOT modify `scripts/build-libuvc.sh` SHA constants without updating both `FORK_SHA` and `UPSTREAM_SHA` together — they are the single source of truth for the dependency.
- Do NOT hardcode `aarch64-linux-gnu` in build paths — use `$(gcc -print-multiarch)`.
- Do NOT reintroduce a fixed, device-measured settle constant between the port reset and the reopen. It was measured on one DJI Osmo Pocket 3 and shipped as `RESET_SETTLE_MS` (4 s); it both over-waited on fast devices and burned the single reopen on slow ones. The replacement polls re-enumeration and requires a delivered frame, bounded by `reset-settle-max-ms`.
- Do NOT treat a successful `uvc_start_streaming()` as proof the device recovered — it returns OK on a still-wedged device. Only a delivered frame proves it.
- Do NOT present `reset-settle-max-ms` as a hard upper bound on recovery time. It bounds the element's own retry loop only; the synchronous libuvc teardown between attempts is uninterruptible and has been measured pushing the total to ~22 s against an 8 s budget.
- Do NOT drop the `uvc_exit()`/`uvc_init()` on the recovery path — a libuvc context held across a port reset cannot reopen the re-enumerated device, and without the refresh the recovery never completes.
- Do NOT treat `LIBUSB_ERROR_NOT_FOUND` from `libusb_reset_device()` as a failure — it means the reset re-enumerated the device, which is success.
- Do NOT re-arm the port-reset one-shot on the first frame after a recovery; a device that emits one frame and re-wedges would then reset the port in an endless loop.
- Do NOT give the reset-recovery path the full `reconnect` retry ladder — the exhaustion tests assert an exact reopen-attempt count, and `reconnect` is the property that buys the ladder.
- Do NOT call `force_usb_release()` before `uvc_close()` — it was a double-free/UAF vector; the fix lets `uvc_close()` own the single `libusb_close()`.
- Do NOT push one `GstBuffer` per NAL unit. The pad templates advertise `alignment=au`; the element must emit one buffer per ACCESS UNIT. Splitting a multi-slice picture across buffers that each claim to be a whole access unit mis-frames every downstream consumer that trusts the caps.
- Do NOT "fix" the `alignment=au` mismatch by weakening the caps to `alignment=nal`. Downstream reads the contract; the contract is correct and the emitter was not.
- Do NOT write the cached parameter sets at the head of an aggregated access-unit buffer. They go immediately before the AU's first IDR slice, so an AUD — which must be the very first NAL of its access unit — keeps its position.
- Do NOT enable `control-socket` by default or fall back to a world-accessible path when `XDG_RUNTIME_DIR` is unset — the socket must be opt-in and per-instance.
- Do NOT set PTZ properties outside the param-spec range in tests — GObject emits a range warning that gst-check turns into a longjmp, skipping teardown and hanging the process.
- Do NOT raise a `QUIRK_MAX_PIXEL_RATE` cap on the strength of a descriptor, a datasheet, or a successful `uvc_get_stream_ctrl_format_size()`. Only frames that actually ADVANCE on real hardware justify a higher cap; the cap is deliberately parked at the last CONFIRMED-GOOD rate, not the last known-bad one, because a cap set too low only costs resolution while a cap set too high costs the whole stream. (The Osmo Pocket 3 row is now capped at `3840x2160x30` = `248832000u` — raised from `62208000u` only after 4K@30 was captured through this element on 2026-07-30: 300/300 access units in ~10.8 s, SPS-verified `3840x2160`/`high`/`5.2`, zero errors, reproduced twice on board `192.168.78.131`. That is the bar. 4K@60/50/48 stay capped out — an uncapped build did stream 4K@60 that day, but the original zero-frame observation was never explained; see `camera-compat.md` §2 Step 5.)
- Do NOT read an `Unable to get stream control: Invalid mode` failure as a caps-logic or descriptor problem before checking what mode the device last committed. libuvc rejects the mode when the device's SET_CUR/GET_CUR readback disagrees, and a camera that answers the first probe from its previously committed mode fails EVERY negotiation that asks for a larger mode — deterministically, though it looks intermittent in the field. That is what `QUIRK_DOUBLE_PROBE` is for; the Osmo Pocket 3 row sets it.
- Do NOT "fix" the `quirks_ladder_no_quirk_picks_4k60` test because it asserts the buggy 3840x2160@60 outcome. That is deliberate: it pins the untouched max-area-then-max-fps behavior that every camera WITHOUT a quirk row still gets, and it is the control half of the pair whose other half proves the Osmo cap works.
- Do NOT special-case a device inside `gst_libuvc_h264_negotiate()` (`if (vid == ... && w == 3840 ...)`). The quirk table exists so device knowledge stays data-driven — one row, no branching in the selection loop.
- This plugin is **not** in the device image REPOS list by default — don't assume it's always present on device.
