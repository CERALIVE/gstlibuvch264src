/* PTS drift-bound regression guard (Task 1, harden-v2). Committed RED.
 *
 * This is the regression guard for the Option B PTS rearchitecture (Task 5):
 * delete the smoothing estimator and stamp each buffer with the running-time the
 * frame actually arrived at. The test encodes that contract as two bounds and
 * asserts them against a long, realistic synthetic frame stream:
 *
 *   rate fidelity   |(PTS_N - PTS_0) - (ts_N - ts_0)| <= 2 ms, for every N.
 *                   The PTS clock must advance at the SAME rate as the real
 *                   arrival clock - no slow accumulating drift.
 *   instantaneous   max_i |PTS_i - ts_i| <= 5 ms.
 *                   Each PTS must sit within 5 ms of the running-time the frame
 *                   arrived at - no large transient offset (e.g. after a drop).
 *
 * ts_i is the controllable test-clock running-time at frame arrival, computed
 * exactly as frame_callback() does it: ts = gst_clock_get_time(clock) - base_time
 * (frame_pipeline.c:164-176). The estimator DISCARDS that ts and instead builds
 * PTS from prev_pts + frame_interval + pts_stretch + pts_offset_sum
 * (frame_pipeline.c:375), so it cannot meet either bound:
 *   - a dropped frame makes ts jump ~2 intervals while PTS advances only ~1, so
 *     |PTS_i - ts_i| immediately diverges by ~one frame interval (~33 ms) and the
 *     gradual resync (50 us/frame stretch) cannot close it for hundreds of frames;
 *   - per-frame arrival jitter never reaches PTS at all (the estimator smooths it
 *     out), so PTS and ts disagree by the jitter amplitude every frame.
 * Both blow past 2 ms / 5 ms, so this test FAILS today (RED) and goes green once
 * Option B stamps PTS = ts directly.
 *
 * DETERMINISM. The libuvc mock feeder paces frames with usleep() (wall clock),
 * which cannot be driven by a controllable clock, so this test does NOT use the
 * feeder. Like test_live_source.c's spspps case it constructs the element
 * directly, installs a GstTestClock, and calls frame_callback() itself - setting
 * the test clock to a jittered, occasionally-skipped arrival schedule before each
 * call. No wall-clock sleep is involved; every ts_i is exact and reproducible.
 *
 * test_pts_monotonic INTERACTION (recorded per Task 1). test_pts_monotonic.c
 * asserts DURATION only as an UPPER bound:
 *     if (GST_CLOCK_TIME_IS_VALID(dur) && (dur == 0 || dur > DURATION_SANE_MAX))
 * with DURATION_SANE_MAX == GST_SECOND - i.e. DURATION must be non-zero and
 * <= 1 s. There is NO lower bound pinning DURATION to ~frame_interval. So Option
 * B (DURATION = actual inter-arrival delta, ~33 ms +/- jitter, ~66 ms across a
 * drop - always in (0, 1 s]) will NOT regress test_pts_monotonic. Confirmed
 * against tests/test_pts_monotonic.c lines 96 and 126.
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

/* Per-frame arrival jitter: +/- 3 ms around the nominal interval (a realistic
 * libuvc capture wobble). Bounded noise, NOT drift: the long-run mean spacing
 * stays exactly NOMINAL_INTERVAL_NS. */
#define JITTER_NS (3 * GST_MSECOND)

/* Total nominal frame slots fed. >100 delivered frames (Task 1) and crosses the
 * estimator's MIN_FRAMES_CALC_INTERVAL (60) resync window ~3 times. */
#define NUM_SLOTS 210

/* Nominal (pre-jitter) arrival of slot 0. Chosen well above one frame interval
 * so the first-frame baseline latch (prev_pts = ts - frame_interval, valid only
 * when ts > frame_interval) makes PTS_0 == ts_0 cleanly, and far enough above 0
 * that slot 0's negative jitter never produces an arrival below the test clock's
 * start time of 0 (GstTestClock forbids time moving backwards). */
#define ARRIVAL_ORIGIN_NS GST_SECOND

/* base_time the element subtracts to get running-time. Zero keeps ts == clock. */
#define BASE_TIME_NS 0

