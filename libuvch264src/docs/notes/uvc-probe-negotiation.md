# UVC Probe Negotiation Policy

**Status:** [EXISTS] board-adjudicated policy rationale; production adoption follows the recorded verdict
**Date:** 2026-08-27
**Scope:** UVC stream-control probe retries and the DJI Osmo Pocket 3 rate-cap decision

This note records why stream negotiation should retry one specific failure once,
rather than probe every device twice or try to infer how the requested mode relates
to a camera's previous mode. It also preserves the hardware campaign that selected
that policy and retained the 4K@30 ceiling.

## Decision

Use a maximum of two `uvc_get_stream_ctrl_format_size()` attempts. Make the first
attempt normally. Retry once only when the first attempt returns
`UVC_ERROR_INVALID_MODE`; propagate every other error immediately.

This is the campaign's G1 outcome:

```text
RULE_G: G1 retry_runs=1 failures=0 undersized_classes=0
```

Keep the DJI Osmo Pocket 3's maximum pixel rate at `248832000` px/s, exactly
`3840x2160@30`. This is the campaign's C2 outcome:

```text
RULE_C_FINAL: C2 rate=248832000 provenance=edge-aggregate-contiguous-ceiling
```

These are evidence-bound decisions, not a claim that every UVC camera behaves like
the tested Osmo.

## Why the retry keys on the error

The defect is visible at the UVC transaction boundary. A probe writes the requested
stream control with `SET_CUR`, reads it back with `GET_CUR`, and libuvc returns
`UVC_ERROR_INVALID_MODE` when the returned control does not agree. On the tested
camera, the first readback after a mode increase described the mode committed by the
previous process. A second identical transaction let the requested control settle.

Transition state is the wrong policy input. A process can know what it requests now,
but it cannot reliably know what another process committed before it started. The
camera can outlive the process, a holder can be killed, the plugin can be reloaded,
and a service can restart without preserving the prior stream control. Recording
state locally would therefore turn an observable device error into a guess based on
incomplete history.

The error is both available and specific. It also bounds the workaround:

1. A healthy device succeeds on the first attempt and receives no extra control
   transaction.
2. `UVC_ERROR_INVALID_MODE` receives exactly one retry, which covers the measured
   stale-readback behavior.
3. A second `UVC_ERROR_INVALID_MODE`, or any different first error such as
   `UVC_ERROR_PIPE` or `UVC_ERROR_NO_DEVICE`, is returned to the caller. The policy
   cannot loop and cannot convert an unrelated transport failure into apparent
   success.

## Why unconditional double-probe was struck

G2, an unconditional two-probe policy for every device and every result, was removed
from the decision set by user decision before the campaign. The diagnostic `double`
arm remained useful as a comparison, but it was never a shipping candidate.

The distinction matters even though both `retry` and `double` passed the transition
matrix. An unconditional second call performs a redundant device transaction after a
successful first call and risks obscuring the first meaningful non-retryable error.
G1 instead leaves healthy and differently-failing devices on their normal one-call
path. The retry condition and two-attempt limit are part of the policy, not tuning
knobs.

Linux media precedent follows the same shape:

