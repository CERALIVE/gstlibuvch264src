/* PAUSED->PLAYING PTS rebaseline continuity test (harden-v2 Task 13).
 *
 * Guards the Metis-identified Option B correctness fix (Task 5): on a
 * PLAYING -> PAUSED -> PLAYING cycle the pipeline re-assigns the element's
 * base_time WITHOUT ever passing through NULL, so start() never re-runs. The
 * element's change_state vmethod therefore resets BOTH the base_time AND the
 * prev_pts latch sentinels on GST_STATE_CHANGE_PAUSED_TO_PLAYING, so the first
 * frame after the resume re-latches the NEW running-time baseline instead of:
 *   - clamping the PTS to prev_pts + 1 (the stale-prev_pts bug, had only
 *     prev_pts been reset and the monotonicity guard fired), or
 *   - adding the whole pause gap (the stale-base_time bug, had base_time not
 *     been reset and ts = now - old_base_time still included the pause).
 *
 * The contract asserted here:
 *   - the first post-resume PTS re-latches to the new running-time,
 *     |PTS - (now - new base_time)| within the drift bound;
 *   - it is NOT clamped to prev_pts + 1 (it steps forward by ~one real frame
 *     interval, not a single tick);
 *   - it does NOT jump the whole pause gap ahead (the step from the last
 *     pre-pause PTS stays a small multiple of the frame interval, never the
 *     hours-scale pause gap);
 *   - subsequent PTS stay strictly monotonic and within the drift bound
 *     relative to the new base_time.
 *
 * DETERMINISM. Like test_pts_drift.c / test_framerate_mismatch.c, the libuvc
 * mock feeder paces frames with usleep() (wall clock) and cannot be driven by a
 * controllable clock, so this test does NOT use the feeder. It constructs the
 * element directly, installs a GstTestClock, and calls frame_callback() itself,
 * stepping the test clock between calls. The PAUSED->PLAYING transition is
 * driven by calling the real change_state vmethod through the class vtable;
 * the pipeline's base_time re-assignment is modelled by
 * gst_element_set_base_time() advanced by the pause gap (so running-time stays
 * continuous across the pause, exactly as a real pipeline keeps it).
 *
 * GST_CHECKS selects this single test per ctest invocation (see CMakeLists.txt).
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

/* Nominal arrival of the first frame. Above one frame interval so the first
 * frame's baseline latch makes PTS_0 == ts_0 cleanly, and above 0 so the test
 * clock (which starts at 0) only ever moves forward. */
#define ARRIVAL_ORIGIN_NS GST_SECOND

/* base_time the element subtracts to get running-time in phase 1. Zero keeps
 * ts == clock; pinned (not G_MAXUINT64) so phase-1 frames skip the latch. */
#define BASE_TIME_NS 0

/* Frame counts for the two PLAYING runs around the pause. */
#define PHASE1_FRAMES 30
#define PHASE2_FRAMES 30

/* The pause gap: an hour of pipeline-clock time elapses while PAUSED. Big enough
 * that a stale, un-reset base_time would jump the PTS an unmistakable
 * hours-scale amount ahead. */
#define PAUSE_GAP_NS (3600 * GST_SECOND)

/* On resume the pipeline re-assigns base_time advanced by the pause gap so
 * running-time stays continuous across the pause (running-time does not advance
 * while PAUSED). The element re-latches self->base_time from this value. */
#define NEW_BASE_TIME_NS PAUSE_GAP_NS

/* The drift bound: each PTS must sit within 5 ms of the running-time the frame
 * arrived at. It also doubles as the sharp re-latch check on the first resumed
 * frame: a prev_pts + 1 clamp would leave it ~one 33 ms interval below the true
 * running-time, and a stale-base_time jump a whole pause gap above it - both far
 * outside 5 ms even though both are technically "within one frame interval" of
 * nothing useful. */
#define INSTANT_BOUND_NS (5 * GST_MSECOND)

/* The resume step (first post-resume PTS minus last pre-pause PTS) must stay a
 * small multiple of the frame interval, never the hours-scale pause gap. */
#define LARGE_JUMP_BOUND_NS GST_SECOND

/* One bare H.264 IDR access unit: 4-byte start code, IDR NAL header (0x65), then
 * a 0xAB payload that can never form a start code - exactly one NAL, one
 * PTS-bearing buffer, no SPS/PPS so frame_callback() never touches the disk. */
static size_t
craft_idr (uint8_t * b)
{
  b[0] = 0x00; b[1] = 0x00; b[2] = 0x00; b[3] = 0x01;
  b[4] = 0x65;
  memset (b + 5, 0xAB, 48);
  return 5 + 48;
}

/* Inject one IDR at the given test-clock time and return its buffer PTS, or set
 * *missing when the access unit produced no buffer (a harness wiring bug). */