/* The contract bounds under test. */
#define RATE_FIDELITY_BOUND_NS (2 * GST_MSECOND)
#define INSTANT_BOUND_NS       (5 * GST_MSECOND)

/* Deterministic so the RED/GREEN verdict never depends on RNG luck. */
#define JITTER_SEED 0x5EEDU

/* Slots whose frame is "dropped" (never delivered): the clock still advances
 * through them, so the next delivered frame arrives ~2 (or ~3 for the burst)
 * intervals after its predecessor. None land on a 60-frame resync boundary, so
 * the resulting offset persists across the measurement window. */
static const int dropped_slots[] = { 20, 21, 95, 160 };

static gboolean
slot_is_dropped (int slot)
{
  for (gsize i = 0; i < G_N_ELEMENTS (dropped_slots); i++) {
    if (dropped_slots[i] == slot)
      return TRUE;
  }
  return FALSE;
}

/* Craft one bare H.264 IDR access unit: a 4-byte Annex-B start code, the IDR
 * NAL header (nal_ref_idc=3, type=5 -> 0x65), then a non-zero payload (0xAB can
 * never form a 00 00 01 start code, so this is exactly one NAL). No SPS/PPS NAL
 * is included, so frame_callback() never flags a parameter-set change and
 * store_spspps() is never called - the test touches no disk. Each access unit
 * yields exactly one PTS-bearing buffer. */
static size_t
craft_idr (uint8_t * b)
{
  b[0] = 0x00; b[1] = 0x00; b[2] = 0x00; b[3] = 0x01;
  b[4] = 0x65;
  memset (b + 5, 0xAB, 48);
  return 5 + 48;
}

