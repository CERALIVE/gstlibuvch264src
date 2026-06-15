# SAFETY ANALYSIS: exposing USB `dwMaxPayloadTransferSize` tuning

**Task:** gstlibuvch264src-hardening Task 6 (doc-first gate)
**Question:** Should the element expose the USB bulk/iso payload size
(`uvc_stream_ctrl_t.dwMaxPayloadTransferSize`, the UVC `bMaxPayloadTransferSize`
field) as a tunable property, and if so under what contract? This document is the
gate for **Task 12** — Task 12 MUST NOT add a payload-tuning property unless this
analysis ends in `VERDICT: SAFE-TO-EXPOSE`.
**Method:** Code analysis of the element's negotiation flow
(`gstlibuvch264src.c`), the vendored CeraLive/libuvc fork's probe/commit and
transfer-allocation paths (`stream.c`), and the USB/Rockchip DMA constraints that
bound the safe range. No hardware required; no code changed in this task.
**Scope:** Investigation, design contract, and verdict only. No property added,
no libuvc edits.

---

## VERDICT: SAFE-TO-EXPOSE (with fallback)

Exposing `dwMaxPayloadTransferSize` tuning is **safe** *only* under the strict
contract in §5: opt-in property, **default preserves current device-negotiated
behavior unchanged**, the override is clamped to a sane range, **re-committed and
read back** (UVC `GET_CUR`) to confirm the device accepted it, and **falls back
to the device-negotiated value on any rejection, mismatch, or stream-start
failure**. Without that mandatory graceful fallback the feature is **NOT-SAFE** —
a naive "set the field and stream" implementation silently diverges host and
device state and can wedge constrained Rockchip targets with `LIBUSB_ERROR_NO_MEM`
or `UVC_ERROR_INVALID_MODE`. The verdict is conditional on Task 12 implementing
every clause of §5.

---

## 1. Current negotiated value source (do not break this)

`dwMaxPayloadTransferSize` is **chosen by the device**, not by the element. The
field lives in `uvc_stream_ctrl_t` (libuvc `libuvc.h:510`) inside the element
instance as `self->uvc_ctrl` (`gstlibuvch264src_internal.h:24`). It is populated
exclusively by `uvc_get_stream_ctrl_format_size()`, called at exactly two sites:

| Site | Path | Line |
|------|------|------|
| `negotiate()` | initial caps negotiation | `gstlibuvch264src.c:326` |
| `gst_libuvc_h264_src_reconnect()` | opt-in reconnect re-arm | `gstlibuvch264src.c:1015` |

Both calls run the **UVC probe/commit handshake** end to end:

```
uvc_get_stream_ctrl_format_size(devh, ctrl, cf, w, h, fps)   stream.c:472
  ├─ match frame descriptor; set bFormatIndex/bFrameIndex/dwFrameInterval
  └─ uvc_probe_stream_ctrl(devh, ctrl)                        stream.c:611
       ├─ uvc_query_stream_ctrl(ctrl, UVC_SET_CUR)   // host proposes        :616
       └─ uvc_query_stream_ctrl(ctrl, UVC_GET_CUR)   // DEVICE WRITES BACK   :617
            └─ ctrl->dwMaxPayloadTransferSize = DW_TO_INT(buf + 22)          :262
```

The decisive line is `stream.c:262`: after `GET_CUR`, libuvc overwrites the
control block with what the **device** returned. The host's proposed value in the
`SET_CUR` is whatever `uvc_get_stream_ctrl_format_size` left in the block
(libuvc does not set the payload itself in the format-size path, so the device's
preferred/maximum value flows back). `uvc_probe_stream_ctrl` then verifies the
round-trip with `_uvc_stream_params_negotiated` (`stream.c:597-603`), which
compares `dwMaxPayloadTransferSize` for equality and re-probes on mismatch.

**Net:** today the element never touches the payload field. The device is the
single source of truth. **Any tuning feature must preserve this as the default
(property unset ⇒ byte-for-byte identical negotiation).** This is non-negotiable
and is the reason the default cannot be a fixed number — it must be the sentinel
"don't touch."

### Second commit at stream start (the silent-divergence trap)

`uvc_start_streaming()` → `uvc_stream_start()` issues a **third** commit before
allocating any transfer buffers:

