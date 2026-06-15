# gstlibuvch264src

GStreamer source element that pulls H.264 frames directly from DJI action cameras and UVC devices via libuvc. Developed by UnlimitedIRL; forked/maintained under CeraLive.

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
│   │   └── uvc_device.{c,h}            # USB teardown helper + V4L2 capability probe
│   ├── docs/notes/
│   │   └── reconnect-spike.md          # Spike verdict: libuvc dead-handle teardown is SAFE
│   └── meson.build                     # Canonical production build
├── tests/                   # Hardware-independent ctest suite (mock-backed)
│   ├── mock_libuvc.{c,h}    # libuvc mock (~16 fns); env/API config; PTZ + descriptor support
│   ├── mock_libusb.{c,h}    # libusb mock for teardown double-close tests
│   ├── test_plugin_load.c   # Smoke: registration, factories, pads, index default
│   ├── test_mock_smoke.c    # gst-check: 10-buffer pipeline via mock
│   ├── test_device_select.c # Device selection: ordinal/vid:pid/serial/bus + index validation
│   ├── test_ptz.c           # PTZ properties + capability gate
│   ├── test_socket.c        # Control socket: default-off, per-instance path, mode 0600
│   ├── test_negotiate.c     # Caps negotiation: leak (LSAN), zero-format, framerate edge cases
│   ├── test_usb_teardown.c  # USB teardown: single libusb_close, real interface count
│   ├── test_pts_thread_safety.c # PTS/clock race + frame throughput
│   ├── test_pts_monotonic.c # PTS monotonicity + restart IDR gate
│   ├── test_live_source.c   # LATENCY query, buffer OFFSET, SPS/PPS write-on-change
│   ├── test_sps_bounds.c    # SPS/PPS/VPS NAL copy bounds (heap overflow guard)
│   ├── test_nal_parse.c     # NAL parser: multi-slice, 3+4-byte start codes, size_t bounds
│   ├── test_cache.c         # SPS/PPS cache path safety + resolution key
│   ├── test_error_map.c     # uvc_error_t → GST_ELEMENT_ERROR mapping
│   ├── test_v4l2_probe.c    # V4L2 VIDIOC_TRY_FMT probe (non-fatal)
│   ├── tsan.suppressions    # TSan suppressions for third-party + baselined GMutex blind spots
│   └── tsan_pts.suppressions# TSan suppressions for PTS/clock GMutex (permanent blind spot)
├── patches/                 # libuvc v0.0.7 patches (UVC 1.5 + H.265), applied at build
├── CMakeLists.txt           # TEST-ONLY build: compiles plugin + full ctest suite
├── Dockerfile               # Reproducible build environment (pinned ubuntu:24.04 + libuvc SHA)
└── README.md
```

> `libuvc/` is no longer vendored in-tree — it is cloned at the pinned SHA
> (`68d07a00e11d1944e27b7295ee69673239c00b4b`, upstream v0.0.7) and patched at
> build time. The Dockerfile does this for the production image; the top-level
> `CMakeLists.txt` does the same via `FetchContent` for the test build.

---

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Plugin element logic | `libuvch264src/src/gstlibuvch264src.c` |
| NAL parsing / PTS / frame callback | `libuvch264src/src/frame_pipeline.c` |
| PTZ probe/set + control socket | `libuvch264src/src/ptz_control.c` |
| USB teardown + V4L2 probe | `libuvch264src/src/uvc_device.c` |
| SPS/PPS cache | `libuvch264src/src/spspps_cache.c` |
| Error mapping helper | `libuvch264src/src/gstlibuvch264src_error.c` |
| Meson build config | `libuvch264src/meson.build` |
| Build environment | `Dockerfile` |
| Reconnect feasibility verdict | `libuvch264src/docs/notes/reconnect-spike.md` |
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

**Disconnect:** When the UVC device is unplugged mid-stream, libuvc stops delivering frames silently (no NULL-frame callback in callback mode). The element detects this via a bounded `g_async_queue_timeout_pop` in `create()`. On timeout with no frames, it posts `GST_ELEMENT_ERROR(RESOURCE, READ)` to the bus and returns `GST_FLOW_ERROR`. Downstream (cerastream) handles the error.

**Reconnect:** Opt-in, default off. The reconnect spike (`libuvch264src/docs/notes/reconnect-spike.md`) confirmed that native libuvc teardown after `LIBUSB_TRANSFER_NO_DEVICE` is **SAFE** — `uvc_stop_streaming()` → `uvc_close()` does not deadlock, the callback thread joins cleanly, and the libusb handle is closed exactly once. In-element reconnect is therefore feasible when enabled.

**Critical teardown constraint:** `force_usb_release()` must NOT be called before `uvc_close()`. The element's teardown now lets `uvc_close()` own the single `libusb_close()` call; `force_usb_release()` only drops interface claims on the still-open handle.

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
# 1. Clone and patch libuvc at the pinned SHA
git init libuvc && cd libuvc
git remote add origin https://github.com/libuvc/libuvc.git
git fetch --depth 1 origin 68d07a00e11d1944e27b7295ee69673239c00b4b
git checkout FETCH_HEAD
# Apply patches from ../patches/
cd ..

# 2. Build libuvc
cd libuvc && cmake . && make && sudo make install && cd ..

# 3. Build plugin
meson setup build libuvch264src/
cd build && meson compile && meson install

# 4. Move .so to system GStreamer path (multiarch-aware)
MULTIARCH=$(gcc -print-multiarch)
sudo mv /usr/local/lib/${MULTIARCH}/gstreamer-1.0/libgstlibuvch264src.so \
        /lib/${MULTIARCH}/gstreamer-1.0/
sudo cp /usr/local/lib/libuvc.* /usr/lib/${MULTIARCH}/
```

`$(gcc -print-multiarch)` resolves to `aarch64-linux-gnu` on arm64, `x86_64-linux-gnu` on amd64, etc. Do not hardcode the arch string.

Rockchip decoder note: kernel 5.10 → `mppvideodec`; kernel 6.6 → `v4l2slh264dec`.

### Reproducible Docker build

The `Dockerfile` pins both the base image and the libuvc source:

```
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54
```

libuvc is fetched by SHA (`68d07a00e11d1944e27b7295ee69673239c00b4b`) using the `git init` + `fetch --depth 1` pattern (a bare SHA cannot be passed to `git clone --branch`). The arch matrix fails loudly on unknown `TARGETARCH` values — no silent fallback.

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

- **DO NOT heavily modify `libuvc/`** — vendored upstream library. Patch minimally; prefer upgrading the whole vendor snapshot if fixes are needed.
- Do NOT create `libuvc/AGENTS.md` — vendored code, not a CeraLive module.
- Do NOT link against system libuvc if it exists; the vendored copy is intentional for version pinning.
- Do NOT hardcode `aarch64-linux-gnu` in build paths — use `$(gcc -print-multiarch)`.
- Do NOT call `force_usb_release()` before `uvc_close()` — it was a double-free/UAF vector; the fix lets `uvc_close()` own the single `libusb_close()`.
- Do NOT enable `control-socket` by default or fall back to a world-accessible path when `XDG_RUNTIME_DIR` is unset — the socket must be opt-in and per-instance.
- Do NOT set PTZ properties outside the param-spec range in tests — GObject emits a range warning that gst-check turns into a longjmp, skipping teardown and hanging the process.
- This plugin is **not** in the device image REPOS list by default — don't assume it's always present on device.