GST_START_TEST (test_pts_drift)
{
  /* --- construct the element and seed the fields frame_callback() reads ---
   * Bypass start()/negotiate(): set only what the PTS path needs. prev_pts and
   * base_time keep their init sentinels except base_time, which we pin to a real
   * value so the first frame does not latch from gst_element_get_base_time(). */
  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->frame_interval = NOMINAL_INTERVAL_NS;
  self->base_time = BASE_TIME_NS;       /* not G_MAXUINT64: skip the latch */
  /* prev_pts stays G_MAXUINT64 from init() so the first frame seeds the
   * baseline as PTS_0 == ts_0. */

  GstTestClock *tclock =
      GST_TEST_CLOCK (gst_test_clock_new_with_start_time (0));
  self->clock = GST_CLOCK (tclock);     /* frame_callback() refs/unrefs its own */

  GRand *rng = g_rand_new_with_seed (JITTER_SEED);

  /* Recorded on the (single) test thread; asserted only after full teardown so a
   * fail_unless() longjmp can never leak the element, clock, or buffers. */
  static gint64 ts_arr[NUM_SLOTS];
  static gint64 pts_arr[NUM_SLOTS];
  int delivered = 0;
  gboolean pts_invalid = FALSE;
  gboolean missing_buffer = FALSE;

  uint8_t au[128];

  for (int slot = 0; slot < NUM_SLOTS; slot++) {
    /* Jittered absolute arrival time for this slot. The step between slots is
     * NOMINAL +/- up to 6 ms, always strictly positive, so the test clock only
     * ever moves forward (GstTestClock forbids going backwards). */
    gint64 jitter =
        g_rand_int_range (rng, -(gint) JITTER_NS, (gint) JITTER_NS + 1);
    GstClockTime arrival = (GstClockTime) ((gint64) ARRIVAL_ORIGIN_NS +
        (gint64) slot * (gint64) NOMINAL_INTERVAL_NS + jitter);
    gst_test_clock_set_time (tclock, arrival);

    if (slot_is_dropped (slot))
      continue;                 /* frame never reaches frame_callback() */

    /* ts_i exactly as the element computes it (frame_pipeline.c:164-176). */
    GstClockTime now = gst_clock_get_time (self->clock);
    gint64 ts = (gint64) now - (gint64) self->base_time;

    size_t len = craft_idr (au);
    uvc_frame_t frame;
    memset (&frame, 0, sizeof (frame));
    frame.data = au;
    frame.data_bytes = len;
    frame.frame_format = UVC_FRAME_FORMAT_H264;
    frame_callback (&frame, self);

    /* One IDR access unit -> exactly one PTS-bearing buffer. */
    GstBuffer *buf = g_async_queue_try_pop (self->frame_queue);
    if (buf == NULL) {
      missing_buffer = TRUE;
    } else {
      GstClockTime pts = GST_BUFFER_PTS (buf);
      if (!GST_CLOCK_TIME_IS_VALID (pts))
        pts_invalid = TRUE;
      if (delivered < NUM_SLOTS) {
        ts_arr[delivered] = ts;
        pts_arr[delivered] = (gint64) pts;
        delivered++;
      }
      gst_buffer_unref (buf);
    }

    /* Defensive: drain any unexpected extra buffers so nothing leaks. */
    GstBuffer *extra;
    while ((extra = g_async_queue_try_pop (self->frame_queue)) != NULL)
      gst_buffer_unref (extra);
  }

  /* --- compute the two bounds (no asserts yet) --- */
  gint64 max_rate_err = 0;      /* max |(PTS_i - PTS_0) - (ts_i - ts_0)| */
  gint64 max_inst_err = 0;      /* max |PTS_i - ts_i| */
  if (delivered > 0) {
    gint64 pts0 = pts_arr[0];
    gint64 ts0 = ts_arr[0];
    for (int i = 0; i < delivered; i++) {
      gint64 inst = pts_arr[i] - ts_arr[i];
      if (inst < 0)
        inst = -inst;
      if (inst > max_inst_err)
        max_inst_err = inst;

      gint64 rate = (pts_arr[i] - pts0) - (ts_arr[i] - ts0);
      if (rate < 0)
        rate = -rate;
      if (rate > max_rate_err)
        max_rate_err = rate;
    }
  }

  /* --- full teardown BEFORE any assertion (fail_unless longjmps) --- */
  g_rand_free (rng);
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);

  /* --- sanity: the stimulus actually ran --- */
  fail_unless (!missing_buffer,
      "an IDR access unit produced no buffer (harness wiring bug)");
  fail_unless (!pts_invalid, "a buffer carried an invalid PTS");
  fail_unless (delivered == NUM_SLOTS - (int) G_N_ELEMENTS (dropped_slots),
      "delivered %d frames; expected %d (slots %d minus %d drops)",
      delivered, NUM_SLOTS - (int) G_N_ELEMENTS (dropped_slots),
      NUM_SLOTS, (int) G_N_ELEMENTS (dropped_slots));
  fail_unless (delivered >= 100,
      "only %d frames delivered; need >=100 to exercise drift", delivered);

  /* --- the contract (RED today; green after Option B, Task 5) --- */
  fail_unless (max_inst_err <= INSTANT_BOUND_NS,
      "instantaneous bound violated: max |PTS_i - ts_i| = %" G_GINT64_FORMAT
      " us > %" G_GINT64_FORMAT " us (5 ms). The estimator builds PTS from "
      "prev_pts + frame_interval and discards the arrival ts, so a dropped "
      "frame leaves PTS ~one interval off the running clock. Fixed by Option B "
      "(Task 5).", max_inst_err / 1000, (gint64) INSTANT_BOUND_NS / 1000);

  fail_unless (max_rate_err <= RATE_FIDELITY_BOUND_NS,
      "rate-fidelity bound violated: max |(PTS_N - PTS_0) - (ts_N - ts_0)| = %"
      G_GINT64_FORMAT " us > %" G_GINT64_FORMAT " us (2 ms). The PTS clock drifts "
      "from the real arrival clock because dropped frames and jitter never reach "
      "the estimator. Fixed by Option B (Task 5).",
      max_rate_err / 1000, (gint64) RATE_FIDELITY_BOUND_NS / 1000);
}

GST_END_TEST;

static Suite *
pts_drift_suite (void)
{
  Suite *s = suite_create ("libuvch264src-pts-drift");

  TCase *tc = tcase_create ("pts_drift");
  tcase_set_timeout (tc, 60);
  tcase_add_test (tc, test_pts_drift);
  suite_add_tcase (s, tc);

  return s;
}

GST_CHECK_MAIN (pts_drift);
