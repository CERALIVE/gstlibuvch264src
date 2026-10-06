<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## PROPERTIES

All properties are on `libuvcsrc` and its `libuvch264src` / `libuvch26xsrc` aliases.

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

Opt-in bounded-backoff teardown/reopen on sustained silence. Default is **off**:
the default `auto-port-reset=true` first attempts one-shot wedge recovery before
`GST_ELEMENT_ERROR(RESOURCE, READ)`. With `reconnect=true`, the reconnect ladder
runs instead. See DISCONNECT / RECONNECT BEHAVIOR.

### `max-payload` (uint, range 0..4194304, default `0`)

USB payload transfer size hint in bytes (`dwMaxPayloadTransferSize`). `0` (the default) leaves the device-negotiated value unchanged. A nonzero value is clamped to `[512, 4194304]`, applied via UVC probe/commit with read-back, and falls back to the device-negotiated value if the device refuses it. Read-back reports the effective committed value. See `libuvch264src/docs/notes/bmaxpayload-analysis.md` for tuning guidance.

### `transfer-buffers` (uint, range 0..255, default `0`)

USB transfer buffer count hint: the number of USB transfer buffers `libuvc` submits per stream. `0` (the default, and the sentinel) leaves the library's default count unchanged — no device write at all. A nonzero value is clamped to `[2, 100]` and applied via the CeraLive fork's `uvc_set_transfer_buffers()` right before streaming starts, in both the initial `start()` and on every reconnect re-arm (the fork API rejects the call mid-stream, so it must precede `uvc_start_streaming()`). Read-back reports the effective (clamped) value once applied; before that it reports the requested value. Requires the CeraLive libuvc fork (backs fork item A2, `libuvch264src/docs/notes/camera-compat.md` §3); on upstream libuvc (`LIBUVC_USE_FORK=OFF`) a nonzero request is a no-op with one warning, and the property itself is otherwise harmless to set on either build.

### `reset-settle-max-ms` (uint, range 0..120000, default `8000`)

Budget, in milliseconds, for the element's **own** readiness loop after a port reset — re-enumeration polling, the reopen retries, and the wait for the first real frame. It is a budget, not a delay: the recovery returns the instant frames are actually flowing, so a device that comes back quickly is not made to wait. When it is spent the element stops starting new attempts and falls through to the usual `RESOURCE/READ` disconnect error. Also bounds the re-enumeration poll on a `start()` that had to force-clean a previous session.

**It does not bound the total.** `uvc_stop_streaming()` and `uvc_close()` are synchronous and libuvc exposes no interruption seam, so on a device that is still re-enumerating the teardown between attempts can push the total well past this value — measured **21880 ms and 21893 ms against an 8000 ms budget** on two independent hardware runs. Size it for the fast path; the worst-case tail is teardown-bound, not policy-bound. See DISCONNECT / RECONNECT BEHAVIOR.

### `reset-rearm-frames` (uint, range 1..100000, default `30`)

Frames the device must deliver after a recovery before the one-shot port reset re-arms for a LATER wedge. The default is ~1 s at 30 fps. Re-arming on the first frame back would let a device that emits one frame and immediately re-wedges reset the port in an endless loop.

### `auto-port-reset` (boolean, default `true`)

Controls the silence-triggered wedge-recovery port reset. The default remains `true` and preserves the always-on recovery behavior. Set it to `false` for a device or scenario where issuing `USBDEVFS_RESET` risks stranding the camera; sustained silence then skips the reset and falls through to the normal `RESOURCE/READ` disconnect error path.

### `deep-port-recovery` (boolean, default `false`)

Opt-in escalation for the case the port reset cannot fix: a reset the device never comes back from. The kernel retries enumeration `PORT_INIT_TRIES` times, each failing `error -71` (`EPROTO`), then logs `unable to enumerate USB device` and stops — and at that point the device object is gone, so there is nothing left for another `libusb_reset_device()` to reset.