```
uvc_stream_start()                                            stream.c:~400
  └─ uvc_query_stream_ctrl(devh, ctrl, 0, UVC_SET_CUR)        stream.c:404
```

Critically, this `SET_CUR` is **not** followed by a `GET_CUR` re-read. So if Task
12 mutates `self->uvc_ctrl.dwMaxPayloadTransferSize` after
`uvc_get_stream_ctrl_format_size` returns but before `uvc_start_streaming`, that
mutated value *is* committed to the device — but the host then proceeds to
allocate transfers from the host-side number with **no confirmation the device
accepted it**. A device that clamps the value answers later `GET_CUR`s with a
smaller number while the host has already sized its DMA buffers larger. This is
the core hazard and the reason §5 mandates an explicit re-probe + read-back.

---

## 2. Bandwidth / throughput impact

`dwMaxPayloadTransferSize` is the per-transfer USB payload ceiling. It feeds the
host transfer setup in `uvc_stream_start` two different ways depending on
transport:

### Bulk transport (the compressed-stream path — DJI / UVC H.264/H.265)

```c
for (transfer_id = 0; transfer_id < LIBUVC_NUM_TRANSFER_BUFS; ++transfer_id) {
  transfer = libusb_alloc_transfer(0);
  strmh->transfer_bufs[transfer_id] = malloc(strmh->cur_ctrl.dwMaxPayloadTransferSize); // :1228
  libusb_fill_bulk_transfer(transfer, ..., strmh->cur_ctrl.dwMaxPayloadTransferSize, ...); // :1230-1234
}
```

`dwMaxPayloadTransferSize` **directly and linearly** sizes every one of
`LIBUVC_NUM_TRANSFER_BUFS` host buffers and their backing kernel URBs.
`LIBUVC_NUM_TRANSFER_BUFS = 100` on Linux (`20` on Android;
`libuvc_internal.h:226-230`). So:

```
total simultaneously-pinned DMA ≈ LIBUVC_NUM_TRANSFER_BUFS × dwMaxPayloadTransferSize
                                ≈ 100 × payload
```

Throughput effect: a *larger* payload means fewer, larger bulk transfers per
frame → fewer URB completions, fewer callback wakeups, lower per-packet syscall
and interrupt overhead, and less chance of an endpoint-empty stall on a bursty
encoder. A *smaller* payload means more, smaller transfers → more overhead, but a
much smaller pinned-memory footprint and finer-grained completion. For a steady
1080p/4K H.264/H.265 stream the device-negotiated value is already tuned by the
camera firmware; the realistic win from tuning is **reducing** the footprint on a
memory-starved board, or nudging it up to cut wakeup overhead on a fast link —
not a dramatic bitrate change. The codec bitrate is set elsewhere (the encoder),
not here; this knob only changes how that bitstream is chopped onto the wire.

### Isochronous transport

```c
config_bytes_per_packet = strmh->cur_ctrl.dwMaxPayloadTransferSize;   // :1147
// pick the first altsetting whose endpoint packet >= config_bytes_per_packet  :1180
packets_per_transfer = (dwMaxVideoFrameSize + ep_bytes - 1) / ep_bytes;        // :1183
if (packets_per_transfer > 32) packets_per_transfer = 32;                      // :1187
total_transfer_size = packets_per_transfer * endpoint_bytes_per_packet;        // :1190
```

On iso the payload is used to **select the USB altsetting** (the bandwidth
reservation): the loop picks the first endpoint whose `wMaxPacketSize` (× high-
speed multiplier) is ≥ the requested payload. A larger payload forces a
higher-bandwidth altsetting; if none qualifies the search falls through to
`UVC_ERROR_INVALID_MODE` (`stream.c:1196-1198`). The per-transfer buffer is then
bounded by `dwMaxVideoFrameSize` and a hard 32-packet cap, so iso DMA does not
scale linearly with payload the way bulk does — but altsetting bandwidth is a
hard, host-controller-arbitrated reservation that can fail to be granted.

---

## 3. Failure modes (why naive exposure is dangerous)

### 3.1 `LIBUSB_ERROR_NO_MEM` on constrained Rockchip DMA pools

