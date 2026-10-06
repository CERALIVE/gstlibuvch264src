<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## PTZ CONTROL SURFACE

Two independent surfaces, both capability-gated:

**Native GObject properties (always available, no socket needed)**
Set `pan`, `tilt`, `zoom` via `g_object_set()` or `gst-launch-1.0 ... pan=N`. The `set-ptz` action signal drives all three in one call. These are the preferred interface for programmatic control from cerastream/CeraUI.

**Opt-in Unix-domain socket (default off)**
Set `control-socket=true` to enable. The socket accepts JSON commands for `PAN_TILT`, `ZOOM`, `GET_POSITION`, and `GET_CAPABILITIES`. Routes through the same `ptz_set_pan/tilt/zoom` helpers as the native props — same clamping, same capability gate, same locking. A consumer must read the resolved `control-socket-path` property (or set an explicit path) after enabling the socket.

---

