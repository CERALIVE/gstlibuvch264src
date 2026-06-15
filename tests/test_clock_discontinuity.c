/* Clock-discontinuity PTS recovery (Task 16, harden-v2).
 *
 * Deterministic unit counterpart to the Task 13 state-transition test. It pins
 * the Option B (Task 5) clock-swap contract: when the pipeline hands the element
 * a new clock mid-stream - or the clock running-time jumps - PTS must recover
 * cleanly. "Cleanly" means three things, asserted as three independent bounds:
 *
 *   monotonic    The thin guard never emits a backward (or repeated) PTS. Across
 *                the whole stream, including the swap boundary, PTS is strictly
 *                increasing (frame_pipeline.c:329-334).
 *   rebaselined  After the swap the first frame latches the NEW clock's
 *                running-time rather than continuing the old PTS - so the first
 *                post-jump PTS sits within the drift bound of the new clock, not
 *                ~the whole jump away from it. This is the Task 5 rebaseline
 *                (gstlibuvch264src.c set_clock: base_time/prev_pts -> G_MAXUINT64).
 *   drift        Within a bounded number of frames after the jump, |PTS_i - ts_i|
 *                is back inside the 5 ms instantaneous bound (and stays there),
 *                where ts_i is the new clock's running-time. A reintroduced
 *                smoothing estimator could NOT meet this: it would advance PTS by
 *                ~one frame interval per frame and take hundreds of frames (or
 *                never) to close a multi-second jump - which is exactly the
 *                "absorb the jump" behavior Option B deleted.
 *
 * DETERMINISM. Like test_pts_drift.c, this does NOT use the libuvc mock feeder
 * (it paces with usleep(), a wall clock that a controllable test clock cannot
 * drive). It constructs the element directly, installs a GstTestClock, and calls
 * frame_callback() itself, stepping the clock with gst_test_clock_set_time()
 * before each call. The clock swap goes through the REAL gst_element_set_clock()
 * vmethod so the Task 5 rebaseline path is exercised, not re-implemented. The
 * element's base time is pinned to 0 (gst_element_set_base_time), so the
 * post-swap relatch resolves base_time back to 0 and ts_i == the clock time we
 * set: the expected running-time is known independently of what the element
 * stamps, which is what gives the drift bound teeth.
 *
 * GstTestClock forbids time moving backwards, and each clock is its own object,
 * so a forward jump is modelled by swapping to a SECOND test clock started far
 * ahead. A backward swap is intentionally NOT tested for monotonicity: the Task 5
 * rebaseline deliberately FOLLOWS the new clock, so a backward clock would
 * (correctly) produce a lower PTS - the element trusts the pipeline's clock.
 *
 * GST_CHECKS selects a single test per ctest invocation (see CMakeLists.txt).
 */

#include <gst/check/gstcheck.h>
#include <gst/check/gsttestclock.h>
#include <stdint.h>
#include <string.h>

/* gstcheck.h already installs GST_CAT_DEFAULT (check_debug); drop it so the
 * element's internal header can install its own category without a warning. */
#undef GST_CAT_DEFAULT
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"

/* The mock advertises one 30 fps interval, so negotiate() would set
 * frame_interval = 1e9 / 30. We construct the element directly (no negotiate),
 * so we seed the same value the running element uses. */
#define NOMINAL_INTERVAL_NS (GST_SECOND / 30)

/* The instantaneous drift contract under test (same as test_pts_drift.c). */
#define INSTANT_BOUND_NS (5 * GST_MSECOND)

/* "back within the drift bound within N frames" - the recovery budget after the
 * clock jump. Option B rebaselines on the first post-jump frame, so recovery is
 * immediate (frame 0); the budget gives slack and documents the contract. */
#define RECOVERY_FRAMES_N 5

/* --- clock_discontinuity scenario geometry --- */
#define PHASE1_FRAMES 30
#define PHASE2_FRAMES 30

/* Phase-1 frames arrive on clock #1 starting at 1 s (well above one frame
 * interval and above 0 so the first arrival never lands below the test clock's
 * start time of 0). Phase-2 frames arrive on clock #2 jumped far forward to
 * 100 s: a ~98 s discontinuity that a smoothing estimator could never absorb
 * within the drift bound, but a direct ts-stamp rebaselines onto instantly. */
#define PHASE1_ORIGIN_NS (1 * GST_SECOND)
#define PHASE2_ORIGIN_NS (100 * GST_SECOND)

/* base_time the element subtracts to get running-time. Pinned to 0 so ts equals
 * the clock time we set, on both clocks, before and after the swap. */
#define BASE_TIME_NS 0