The bulk path pins `100 × payload` bytes of DMA-capable kernel memory at
`libusb_submit_transfer` time (usbfs allocates the URB transfer buffers from the
kernel). Rockchip targets (RK3588 and friends) run with **bounded CMA / DMA
pools**; the USB host controller (DWC3/EHCI) draws coherent DMA from that pool.
Doubling the payload doubles 100 concurrent pinned buffers. usbfs has also
historically capped a single URB buffer (`MAX_USBFS_BUFFER_SIZE`, 16 MB) and the
per-process usbfs memory budget. Exceeding either yields `LIBUSB_ERROR_NO_MEM`
(or `-ENOMEM` from the submit ioctl), which surfaces from `uvc_stream_start` and
fails the whole stream start. On a device image this manifests as "camera worked
yesterday, OOMs today after someone set a big payload" — a foot-gun unless the
override is range-capped and falls back. **This is the single most important
reason the allowed range must be conservative and a NO_MEM start failure must
retry with the device-negotiated value.**

### 3.2 DJI firmware quirks rejecting larger payloads

Most DJI action cameras enumerate as USB3 UVC, but their firmware UVC
implementations are quirky: several reject or silently clamp a host-proposed
`dwMaxPayloadTransferSize` that differs from the value they themselves advertised
in `GET_CUR`/`GET_MAX`. The probe/commit equality gate
(`_uvc_stream_params_negotiated`, `stream.c:602`) is exactly the mechanism that
catches this *if* we re-probe: propose value X via `SET_CUR`, read back via
`GET_CUR`, and if the device returns Y ≠ X the negotiation is not honored. The
danger is the stream-start `SET_CUR` at `stream.c:404` has **no** such read-back,
so a Task-12 implementation that skips an explicit re-probe will commit X, size
host buffers for X, and stream against a device that quietly switched to Y →
truncated/garbled frames or a stalled endpoint. Some DJI firmware additionally
stalls the streaming endpoint outright on an out-of-policy payload, which then
trips the element's existing sustained-silence disconnect detection (~5 s of no
frames) and, with `reconnect=false`, posts `RESOURCE/READ`. The fallback must
treat a read-back mismatch as "not accepted" and revert before streaming.

### 3.3 USB2 vs USB3 differences

The viable payload range is link-speed dependent:

- **USB2 high-speed:** bulk endpoints carry 512-byte packets; iso microframe
  ceiling is 1024 × 3 = 3072 bytes (high-bandwidth). A payload tuned for USB3
  overflows what any USB2 altsetting endpoint advertises, so the iso altsetting
  search at `stream.c:1180` finds nothing usable → `UVC_ERROR_INVALID_MODE`, and
  on bulk the larger buffer simply wastes pinned memory while the device still
  packetizes at 512 B.
- **USB3 SuperSpeed:** bulk endpoints use 1024-byte packets with bursts (and
  SS endpoint companion descriptors, read at `stream.c:1160-1166`), so large
  payloads are legitimate and beneficial.

A camera that *enumerates* on a USB2 port (cable, hub, or a downgraded link) will
not honor a USB3-sized payload. Task 12 cannot assume USB3; the safe range must be
valid on USB2 (i.e. its lower bound and default must keep working at high-speed),
and the fallback must catch the `UVC_ERROR_INVALID_MODE` / mismatch that a
mis-sized value produces on a slower link.

---

## 4. What today's code does NOT protect against

- There is **no** post-`SET_CUR` read-back at stream start (`stream.c:404`), so a
  mutated host value is committed without confirmation.
- `uvc_get_stream_ctrl_format_size` does not expose a payload argument; Task 12
  must mutate `self->uvc_ctrl.dwMaxPayloadTransferSize` *between* the format-size
  call and `uvc_start_streaming`, then force a re-probe — there is no libuvc API
  that does "format-size with a payload override + verify" in one call.
- The reconnect path (`gstlibuvch264src.c:1015`) re-runs the negotiation from
  scratch; any payload override must be re-applied **and re-verified** there too,
  or a reconnect silently reverts to the device value (acceptable behavior, but it
  must be intentional and logged, not an accident).

---

## 5. SAFE design contract for Task 12 (mandatory)

Task 12 MUST implement **all** of the following. Omitting any clause downgrades
the verdict to NOT-SAFE.

1. **Opt-in property, sentinel default = current behavior.**
   Add a property (e.g. `max-payload-size`, `uint`, default **`0`**). `0` means
   "unset / use the device-negotiated value" — when unset the element performs
   **zero** extra writes and negotiation is byte-for-byte identical to today. The
   default MUST NOT be any fixed nonzero number.