When `true`, the element escalates **once per silence episode**, and only after the port reset AND every reopen inside `reset-settle-max-ms` have already failed:

1. **Device-level `authorized` 0→1**, if the device object survived and still reports the vid:pid captured before the reset. A logical deauthorise + re-probe of exactly one device; the port's power state is untouched.
2. **Port-level `disable` 1→0** (1 s hold), if the device object is gone. Clears `PORT_POWER` and the latched `C_CONNECTION`/`C_ENABLE` bits so the hub sees a genuine connect-change.

Then one further settle pass — polling, reopen, and a **delivered frame** — before falling through to the usual `RESOURCE/READ` disconnect error. This means an enabled rung can roughly **double** the worst-case recovery time, which is a second reason it is opt-in.

**Why it defaults to `false`.** On board `192.168.78.131` the rung is proven to **fire** and proven **not to recover**: the port cycle drives xHCI `PORTSC` from `Powered Connected Enabled` to `Powered-off Not-connected Disabled Link:Disabled` and back with `Change: CSC`, the kernel re-runs enumeration from scratch with a fresh address — and the device still failed `error -71`, 3/3 cycles at a 10 s hold. Turning it on by default would spend a second budget and root-only sysfs writes on every wedge with no evidence behind it. Flipping the default requires a separate commit with a real ×3 recovery. See `.omo/evidence/device-platform-wave4/task-11-board-proof.md`.

**Bounds and safety.**

- **It is a port STATE cycle, not a proven VBUS removal.** `PORTSC` reports `Powered-off`, but the same root hub advertises `wHubCharacteristic 0x000a` = *"No power switching"* and the board's Type-C 5 V rail is a separate GPIO regulator. Nothing here establishes that VBUS physically dropped; the logs say "logical re-probe" for that reason.
- **Never touches a hub carrying another device.** No sysfs attribute reports a hub's power-switching mode, and ganged hubs are real on this hardware (the board's Terminus `1a40:0101` reports `Ganged power switching`), so a port whose hub has any other enumerated child is refused outright.
- **Never touches a device that is not ours.** Bus addresses are recycled; the vid:pid captured before the reset is re-checked, and a mismatch at either the device path or the port's current occupant is refused.
- **Needs privilege.** USB sysfs attributes are root-writable only. Embedded in a root service (cerastream's unit has no `User=`) the writes land; run as a normal user they return `denied` and are logged as such rather than silently doing nothing.
- The target is resolved from sysfs **while the device is still open** — after a failed re-enumeration there is no device directory left to resolve a port from.

### `deliverable-caps` (GstCaps, read-only)

The post-quirk mode ladder `negotiate()` actually selects from — what this element will **accept**, not what the device **advertises**. `NULL` until a device has been negotiated; a consumer must read `NULL` as *unknown*, never as *no modes*.

For a camera with a `QUIRK_MAX_PIXEL_RATE` row these differ: the DJI Osmo Pocket 3 advertises `3840x2160@60/50/48` and this property omits all three, because negotiation refuses them.

### Action signal: `filter-deliverable-caps(advertised, vendor-id, product-id)` → GstCaps

The same exclusion applied to a **caller-supplied** ladder, with no device involved. Pure caps arithmetic — it does not open, probe, or touch any camera.

This exists for consumers that enumerate devices. cerastream already holds the advertised ladder (from `GstDevice::caps()`, a v4l2 enumeration) and the USB ids, and must not open the camera to learn which modes are real — opening one through libuvc detaches `uvcvideo` and destroys `/dev/videoN` for seconds. It emits this on a throwaway element instance instead.

Both surfaces and `negotiate()` route through the single `uvc_quirks_filter_caps()` in `quirks.c`, so the modes an operator is **offered** are by construction the modes negotiation will **accept**.

### Action signal: `set-ptz(pan, tilt, zoom)` → boolean

Drives all three PTZ axes in one emission. Each axis is applied only when the device reports it. Returns `TRUE` if at least one supported axis was driven and every attempted set succeeded.

---