- The linux-media patch discussion
  [“media: uvcvideo: Set V4L2_CTRL_FLAG_DISABLED during queryctrl errors”](https://www.spinics.net/lists/linux-media/msg275371.html)
  changed UVC control reads to **“Only retry on -EIO”**, set
  `MAX_QUERY_RETRIES` to 2, and explains that it **“Retries for an extra attempt
  to read the control, to avoid spurious errors. More attempts do not seem to
  produce better results in the tested hardware.”** This is the closest direct
  precedent: a known transient device error gets one bounded extra attempt;
  other results do not.
- The kernel's
  [V4L2 generic error-code documentation](https://docs.kernel.org/userspace-api/media/gen-errors.html)
  says an `EBUSY` ioctl **“must not be retried without performing another action
  to fix the problem first.”** That is a complementary boundary: an error whose
  cause cannot be repaired by repeating the same operation must propagate or
  trigger a real state change, not enter a blanket retry path.

These citations support the policy shape, not this camera-specific error choice.
The campaign itself is what established `UVC_ERROR_INVALID_MODE` as the retryable
signal here.

## Campaign method

### Verdict scope and binary provenance

The campaign ran on a Radxa ROCK 5B+ (`ceralive2`) with Debian 12 and kernel
`7.2.0-ceralive-rk3588`. The camera was a DJI Osmo Pocket 3 (`2ca3:0023`) with
`bcdDevice 5.04`, product/firmware string `DJIPocket3`, and serial
`123456789ABCDEF`, attached at USB high speed (480 Mbit/s).

All verdict-bearing runs used deployment session
`fe94c567-7be1-459f-a2c4-8ebf2e4a7275`, drill plugin SHA-256
`28ca41aaea1e424a1c3bec28c9a9c5d6c0a1295a916a8705302375fd04e2aa9b`, and
matching libuvc SHA-256
`5cce5600d7ab9c12408f40b609782a1615f1e1fd1915b77d38d5603796c106a5` built
from the CeraLive fork at
`f3eda761b69acdfa6c0ffc02119b2bda172b9d46`.

`tests/board/negotiation-matrix.sh` computed every count and verdict. A run counted
only when it carried complete host, OS, binary, library, camera, USB-topology, and
thermal provenance and ended with `RUN_COMPLETE`. The bounded mode rule required
every requested access unit, at least 90% of nominal fps, SPS-derived requested
geometry, and no element error or warning. The sustained rule replaced the bounded
count with survival for the full time window and retained the other checks.

### Drill A: reproduce the stale-readback branch

Run UUID `78fd9789-c9af-4b0b-8840-0aca122bf747` used `policy=single`, N=10 per
transition class, alternating both increase and decrease pairs. The script emitted:

```text
CLASS_COUNTS: class=increase n=10 failures=10 inconclusive=0
VERDICT_CLASS: FAIL class=increase failures=10/10
CLASS_COUNTS: class=same n=10 failures=0 inconclusive=0
VERDICT_CLASS: PASS class=same failures=0/10
CLASS_COUNTS: class=decrease n=10 failures=0 inconclusive=0
VERDICT_CLASS: PASS class=decrease failures=0/10
RULE_A_BRANCH: REPRODUCED policy=single n=10 failures=10 rate=100.0%
```

The increase class split evenly: `1280x720@30 → 1920x1080@30` failed 5/5 and
`1920x1080@30 → 3840x2160@30` failed 5/5. Same-mode passed 10/10; both decrease
pairs passed 5/5. This exactly reproduced the direction-specific 2026-07-30
baseline rather than a generic negotiation failure.

### Drill B: compare the shipping candidate with the diagnostic control

The `retry` run UUID was `3bd49a3a-1ba7-442e-8d60-5aecdc781981`; the
unconditional-double diagnostic UUID was `95195d9b-e6c4-45a5-80b4-e5a3b936705f`.
Each policy ran N=10 for increase, same, and decrease. The retry arm emitted:

```text
CLASS_COUNTS: class=increase n=10 failures=0 inconclusive=0
VERDICT_CLASS: PASS class=increase failures=0/10
CLASS_COUNTS: class=same n=10 failures=0 inconclusive=0
VERDICT_CLASS: PASS class=same failures=0/10
CLASS_COUNTS: class=decrease n=10 failures=0 inconclusive=0
VERDICT_CLASS: PASS class=decrease failures=0/10
```

The double arm emitted the same zero-failure counts. Rule G scored only the retry
arm and emitted:

```text
RULE_G_INPUT: run=/data/uvc-quirk-generalization/task-6/retry/transition-20260827T182332Z-3bd49a3a class=increase n=10 failures=0
RULE_G_INPUT: run=/data/uvc-quirk-generalization/task-6/retry/transition-20260827T182332Z-3bd49a3a class=same n=10 failures=0
RULE_G_INPUT: run=/data/uvc-quirk-generalization/task-6/retry/transition-20260827T182332Z-3bd49a3a class=decrease n=10 failures=0
RULE_G: G1 retry_runs=1 failures=0 undersized_classes=0
VERDICT: PASS rule=g result=G1
```

There were no inconclusive attempts, SSH drops, undersized classes, or reruns.

### Drill C: separate probe timing from undeliverable caps

The shipping-candidate `retry` after-smaller run UUID
`ac025071-0961-48be-9905-c22e76e3f445` ran N=45 at each high rate. The script
emitted:

```text
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@60 failures=45/45
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@50 failures=45/45
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@48 failures=45/45
```

The 4K@30 control, UUID `d3bb3e14-a94c-4f89-a1d5-fa83f80c01a3`, emitted:

```text
CELL_COUNTS: arm=after-smaller mode=3840x2160@30 n=5 passes=5 failures=0 inconclusive=0 skipped=0
VERDICT_CELL: PASS arm=after-smaller mode=3840x2160@30 passes=5/5
```

The unconditional-double diagnostic, UUID
`8c2e8c58-1ca3-4499-9cc1-cfc5922269a9`, failed 45/45 at each of
4K@60, 4K@50, and 4K@48 with the same three `VERDICT_CELL` lines. The reduced
single-probe control, UUID `81ca5bd0-9c4f-4d00-86a8-1b47592000e7`, emitted:

```text
CELL_COUNTS: arm=after-smaller mode=3840x2160@60 n=5 passes=0 failures=5 inconclusive=0 skipped=0
VERDICT_CELL: FAIL arm=after-smaller mode=3840x2160@60 failures=5/5
```

The high-rate logs consistently reported `Unable to negotiate common caps` and
`not-negotiated (-4)`, with `invalid_mode=0`, zero access units, unreadable
geometry, and one element error. Identical failures across retry, unconditional
double, and single policies show that another probe does not address this class.

The retry sustained UUID `956a010b-3ba7-4e4a-92b9-dcb4b2c4b08a` ran three
3-minute 4K@60 windows and emitted:

```text
SUSTAINED_RUN: FAIL replicate=1 aus=0 wall=180.005s fps=0.000
SUSTAINED_RUN: FAIL replicate=2 aus=0 wall=180.005s fps=0.000
SUSTAINED_RUN: FAIL replicate=3 aus=0 wall=180.005s fps=0.000
```

The operator selected the unattended path. Cold-start UUID
`f0540002-2e93-4448-9176-b6941af51533` recorded each high-rate cell as
`SKIPPED ... skipped=10/10 reason=unattended`; after-replug UUID
`675e27ce-0a7b-4dcb-bd52-7169d8cf9412` recorded each as
`SKIPPED ... skipped=5/5 reason=unattended`. Skips were distinct from passes and
made C1 unreachable. The aggregate emitted:

```text
RULE_C_PASSED_MODES: 3840x2160@30
RULE_C_SUSTAINED: passing=0 required=3
RULE_C_VENDOR_GATE: runs=0 passing=0
RULE_C_AGGREGATE: C2 rate=248832000
RULE_C_FINAL: C2 rate=248832000 provenance=edge-aggregate-contiguous-ceiling
VERDICT: PASS rule=c result=C2
```

The vendor gate did not apply because the edge result was C2, not C1-provisional.

### Drill D: regression sanity

The winning `retry` build completed one 5-minute 1080p30 soak, UUID
`44d42e2f-eb50-46ff-8481-0392f8bf7b9e`. The script emitted:

```text
RUN_SCORE: PASS mode=1920x1080@30 aus=8975 wall=300.064s fps=29.910 floor=27.000 geometry=1920x1080 errors=0
SUSTAINED_RUN: PASS replicate=1 aus=8975 wall=300.064s fps=29.910
VERDICT: PASS
```

The independent real-hardware wedge-recovery drill then exercised three
deterministic external port-reset cycles on the camera's current `9-1` bus. It
reported 112 ms, 121 ms, and 109 ms from reset to advancing frames against the
8,000 ms bound, left the `10-1` negative control unchanged, and emitted:

```text
RESULT: PASS (3 cycle(s) recovered inside 8000ms)
```

This regression check does not affect G1 or C2; it shows the existing recovery
ladder still operates with the drill build.

## Interpretation boundary

G1 addresses one observable stream-control failure: first-probe
`UVC_ERROR_INVALID_MODE`. C2 addresses a different layer: high rates rejected at
caps negotiation and never reaching frame delivery. Keeping those decisions
separate prevents a successful retry policy from being used as evidence that a
descriptor-advertised mode is deliverable.

Future cap changes still require advancing frames, script-scored fps,
SPS-confirmed geometry, and zero element errors on real hardware. Future retry
changes still require a specific retryable signal and a fixed attempt bound.
