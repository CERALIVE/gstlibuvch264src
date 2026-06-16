# gstlibuvch264src

GStreamer source element for UVC H.264 (and H.265) capture devices — DJI action cameras and compatible USB UVC hardware. Developed by UnlimitedIRL; forked and maintained under CeraLive.

Feeds raw H.264/H.265 bitstream into the cerastream pipeline. HDMI capture paths bypass this element entirely.

> **Security:** CVE-2026-1991 (null-deref in scan-streaming path) is fixed in the CeraLive fork at commit `eae7f49` (tag `ceralive-v0.0.7.2`) and also carried as `patches/cve-2026-1991-scan-streaming-nullguard.patch` for the upstream fallback path. Upstream libuvc is effectively dead (last commit 2024); the CeraLive fork at `https://github.com/CeraLive/libuvc.git` is the canonical dependency.

[![CI](https://github.com/CERALIVE/gstlibuvch264src/actions/workflows/build-check.yml/badge.svg)](https://github.com/CERALIVE/gstlibuvch264src/actions/workflows/build-check.yml)
[![Release](https://github.com/CERALIVE/gstlibuvch264src/actions/workflows/publish-release.yml/badge.svg)](https://github.com/CERALIVE/gstlibuvch264src/actions/workflows/publish-release.yml)

---

## Example Pipelines

### H.264 — Basic Capture

**Display on HDMI output (Rockchip, kernel 6.6):**
```
gst-launch-1.0 libuvch264src index=0 \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! queue ! h264parse ! queue ! v4l2slh264dec ! queue ! videoconvert ! kmssink
```

**Display on HDMI output (Rockchip, kernel 5.10):**
```
gst-launch-1.0 libuvch264src index=0 \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! queue ! h264parse ! queue ! mppvideodec ! queue ! videoconvert ! kmssink
```

**Select device by USB serial number:**
```
gst-launch-1.0 libuvch264src index="serial:CAM-001" \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! queue ! h264parse ! fakesink
```

**Select device by vendor:product ID (hex):**
```
gst-launch-1.0 libuvch264src index="1234:5678" \
  ! video/x-h264 ! fakesink
```

**Pan/tilt/zoom (capability-gated — silently ignored if device doesn't support it):**
```
gst-launch-1.0 libuvch264src index=0 pan=18000 tilt=0 zoom=100 \
  ! video/x-h264 ! fakesink
```

---

### H.265 — Capture and Decode

Use the `libuvch26xsrc` alias when working with H.265. It registers the same element under a dual-codec name that makes the codec intent explicit.

**H.265 decode to display (Rockchip, kernel 6.6):**
```
gst-launch-1.0 libuvch26xsrc index=0 \
  ! video/x-h265,width=1920,height=1080,framerate=30/1 \
  ! queue ! h265parse ! queue ! v4l2slh265dec ! queue ! videoconvert ! kmssink
```

**H.265 decode to display (Rockchip, kernel 5.10):**
```
gst-launch-1.0 libuvch26xsrc index=0 \
  ! video/x-h265,width=1920,height=1080,framerate=30/1 \
  ! queue ! h265parse ! queue ! mppvideodec ! queue ! videoconvert ! kmssink
```

**H.265 capture by serial number — pipe to fakesink for testing:**
```
gst-launch-1.0 libuvch26xsrc index="serial:CAM-002" \
  ! video/x-h265,width=3840,height=2160,framerate=30/1 \
  ! queue ! h265parse ! fakesink
```

---

### A/V Mux to MPEG-TS (AAC audio)

These examples wire a separate `alsasrc` for audio. The element itself carries video only.

ALSA device numbering varies by platform. Check `aplay -l` to confirm the correct card index.
Common values: `hw:2` on generic Linux desktops, `hw:5` on RK3588-based boards.

**H.265 video + AAC audio muxed to MPEG-TS file:**
```
gst-launch-1.0 \
  libuvch26xsrc index=0 \
    ! video/x-h265,width=1920,height=1080,framerate=30/1 \
    ! h265parse ! queue ! mux. \
  alsasrc device=hw:2 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! voaacenc bitrate=128000 ! queue ! mux. \
  mpegtsmux name=mux ! filesink location=output.ts
```

**RK3588 variant (ALSA card 5):**
```
gst-launch-1.0 \
  libuvch26xsrc index="serial:CAM-001" \
    ! video/x-h265,width=1920,height=1080,framerate=30/1 \
    ! h265parse ! queue ! mux. \
  alsasrc device=hw:5 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! voaacenc bitrate=128000 ! queue ! mux. \
  mpegtsmux name=mux ! filesink location=output.ts
```

**H.264 video + AAC audio muxed to MPEG-TS file:**
```
gst-launch-1.0 \
  libuvch264src index=0 \
    ! video/x-h264,width=1920,height=1080,framerate=30/1 \
    ! h264parse ! queue ! mux. \
  alsasrc device=hw:2 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! voaacenc bitrate=128000 ! queue ! mux. \
  mpegtsmux name=mux ! filesink location=output.ts
```

> **Note:** Opus audio is not compatible with MPEG-TS. Use `voaacenc` for any `mpegtsmux`
> pipeline. If you need Opus, mux into Matroska instead (see the MKV recording example below).

---

### SRT Streaming

**H.265 + AAC streamed over SRT (listener mode):**
```
gst-launch-1.0 \
  libuvch26xsrc index=0 \
    ! video/x-h265,width=1920,height=1080,framerate=30/1 \
    ! h265parse ! queue ! mux. \
  alsasrc device=hw:2 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! voaacenc bitrate=128000 ! queue ! mux. \
  mpegtsmux name=mux \
    ! srtserversink uri="srt://0.0.0.0:9000?mode=listener" latency=200
```

**H.264 streamed over SRT (caller mode, connecting to a remote server):**
```
gst-launch-1.0 \
  libuvch264src index=0 \
    ! video/x-h264,width=1920,height=1080,framerate=30/1 \
    ! h264parse ! queue ! mux. \
  alsasrc device=hw:2 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! voaacenc bitrate=128000 ! queue ! mux. \
  mpegtsmux name=mux \
    ! srtsink uri="srt://192.168.1.100:9000?mode=caller" latency=200
```

---

### Local Recording

**Record H.265 + AAC to MP4:**
```
gst-launch-1.0 \
  libuvch26xsrc index=0 \
    ! video/x-h265,width=1920,height=1080,framerate=30/1 \
    ! h265parse ! queue ! mux. \
  alsasrc device=hw:2 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! voaacenc bitrate=128000 ! queue ! mux. \
  mp4mux name=mux ! filesink location=recording.mp4
```

**Record H.265 + Opus to Matroska (MKV) — Opus is valid here:**
```
gst-launch-1.0 \
  libuvch26xsrc index=0 \
    ! video/x-h265,width=1920,height=1080,framerate=30/1 \
    ! h265parse ! queue ! mux. \
  alsasrc device=hw:2 \
    ! audioconvert ! audioresample \
    ! audio/x-raw,rate=48000,channels=2 \
    ! opusenc ! queue ! mux. \
  matroskamux name=mux ! filesink location=recording.mkv
```

**Record H.264 video only to MKV:**
```
gst-launch-1.0 libuvch264src index="1234:5678" \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! h264parse ! matroskamux ! filesink location=recording.mkv
```

---

### Reconnect on Disconnect

Set `reconnect=true` to enable in-element auto-reconnect when the device is unplugged
mid-stream. The element retries with exponential backoff (1, 2, 4, 8, 16 s; up to 5
attempts) before posting an error. Default is `false` — a disconnect immediately ends
the stream.

A `vid:pid` or `serial:` selector survives a replug (bus address can change). An ordinal
or `bus:` selector may resolve to a different physical device after replug.

**H.264 with reconnect enabled (serial selector survives replug):**
```
gst-launch-1.0 libuvch264src reconnect=true index="serial:CAM-001" \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! queue ! h264parse ! fakesink
```

**H.265 with reconnect enabled (vid:pid selector):**
```
gst-launch-1.0 libuvch26xsrc reconnect=true index="1234:5678" \
  ! video/x-h265,width=1920,height=1080,framerate=30/1 \
  ! queue ! h265parse ! fakesink
```

---

### PTZ Control Socket

The Unix-domain PTZ control socket is off by default. Set `control-socket=true` to
enable it. The element auto-selects a per-instance path under `$XDG_RUNTIME_DIR`:

```
$XDG_RUNTIME_DIR/libuvch264src-<pid>-<seq>.sock
```

Read the resolved path back after the element reaches PAUSED. Two instances in the same
process get distinct paths automatically.

**Enable the opt-in PTZ control socket:**
```
gst-launch-1.0 libuvch264src index=0 control-socket=true \
  ! video/x-h264 ! fakesink
# After PAUSED: g_object_get(element, "control-socket-path", &path, NULL)
```

**Explicit socket path (useful in containers where XDG_RUNTIME_DIR is unset):**
```
gst-launch-1.0 libuvch264src index=0 \
  control-socket=true \
  control-socket-path=/run/ceralive/ptz.sock \
  ! video/x-h264 ! fakesink
```

The socket accepts JSON commands for `PAN_TILT`, `ZOOM`, `GET_POSITION`, and
`GET_CAPABILITIES`. It routes through the same helpers as the native `pan`/`tilt`/`zoom`
properties — same clamping, same capability gate, same locking.

In C, read the resolved path after PAUSED:

```c
gchar *path = NULL;
g_object_get(src, "control-socket-path", &path, NULL);
/* use path, then: */
g_free(path);
```

---

### Rockchip decoder/encoder reference

| Kernel | H.264 decoder | H.265 decoder | Encoder (both codecs) |
|--------|---------------|---------------|-----------------------|
| 5.10   | `mppvideodec` | `mppvideodec` | `mpph264enc` / `mpph265enc` |
| 6.6    | `v4l2slh264dec` | `v4l2slh265dec` | `mpph264enc` / `mpph265enc` |

On kernel 5.10, `mppvideodec` handles both H.264 and H.265 via the Rockchip MPP layer. On kernel 6.6, the V4L2 stateless decoders are codec-specific.

---

### A/V-sync note

This element stamps PTS as pipeline running-time. Residual A/V drift with a Bluetooth microphone is a downstream concern — the BT clock runs independently of the pipeline clock. Add `audioresample` in the audio branch to absorb BT clock drift, or clock-slave the audio source to the pipeline master clock.

---

## Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `index` | string | `"0"` | Device selector: ordinal `"0"`, `"vid:pid"` (hex), `"serial:<sn>"`, or `"bus:<bus>:<addr>"` |
| `pan` | int | `0` | Absolute pan in UVC arcseconds (±648000); capability-gated |
| `tilt` | int | `0` | Absolute tilt in UVC arcseconds (±648000); capability-gated |
| `zoom` | int | `0` | Absolute zoom as UVC focal length (0..65535); capability-gated |
| `control-socket` | bool | `false` | Enable opt-in Unix-domain PTZ control socket (default off) |
| `control-socket-path` | string | `null` | Explicit socket path; auto-selects `$XDG_RUNTIME_DIR/libuvch264src-<pid>-<seq>.sock` when null |
| `reconnect` | bool | `false` | Auto-reconnect on mid-stream disconnect with exponential backoff (default off) |
| `max-payload` | uint | `0` | USB payload transfer size hint in bytes (`dwMaxPayloadTransferSize`); `0` = device default; nonzero clamped to `[512, 4194304]` with read-back |

Action signal: `set-ptz(pan, tilt, zoom)` — drives all three axes in one call; returns `TRUE` if at least one supported axis succeeded.

---

## Build Steps

```bash
sudo apt install build-essential cmake git meson pkg-config
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
sudo apt install libusb-1.0-0 libusb-1.0-0-dev

# 1. Build libuvc (CeraLive fork, default) — no patch step needed
scripts/build-libuvc.sh

# To use upstream v0.0.7 + patches fallback instead:
# LIBUVC_USE_FORK=OFF scripts/build-libuvc.sh

# 2. Build the plugin
meson setup build libuvch264src/
cd build && meson compile && meson install

# 3. Move .so to the system GStreamer path (multiarch-aware)
MULTIARCH=$(gcc -print-multiarch)
sudo mv /usr/local/lib/${MULTIARCH}/gstreamer-1.0/libgstlibuvch264src.so \
        /lib/${MULTIARCH}/gstreamer-1.0/
sudo cp /usr/local/lib/libuvc.* /usr/lib/${MULTIARCH}/
```

`$(gcc -print-multiarch)` resolves to `aarch64-linux-gnu` on arm64 and `x86_64-linux-gnu` on amd64. Do not hardcode the arch string.

---

## Running Tests

The test suite is hardware-independent — it uses a libuvc mock and does not require a UVC device.

```bash
# With sanitizers (recommended)
cmake -B build -DENABLE_SANITIZERS=ON && cmake --build build && ctest --test-dir build --output-on-failure

# Without sanitizers (faster)
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

