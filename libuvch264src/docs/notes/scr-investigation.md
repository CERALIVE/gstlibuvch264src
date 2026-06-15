# INVESTIGATION: UVC payload-header SCR/PTS → `uvc_frame_t.capture_time`

**Task:** gstlibuvch264src Task 15 — SCR/PTS investigation + conditional implementation
**Question:** Do the target DJI/UVC devices populate the UVC payload-header SCR/PTS
such that `uvc_frame_t.capture_time` is non-zero/meaningful in the element's
`frame_callback`? If yes, derive PTS from SCR (guarded). If no, document why the
current arrival-time PTS (Option B) remains correct.
**Method:** Static analysis of the vendored CeraLive libuvc fork (v0.0.7 base SHA
`68d07a00e11d1944e27b7295ee69673239c00b4b`) frame-delivery path, the element's
`frame_pipeline.c` PTS stamping, and the test mock's frame contract. No hardware.
**Scope:** Investigation + verdict + branch decision only.

---

## VERDICT: SCR-ABSENT

`uvc_frame_t.capture_time` is **structurally always `{0, 0}`** in every frame the
element's `frame_callback` receives. The vendored libuvc fork **never writes the
parsed payload-header SCR/PTS onto the delivered frame** — the parsed values
dead-end in private stream-handle fields and are never surfaced. This is
independent of whether a given DJI/UVC camera actually emits the SCR/PTS bits in
its payload header: even a device that sends a perfect SCR would still deliver
`capture_time == {0,0}` to the element, because libuvc drops it before the
callback.

**Branch taken: SCR-ABSENT → NO code change.** Arrival-time PTS (Option B) is not
merely acceptable; given this library it is the *only* correct source. Feeding
`capture_time` into PTS would feed a constant zero, collapsing all timestamps.

---

## 1. The element never reads `capture_time` (and must not, today)

`grep -rn "capture_time" libuvch264src/src/` → **no hits.** `frame_pipeline.c`
stamps PTS purely from the pipeline running-time:

```c
/* frame_pipeline.c frame_callback(), ~l.197-209 */
GstClockTime now = gst_clock_get_time(clock);   /* pipeline clock */
...
GstClockTime ts = now - base_time;               /* running-time = PTS */
```

This is "Option B": the arrival clock **is** the PTS clock, so PTS can never
drift from real time and no SCR/interval estimator is needed (see the long
rationale comment at `frame_pipeline.c:134-160`).

## 2. libuvc DOES parse the payload-header SCR/PTS — into private fields

The fork's transfer-processing path reads both the PTS and SCR source-clock
bits out of the UVC payload header (`stream.c`, `_uvc_process_payload`):

```c
/* stream.c:766-773 */
if (header_info & (1 << 2)) {                       /* PTS present bit  */
    strmh->pts = DW_TO_INT(payload + variable_offset);
}
if (header_info & (1 << 3)) {                       /* SCR present bit  */
    strmh->last_scr = DW_TO_INT(payload + variable_offset);
}
```

On frame completion, `_uvc_swap_buffers` stashes them into `hold_*` snapshots:

```c
/* stream.c:671-672 */
strmh->hold_last_scr = strmh->last_scr;
strmh->hold_pts      = strmh->pts;
```

So the SCR/PTS values *are* available inside the stream handle.

## 3. …but those values are WRITE-ONLY dead-ends — never surfaced on the frame

`grep -rn "hold_pts\|hold_last_scr" build/libuvc-src/src/stream.c` shows the
**only** occurrences are the two writes above (671-672). Nothing ever reads
`hold_pts` / `hold_last_scr`. They are dead.

The function that fills the frame handed to the user callback,
`_uvc_populate_frame` (`stream.c:1330-1393`), sets `frame_format`, `width`,
`height`, `step`, `sequence`, `data`, `metadata`, and:

```c
/* stream.c:1375 — the ONLY time-related field it sets */
frame->capture_time_finished = strmh->capture_time_finished;
```

`capture_time_finished` is **arrival time**, taken from
`clock_gettime(CLOCK_MONOTONIC, ...)` at buffer swap (`stream.c:664`) — not the
device SCR. Critically, `_uvc_populate_frame` **never assigns
`frame->capture_time`**. A full-tree grep confirms the only `capture_time =`
assignments in the whole library are in the format-conversion routines of
`frame.c` / `frame-mjpeg.c` (`out->capture_time = in->capture_time`), which the
element never invokes — it forwards the H.264/H.265 callback frame verbatim.

