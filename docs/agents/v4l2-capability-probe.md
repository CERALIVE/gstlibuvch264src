<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## V4L2 CAPABILITY PROBE

At `start()`, after `uvc_open()` succeeds, the element issues one `VIDIOC_TRY_FMT` ioctl against `/dev/video<N>` (where N is the device ordinal). This is a cheap, non-destructive probe — it does not change any device state. The result is logged via `GST_INFO_OBJECT`:

- `"V4L2 native H.264: available"` — TRY_FMT reports H.264 with positive sizeimage
- `"V4L2 native H.264: unavailable"` — driver present but H.264 not reported
- `"V4L2 probe unavailable: cannot open /dev/videoN"` — no V4L2 node at that index

The probe is **non-fatal** in all cases. Its ordinal-derived node does not prove
USB identity correspondence or frame delivery. It never selects or rejects the
libuvc capture path and does not probe H.265.

---