/* Craft one bare H.264 IDR access unit: a 4-byte Annex-B start code, the IDR
 * NAL header (nal_ref_idc=3, type=5 -> 0x65), then a non-zero payload (0xAB can
 * never form a 00 00 01 start code, so this is exactly one NAL). No SPS/PPS NAL
 * is included, so frame_callback() never flags a parameter-set change and
 * store_spspps() is never called - the test touches no disk. Each access unit
 * yields exactly one PTS-bearing buffer. Identical to test_pts_drift.c. */
static size_t
craft_idr (uint8_t * b)
{
  b[0] = 0x00; b[1] = 0x00; b[2] = 0x00; b[3] = 0x01;
  b[4] = 0x65;
  memset (b + 5, 0xAB, 48);
  return 5 + 48;
}

/* Feed one access unit at clock running-time `arrival` and return the PTS of the
 * single buffer it produced. The element computes ts = now - base_time; with
 * base_time pinned to 0 the caller's `arrival` IS that ts. Sets *missing if the
 * IDR produced no buffer and *invalid if the buffer carried no valid PTS. */
static GstClockTime
feed_one (GstLibuvcH264Src * self, GstTestClock * tclock, GstClockTime arrival,
    gboolean * missing, gboolean * invalid)
{
  gst_test_clock_set_time (tclock, arrival);

  uint8_t au[128];
  size_t len = craft_idr (au);
  uvc_frame_t frame;
  memset (&frame, 0, sizeof (frame));
  frame.data = au;
  frame.data_bytes = len;
  frame.frame_format = UVC_FRAME_FORMAT_H264;
  frame_callback (&frame, self);

  GstClockTime pts = GST_CLOCK_TIME_NONE;
  GstBuffer *buf = g_async_queue_try_pop (self->frame_queue);
  if (buf == NULL) {
    *missing = TRUE;
  } else {
    pts = GST_BUFFER_PTS (buf);
    if (!GST_CLOCK_TIME_IS_VALID (pts))
      *invalid = TRUE;
    gst_buffer_unref (buf);
  }

  /* Defensive: drain any unexpected extra buffers so nothing leaks. */
  GstBuffer *extra;
  while ((extra = g_async_queue_try_pop (self->frame_queue)) != NULL)
    gst_buffer_unref (extra);

  return pts;
}

/* Construct the element and seed only the fields frame_callback() reads, then
 * pin base_time to 0 so the post-swap relatch resolves base_time back to 0 and
 * ts == the clock time we set. Bypasses start()/negotiate(). */
static GstLibuvcH264Src *
make_seeded_element (void)
{
  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->frame_interval = NOMINAL_INTERVAL_NS;
  /* prev_pts / base_time keep their init sentinel (G_MAXUINT64); the first
   * set_clock() below resets them anyway, and the relatch reads the pinned 0. */
  gst_element_set_base_time (GST_ELEMENT (self), BASE_TIME_NS);
  return self;
}

/* Install a fresh GstTestClock as the element's clock through the real
 * gst_element_set_clock() vmethod (exercising the Task 5 rebaseline), and return
 * it with an owned ref. The caller balances the ref in teardown. */
static GstTestClock *
swap_to_new_clock (GstLibuvcH264Src * self, GstClockTime start_time)
{
  GstTestClock *tclock =
      GST_TEST_CLOCK (gst_test_clock_new_with_start_time (start_time));
  /* Claim the floating ref so this object is fully owned here; set_clock() then
   * takes its own refs cleanly and teardown is a simple unref. */
  gst_object_ref_sink (tclock);
  fail_unless (gst_element_set_clock (GST_ELEMENT (self), GST_CLOCK (tclock)),
      "element refused the test clock");
  return tclock;
}

static gint64
abs_diff (gint64 a, gint64 b)
{
  gint64 d = a - b;
  return d < 0 ? -d : d;
}

/* ------------------------------------------------------------------------- *
 * clock_discontinuity: a far-forward clock swap mid-stream. PTS stays strictly
 * monotonic across the swap, rebaselines onto the new clock, and is back inside
 * the drift bound within N frames.
 * ------------------------------------------------------------------------- */