## 4. The frame starts zeroed and stays zeroed

`uvc_allocate_frame` zeroes the struct (`frame.c:70 memset(frame, 0, ...)`), and
the callback frame `strmh->frame` is embedded in the zero-initialized stream
handle. Since step 3 never touches `capture_time`, it is `{0, 0}` for the entire
lifetime of every frame the callback sees.

## 5. Delivery path confirmed

`_uvc_user_caller` (`stream.c:1298-1324`) is the callback thread:

```c
last_seq = strmh->hold_seq;
_uvc_populate_frame(strmh);              /* sets capture_time_finished only */
pthread_mutex_unlock(&strmh->cb_mutex);
strmh->user_cb(&strmh->frame, strmh->user_ptr);   /* -> element frame_callback */
```

There is no other path that constructs the callback frame. `frame->capture_time`
is `{0,0}` at the call site, guaranteed.

---

## Evidence chain (file:line)

| Claim | Location |
|-------|----------|
| Element never references `capture_time` | `libuvch264src/src/` — grep: 0 hits |
| Element stamps PTS = running-time (arrival) | `frame_pipeline.c:197-209`, rationale `:134-160` |
| Payload-header PTS bit parsed | `build/libuvc-src/src/stream.c:766-767` |
| Payload-header SCR bit parsed | `build/libuvc-src/src/stream.c:771-773` |
| SCR/PTS snapshotted to `hold_*` | `stream.c:671-672` |
| `hold_pts`/`hold_last_scr` never read (dead) | grep stream.c — only writes 671-672 |
| Frame populate sets ONLY `capture_time_finished` | `stream.c:1374-1375` |
| `capture_time_finished` = CLOCK_MONOTONIC arrival | `stream.c:664` |
| `frame->capture_time` never assigned in callback path | grep `capture_time =` → frame.c/frame-mjpeg.c conversion only |
| Frame zeroed at allocation | `frame.c:64-70` |
| Callback path uses `_uvc_populate_frame` only | `stream.c:1298-1324` |

---

## Why arrival-time PTS (Option B) remains correct

1. **No usable SCR exists at the element boundary.** The only device-time field
   libuvc *could* expose (`capture_time`) is never populated; the parsed SCR
   dead-ends in `hold_last_scr`. There is nothing to derive PTS from.
2. **`capture_time_finished` is not an SCR substitute.** It is a host
   `CLOCK_MONOTONIC` arrival stamp taken at USB buffer-swap — i.e. arrival time
   in a *different* clock domain from the GStreamer pipeline clock. The element
   already does the equivalent, and clock-domain-correct, thing by reading the
   pipeline clock directly (`gst_clock_get_time`), so PTS lands in the pipeline's
   running-time without a cross-clock conversion.
3. **A guarded SCR path would be dead code.** Any "if SCR non-zero use it" guard
   would, with this library, *always* take the fallback branch — it would add a
   threading/correctness surface (a new shared baseline, new TSan exposure) that
   never executes the SCR branch. That is pure risk with zero behavioral payoff.
4. **Consistency with reconnect.** The reconnect path re-arms the PTS baseline
   and IDR gate (`reconnect-spike.md` §5); arrival-time PTS rebaselines cleanly
   on resume. An SCR baseline would add another field to re-arm for no gain.

## What would have to change to revisit this verdict

Revisit ONLY if the vendored libuvc is upgraded/patched so that
`_uvc_populate_frame` actually assigns `frame->capture_time` from the parsed
`hold_last_scr` (a real SCR→`timeval` conversion), AND bench evidence on the
specific DJI/UVC hardware shows the payload-header SCR bit (`header_info & 0x08`)
is set with monotonic, non-zero values. Absent both, arrival-time PTS stays.

---

## Summary

| Question | Answer |
|----------|--------|
| Does the element read `capture_time`? | **No** (and should not — it is always 0) |
| Does libuvc parse payload-header SCR/PTS? | **Yes**, into `strmh->pts` / `strmh->last_scr` |
| Does libuvc surface SCR/PTS on the frame? | **No** — `hold_pts`/`hold_last_scr` are write-only dead-ends |
| Is `frame->capture_time` ever non-zero in the callback? | **No** — never assigned; frame is zeroed |
| Should we implement SCR-derived PTS? | **No** — there is no SCR at the element boundary |
| Is arrival-time PTS (Option B) correct? | **Yes** — it is the only correct source here |
