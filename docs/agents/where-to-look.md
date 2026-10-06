<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Plugin element logic | `libuvch264src/src/gstlibuvch264src.c` |
| NAL parsing / PTS / frame callback | `libuvch264src/src/frame_pipeline.c` |
| Access-unit aggregation (`alignment=au`) | `libuvch264src/src/frame_pipeline.c` → `split_access_units()` |
| PTZ probe/set + control socket | `libuvch264src/src/ptz_control.c` |
| USB teardown + V4L2 probe | `libuvch264src/src/uvc_device.c` |
| Wedged-device USB port-reset recovery | `libuvch264src/src/gstlibuvch264src.c` → `gst_libuvc_h264_src_reset_silent_device()` |
| Deep USB recovery rung (post-reset `error -71`) | `libuvch264src/src/usb_port_recovery.c`; ladder position in `gstlibuvch264src.c` → `gst_libuvc_h264_src_deep_recovery()` |
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