GST_START_TEST (test_clock_discontinuity)
{
  GstLibuvcH264Src *self = make_seeded_element ();

  /* Recorded on the (single) test thread; asserted only after full teardown so a
   * fail_unless() longjmp can never leak the element, clocks, or buffers. */
  static gint64 ts1[PHASE1_FRAMES], pts1[PHASE1_FRAMES];
  static gint64 ts2[PHASE2_FRAMES], pts2[PHASE2_FRAMES];
  int d1 = 0, d2 = 0;
  gboolean missing = FALSE, invalid = FALSE;

  /* --- phase 1: stream on clock #1 (origin 1 s) --- */
  GstTestClock *clock1 = swap_to_new_clock (self, 0);
  for (int i = 0; i < PHASE1_FRAMES; i++) {
    GstClockTime arrival =
        PHASE1_ORIGIN_NS + (GstClockTime) i * NOMINAL_INTERVAL_NS;
    GstClockTime pts = feed_one (self, clock1, arrival, &missing, &invalid);
    ts1[d1] = (gint64) arrival;
    pts1[d1] = (gint64) pts;
    d1++;
  }

  /* --- the discontinuity: swap to clock #2 jumped far forward (origin 100 s).
   * gst_element_set_clock() resets base_time/prev_pts so the next frame
   * rebaselines onto clock #2's running-time instead of clamping to the old
   * PTS + 1. --- */
  GstTestClock *clock2 = swap_to_new_clock (self, PHASE2_ORIGIN_NS);

  /* --- phase 2: stream on clock #2 --- */
  for (int i = 0; i < PHASE2_FRAMES; i++) {
    GstClockTime arrival =
        PHASE2_ORIGIN_NS + (GstClockTime) i * NOMINAL_INTERVAL_NS;
    GstClockTime pts = feed_one (self, clock2, arrival, &missing, &invalid);
    ts2[d2] = (gint64) arrival;
    pts2[d2] = (gint64) pts;
    d2++;
  }

  /* --- compute verdicts (no asserts yet) --- */
  gboolean monotonic = TRUE;
  gint64 prev = G_MININT64;
  for (int i = 0; i < d1; i++) {
    if (pts1[i] <= prev)
      monotonic = FALSE;
    prev = pts1[i];
  }
  for (int i = 0; i < d2; i++) {
    if (pts2[i] <= prev)
      monotonic = FALSE;
    prev = pts2[i];
  }

  /* First post-jump frame must track the NEW clock (rebaselined), not continue
   * the old PTS: |PTS - new running-time| within the drift bound. */
  gint64 rebaseline_drift = (d2 > 0) ? abs_diff (pts2[0], ts2[0]) : G_MAXINT64;

  /* Recovery: first post-jump frame within the drift bound, and the frame index
   * at which it first recovers (and then stays). */
  int recovery_frame = -1;
  gboolean stays_recovered = TRUE;
  for (int i = 0; i < d2; i++) {
    gint64 drift = abs_diff (pts2[i], ts2[i]);
    if (recovery_frame < 0 && drift <= (gint64) INSTANT_BOUND_NS)
      recovery_frame = i;
    if (recovery_frame >= 0 && drift > (gint64) INSTANT_BOUND_NS)
      stays_recovered = FALSE;
  }

  /* --- full teardown BEFORE any assertion (fail_unless longjmps) --- */
  gst_element_set_clock (GST_ELEMENT (self), NULL);    /* drops element refs */
  gst_object_unref (clock1);
  gst_object_unref (clock2);
  gst_object_unref (self);

  /* --- sanity: the stimulus actually ran --- */
  fail_unless (!missing, "an IDR access unit produced no buffer (wiring bug)");
  fail_unless (!invalid, "a buffer carried an invalid PTS");
  fail_unless (d1 == PHASE1_FRAMES && d2 == PHASE2_FRAMES,
      "delivered %d + %d frames; expected %d + %d", d1, d2,
      PHASE1_FRAMES, PHASE2_FRAMES);

  /* --- the contract --- */
  fail_unless (monotonic,
      "PTS went backward across the clock swap: the thin guard must never emit a "
      "backward or repeated PTS (frame_pipeline.c:329).");

  fail_unless (rebaseline_drift <= (gint64) INSTANT_BOUND_NS,
      "first post-jump PTS did not rebaseline onto the new clock: "
      "|PTS - new running-time| = %" G_GINT64_FORMAT " us > %" G_GINT64_FORMAT
      " us. Option B re-latches base_time/prev_pts on set_clock(); a smoothing "
      "estimator would still be ~the whole jump away.",
      rebaseline_drift / 1000, (gint64) INSTANT_BOUND_NS / 1000);

  fail_unless (recovery_frame >= 0 && recovery_frame < RECOVERY_FRAMES_N,
      "PTS not back within the drift bound within %d frames after the jump "
      "(recovery_frame = %d).", RECOVERY_FRAMES_N, recovery_frame);

  fail_unless (stays_recovered,
      "PTS drifted back outside the bound after recovering - the new clock is "
      "not being stamped directly.");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- *
 * clock_stall_guard: the thin guard itself. When the clock stalls (repeats a
 * running-time) so ts <= prev_pts, the guard must bump PTS to prev_pts + 1 - never
 * backward, never repeated. This exercises the ts <= prev_pts -> prev_pts + 1 arm
 * that the clean forward swap above never trips.
 * ------------------------------------------------------------------------- */

/* A schedule that advances, stalls (repeats the previous arrival), advances,
 * stalls again - mixing fresh running-times with frozen ones so the guard fires
 * on the stalls and stays quiet on the advances. The clock never moves backward
 * (GstTestClock forbids it); a stall is "same time", which is legal. */
#define STALL_FRAMES 8

GST_START_TEST (test_clock_stall_guard)
{
  GstLibuvcH264Src *self = make_seeded_element ();
  GstTestClock *clock1 = swap_to_new_clock (self, 0);

  const GstClockTime O = 1 * GST_SECOND;
  const GstClockTime I = NOMINAL_INTERVAL_NS;
  const GstClockTime sched[STALL_FRAMES] = {
    O,          /* fresh           */
    O,          /* stall -> guard  */
    O,          /* stall -> guard  */
    O + I,      /* advance         */
    O + I,      /* stall -> guard  */
    O + 2 * I,  /* advance         */
    O + 2 * I,  /* stall -> guard  */
    O + 3 * I,  /* advance         */
  };

  static gint64 ts[STALL_FRAMES], pts[STALL_FRAMES];
  int d = 0;
  gboolean missing = FALSE, invalid = FALSE;
  GstClockTime clock_now = 0;   /* GstTestClock starts at 0 */

  for (int i = 0; i < STALL_FRAMES; i++) {
    /* Only advance the clock; a stall reuses the current time (set_time would
     * assert on a non-advancing call in some builds, and "same time" needs no
     * call at all). */
    if (sched[i] > clock_now)
      clock_now = sched[i];
    GstClockTime pts_i = feed_one (self, clock1, clock_now, &missing, &invalid);
    ts[d] = (gint64) clock_now;
    pts[d] = (gint64) pts_i;
    d++;
  }

  /* --- verdicts (no asserts yet) --- */
  gboolean strictly_increasing = TRUE;
  gboolean guard_formula_ok = TRUE;
  int clamps = 0;
  for (int i = 0; i < d; i++) {
    if (i > 0 && pts[i] <= pts[i - 1])
      strictly_increasing = FALSE;

    if (i == 0) {
      /* First frame: prev_pts sentinel -> latch ts as-is. */
      if (pts[i] != ts[i])
        guard_formula_ok = FALSE;
    } else if (ts[i] <= pts[i - 1]) {
      /* Guard arm: ts at or behind the last PTS -> exactly prev_pts + 1. */
      clamps++;
      if (pts[i] != pts[i - 1] + 1)
        guard_formula_ok = FALSE;
    } else {
      /* Clear of the guard: stamp the real running-time. */
      if (pts[i] != ts[i])
        guard_formula_ok = FALSE;
    }
  }

  /* --- teardown BEFORE assertions --- */
  gst_element_set_clock (GST_ELEMENT (self), NULL);
  gst_object_unref (clock1);
  gst_object_unref (self);

  fail_unless (!missing, "an IDR access unit produced no buffer (wiring bug)");
  fail_unless (!invalid, "a buffer carried an invalid PTS");
  fail_unless (d == STALL_FRAMES, "delivered %d frames; expected %d", d,
      STALL_FRAMES);

  fail_unless (clamps > 0,
      "no stall actually tripped the guard - the test would be vacuous");
  fail_unless (strictly_increasing,
      "PTS was not strictly increasing under a stalled clock: the thin guard "
      "must bump a non-advancing ts to prev_pts + 1.");
  fail_unless (guard_formula_ok,
      "guard did not follow ts <= prev_pts -> prev_pts + 1 (else stamp ts).");
}

GST_END_TEST;

static Suite *
clock_discontinuity_suite (void)
{
  Suite *s = suite_create ("libuvch264src-clock-discontinuity");

  TCase *tc_jump = tcase_create ("clock_discontinuity");
  tcase_set_timeout (tc_jump, 60);
  tcase_add_test (tc_jump, test_clock_discontinuity);
  suite_add_tcase (s, tc_jump);

  TCase *tc_guard = tcase_create ("clock_stall_guard");
  tcase_set_timeout (tc_guard, 60);
  tcase_add_test (tc_guard, test_clock_stall_guard);
  suite_add_tcase (s, tc_guard);

  return s;
}

GST_CHECK_MAIN (clock_discontinuity);