2. **Clamp to a conservative, link-aware range.**
   Reject/clamp values outside a sane band. Lower bound large enough to be legal
   on USB2 high-speed (≥ 512 B; practically the device's advertised minimum);
   upper bound small enough that `LIBUVC_NUM_TRANSFER_BUFS × value` cannot exhaust
   a constrained Rockchip DMA pool (cap well under usbfs `MAX_USBFS_BUFFER_SIZE`
   and the board's CMA budget — a few MB per buffer at most, i.e. low-hundreds-of-
   KB-to-low-MB payload, not tens of MB). An out-of-range request is clamped (with
   a `GST_WARNING`) or refused via `GST_ELEMENT_ERROR(RESOURCE, SETTINGS)`, never
   silently applied.

3. **Apply only via the probe/commit handshake, then read back.**
   After `uvc_get_stream_ctrl_format_size` succeeds, write the override into
   `self->uvc_ctrl.dwMaxPayloadTransferSize` and re-run the device round-trip
   (`uvc_probe_stream_ctrl`, i.e. `SET_CUR` + `GET_CUR`). Read back the committed
   `dwMaxPayloadTransferSize` from the control block. **Do not** rely on the
   read-back-free `SET_CUR` inside `uvc_stream_start`.

4. **REQUIRED graceful fallback (the gate condition).**
   If the device returns a different value (read-back mismatch), or the re-probe
   fails, or `uvc_start_streaming` returns `LIBUSB_ERROR_NO_MEM` /
   `UVC_ERROR_INVALID_MODE` / any error attributable to the payload: **revert to
   the device-negotiated value** (re-run `uvc_get_stream_ctrl_format_size` clean,
   no override) and start the stream with that. Log the fallback at
   `GST_WARNING_OBJECT`. The stream MUST come up on the device value rather than
   fail, whenever a clean negotiation would have succeeded. A payload override is
   a *hint*, never a hard requirement.

5. **Idempotent on the reconnect path.**
   Re-apply the same override-then-verify-then-fallback logic in
   `gst_libuvc_h264_src_reconnect()` (`:1015`). A post-replug link that came back
   as USB2, or a device that now refuses the value, must fall back rather than
   loop the reconnect into exhaustion.

6. **Observability.**
   Log the requested vs. committed payload at `GST_INFO_OBJECT` on success and the
   reason on every fallback. Expose the *effective* committed value on read-back of
   the property (or a sibling read-only property), mirroring how
   `control-socket-path` is read back after `PAUSED`.

7. **No change to the disconnect/teardown invariants.**
   This feature touches only the control block before streaming. It must not alter
   teardown ordering — in particular it does **not** introduce any
   `force_usb_release()` call (the established double-`libusb_close` foot-gun; see
   `reconnect-spike.md` §3). Teardown stays native: `uvc_stop_streaming` →
   `uvc_close`.

---

## 6. Summary

| Question | Answer |
|----------|--------|
| Who picks `dwMaxPayloadTransferSize` today? | The **device**, via `uvc_probe_stream_ctrl` `SET_CUR`/`GET_CUR` (`stream.c:617`, write-back at `:262`). The element never touches it. |
| Where is it consumed? | Bulk: `malloc(payload) × LIBUVC_NUM_TRANSFER_BUFS(=100)` + `libusb_fill_bulk_transfer` (`stream.c:1228-1234`). Iso: altsetting selection (`stream.c:1147-1190`). |
| Main failure modes? | `LIBUSB_ERROR_NO_MEM` (Rockchip CMA/DMA pool), DJI firmware clamp/stall (probe mismatch / endpoint stall), USB2 ceilings → `UVC_ERROR_INVALID_MODE`. |
| Silent-divergence trap? | `uvc_stream_start`'s `SET_CUR` (`stream.c:404`) has **no** `GET_CUR` read-back. |
| Default must be… | A sentinel (`0`) that leaves negotiation **unchanged** — no fixed number. |
| Mandatory fallback? | **Yes** — revert to device-negotiated value on mismatch / `NO_MEM` / `INVALID_MODE` / start failure. |
| Safe to expose? | **Yes — with the §5 contract.** Without the graceful fallback: **NOT-SAFE.** |

**This document gates Task 12.** Task 12 may proceed only because this analysis
ends in `VERDICT: SAFE-TO-EXPOSE (with fallback)`, and only if it implements every
clause of §5.
