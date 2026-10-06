<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## STRUCTURE

```
gstlibuvcsrc/
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
│   │   ├── uvc_device.{c,h}      # USB teardown helper + V4L2 capability probe
│   │   ├── quirks.{c,h}                 # Generic table-driven pixel-rate limits and caps filtering; injectable test rows
│   │   └── usb_port_recovery.{c,h}      # deep USB recovery: device `authorized` / port `disable` rung (no GObject, no libuvc, sysfs-root parameterized)
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
│   ├── test_quirks.c        # vid:pid quirk lookup/limits, universal probe-retry policy, Osmo pixel-rate cap, synthetic-row caps filtering
│   ├── test_usb_port_recovery.c # deep-recovery helper against a synthetic sysfs tree: rung selection + leaf-target vetoes
│   ├── board/               # Manual hardware drills plus hardware-free harness self-tests
│   │   ├── negotiation-matrix.sh         # Real-camera drill; AU PTS-span fps scorer
│   │   ├── negotiation-matrix-selftest.sh# Synthetic startup-gap/fallback regression test
│   │   └── wedge-recovery.sh             # Gated-SIGKILL wedge + real-libusb_reset_device recovery timing
│   ├── fuzz_nal.c           # NAL parser fuzz harness (libFuzzer entry point)
│   ├── tsan.suppressions    # TSan suppressions for third-party + baselined GMutex blind spots
│   └── tsan_pts.suppressions# TSan suppressions for PTS/clock GMutex (permanent blind spot)
├── patches/                 # libuvc patches for the upstream fallback path (LIBUVC_USE_FORK=OFF)
│   ├── cve-2026-1991-scan-streaming-nullguard.patch  # CVE-2026-1991 null-deref fix (upstream fallback)
│   ├── uvc15-support.patch  # UVC 1.5 support
│   ├── libuvc-h265-support.patch  # H.265 stream format support
│   └── README.md
├── CMakeLists.txt           # TEST-ONLY build: compiles plugin + full ctest suite
├── Dockerfile               # Production: pinned Debian Trixie + libuvc SHA; Bookworm source-build CI retained
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