static gint64
inject_frame (GstLibuvcH264Src * self, GstTestClock * tclock,
    GstClockTime arrival, gboolean * pts_invalid, gboolean * missing)
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

  gint64 pts = 0;
  GstBuffer *buf = g_async_queue_try_pop (self->frame_queue);
  if (buf == NULL) {
    *missing = TRUE;
  } else {
    GstClockTime b_pts = GST_BUFFER_PTS (buf);
    if (!GST_CLOCK_TIME_IS_VALID (b_pts))
      *pts_invalid = TRUE;
    pts = (gint64) b_pts;
    gst_buffer_unref (buf);
  }

  /* Defensive: a single IDR must yield exactly one buffer; drain any extra. */
  GstBuffer *extra;
  while ((extra = g_async_queue_try_pop (self->frame_queue)) != NULL)
    gst_buffer_unref (extra);

  return pts;
}

GST_START_TEST (test_paused_playing_rebaseline)
{
  /* --- construct the element and seed the fields frame_callback() reads ---
   * base_time is pinned to a real value (0) for phase 1 so the first frame does
   * not latch from gst_element_get_base_time(); prev_pts keeps its G_MAXUINT64
   * init sentinel so frame 0 seeds PTS_0 == ts_0. */
  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->frame_interval = NOMINAL_INTERVAL_NS;
  self->base_time = BASE_TIME_NS;

  GstTestClock *tclock =
      GST_TEST_CLOCK (gst_test_clock_new_with_start_time (0));
  self->clock = GST_CLOCK (tclock);     /* frame_callback() refs/unrefs its own */

  /* Recorded on the (single) test thread; asserted only after full teardown so a
   * fail_unless() longjmp can never leak the element, clock, or buffers. */
  static gint64 pts1_arr[PHASE1_FRAMES];
  static gint64 ts2_arr[PHASE2_FRAMES];
  static gint64 pts2_arr[PHASE2_FRAMES];
  int delivered1 = 0;
  int delivered2 = 0;
  gboolean pts_invalid = FALSE;
  gboolean missing_buffer = FALSE;

  /* --- Phase 1: PLAYING at base_time 0. PTS_i == ts_i == ARRIVAL_ORIGIN + i*interval. --- */
  for (int i = 0; i < PHASE1_FRAMES; i++) {
    GstClockTime arrival = (GstClockTime) ((gint64) ARRIVAL_ORIGIN_NS +
        (gint64) i * (gint64) NOMINAL_INTERVAL_NS);
    gint64 pts = inject_frame (self, tclock, arrival, &pts_invalid,
        &missing_buffer);
    if (delivered1 < PHASE1_FRAMES)
      pts1_arr[delivered1++] = pts;
  }

  gint64 last_pause_pts = (delivered1 > 0) ? pts1_arr[delivered1 - 1] : 0;

  /* --- Simulate PAUSED -> PLAYING ---
   * Drive the REAL change_state vmethod through the class vtable: on
   * GST_STATE_CHANGE_PAUSED_TO_PLAYING it resets self->base_time and
   * self->prev_pts to the G_MAXUINT64 latch sentinels (Task 5), then chains to
   * the parent. We then re-assign the element base_time the way the pipeline
   * does on resume - advanced by the pause gap so running-time stays continuous
   * across the pause - which the next frame re-latches into self->base_time. */
  GST_ELEMENT_GET_CLASS (self)->change_state (GST_ELEMENT (self),
      GST_STATE_CHANGE_PAUSED_TO_PLAYING);
  gst_element_set_base_time (GST_ELEMENT (self),
      (GstClockTime) NEW_BASE_TIME_NS);

  /* --- Phase 2: PLAYING again at the new base_time, after the pause gap. ---
   * ts_j = arrival - NEW_BASE_TIME = ARRIVAL_ORIGIN + (PHASE1_FRAMES + j)*interval,
   * so PTS continues the phase-1 running-time sequence seamlessly. */
  for (int j = 0; j < PHASE2_FRAMES; j++) {
    GstClockTime arrival = (GstClockTime) (
        (gint64) ARRIVAL_ORIGIN_NS +
        (gint64) (PHASE1_FRAMES + j) * (gint64) NOMINAL_INTERVAL_NS +
        (gint64) PAUSE_GAP_NS);
    gint64 ts = (gint64) arrival - (gint64) NEW_BASE_TIME_NS;
    gint64 pts = inject_frame (self, tclock, arrival, &pts_invalid,
        &missing_buffer);
    if (delivered2 < PHASE2_FRAMES) {
      ts2_arr[delivered2] = ts;
      pts2_arr[delivered2] = pts;
      delivered2++;
    }
  }

  /* --- reduce to verdicts (no asserts yet) --- */
  gint64 first_resume_pts = (delivered2 > 0) ? pts2_arr[0] : 0;
  gint64 first_resume_ts = (delivered2 > 0) ? ts2_arr[0] : 0;
  gint64 relatch_err = first_resume_pts - first_resume_ts;
  if (relatch_err < 0)
    relatch_err = -relatch_err;

  gint64 resume_step = first_resume_pts - last_pause_pts;        /* signed */

  gboolean resume_nonmonotonic = FALSE;
  gint64 max_inst_err = 0;
  for (int j = 0; j < delivered2; j++) {
    gint64 inst = pts2_arr[j] - ts2_arr[j];
    if (inst < 0)
      inst = -inst;
    if (inst > max_inst_err)
      max_inst_err = inst;
    if (j > 0 && pts2_arr[j] <= pts2_arr[j - 1])
      resume_nonmonotonic = TRUE;
  }

  /* --- full teardown BEFORE any assertion (fail_unless longjmps) --- */
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);

  /* --- sanity: the stimulus actually ran --- */
  fail_unless (!missing_buffer,
      "an IDR access unit produced no buffer (harness wiring bug)");
  fail_unless (!pts_invalid, "a buffer carried an invalid PTS");
  fail_unless (delivered1 == PHASE1_FRAMES,
      "phase 1 delivered %d frames; expected %d", delivered1, PHASE1_FRAMES);
  fail_unless (delivered2 == PHASE2_FRAMES,
      "phase 2 delivered %d frames; expected %d", delivered2, PHASE2_FRAMES);

  /* (1) The first post-resume PTS re-latches to the NEW running-time
   * (now - new base_time). The 5 ms bound is the sharp check: it rejects a
   * prev_pts + 1 clamp (~one 33 ms interval below the true running-time) and the
   * stale-base_time jump (a whole pause gap above it), both of which a looser
   * one-frame-interval tolerance would not catch on its own. */
  fail_unless (relatch_err <= INSTANT_BOUND_NS,
      "first post-resume PTS did not re-latch to the new running-time: "
      "|PTS - (now - new base_time)| = %" G_GINT64_FORMAT " us > %" G_GINT64_FORMAT
      " us. base_time/prev_pts were not both rebaselined on PAUSED->PLAYING "
      "(Task 5 Option B).", relatch_err / 1000, (gint64) INSTANT_BOUND_NS / 1000);

  /* (2) NOT clamped to prev_pts + 1: prev_pts was reset to the sentinel, so the
   * monotonicity guard must not fire on the first resumed frame - the PTS steps
   * forward by ~one real frame interval, not a single nanosecond tick. */
  fail_unless (first_resume_pts != last_pause_pts + 1,
      "first post-resume PTS was clamped to prev_pts + 1 (%" G_GINT64_FORMAT
      " ns); prev_pts was not reset on PAUSED->PLAYING", first_resume_pts);
  fail_unless (resume_step >= NOMINAL_INTERVAL_NS / 2,
      "first post-resume PTS advanced only %" G_GINT64_FORMAT " us past the last "
      "pre-pause PTS; expected ~one frame interval (a prev_pts + 1 clamp leaves "
      "it ~0)", resume_step / 1000);

  /* (3) NOT jumping the whole pause gap ahead: the step from the last pre-pause
   * PTS is bounded to a small multiple of the frame interval, never the
   * hours-scale pause gap a stale, un-reset base_time would add. */
  fail_unless (resume_step > 0 && resume_step <= LARGE_JUMP_BOUND_NS,
      "first post-resume PTS jumped %" G_GINT64_FORMAT " ms past the last "
      "pre-pause PTS (pause gap was %" G_GINT64_FORMAT " ms); base_time was not "
      "rebaselined and still includes the pause gap",
      resume_step / (gint64) GST_MSECOND,
      (gint64) PAUSE_GAP_NS / (gint64) GST_MSECOND);

  /* (4) Subsequent PTS stay strictly monotonic and within the drift bound
   * relative to the NEW base_time. */
  fail_unless (!resume_nonmonotonic,
      "post-resume PTS was not strictly monotonic");
  fail_unless (max_inst_err <= INSTANT_BOUND_NS,
      "post-resume PTS drifted from the new running-time: max |PTS_j - ts_j| = %"
      G_GINT64_FORMAT " us > %" G_GINT64_FORMAT " us", max_inst_err / 1000,
      (gint64) INSTANT_BOUND_NS / 1000);
}

GST_END_TEST;

static Suite *
pts_resume_suite (void)
{
  Suite *s = suite_create ("libuvch264src-pts-resume");

  TCase *tc = tcase_create ("paused_playing_rebaseline");
  tcase_set_timeout (tc, 60);
  tcase_add_test (tc, test_paused_playing_rebaseline);
  suite_add_tcase (s, tc);

  return s;
}

GST_CHECK_MAIN (pts_resume);
