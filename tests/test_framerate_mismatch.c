/* Framerate-mismatch tolerance tests (harden-v2 Task 9; Oracle Option B).
 *
 * Specifies and guards the element's behavior when the device's real delivery
 * cadence differs from the negotiated framerate (e.g. negotiated 30 fps, device
 * delivering ~24 fps). The contract documented in frame_pipeline.c above
 * frame_callback() is:
 *   - PTS is stamped from real running-time, so it tracks the actual arrival
 *     clock regardless of the negotiated fps (within a small drift bound);
 *   - DURATION stays the constant caps-derived 1/fps (or NONE) and does NOT
 *     track the mismatched real rate;
 *   - no renegotiation and no frame dropping/duplication to force the cadence;
 *   - GST_BUFFER_OFFSET stays a strict +1 sequence so real drops are visible.
 *
 * Two cases, one statically-registered executable (like test_pts_monotonic /
 * test_live_source); GST_CHECKS selects one per ctest invocation:
 *
 *   framerate_mismatch_pts          Deterministic. Negotiated 30 fps, frames
 *                                   injected at a constant ~24 fps cadence via a
 *                                   GstTestClock + direct frame_callback() (the
 *                                   mock feeder paces with usleep() and cannot be
 *                                   driven by a controllable clock - same reason
 *                                   test_pts_drift.c bypasses it). Asserts each
 *                                   PTS sits within 5 ms of the running-time the
 *                                   frame arrived at, every DURATION is the
 *                                   constant caps-derived interval (or NONE) and
 *                                   never the ~41.67 ms real cadence, OFFSET is a
 *                                   strict +1 sequence, and the delivered buffer
 *                                   count equals the injected frame count (no
 *                                   silent drops). RED under the smoothing
 *                                   estimator (PTS built from prev_pts +
 *                                   frame_interval diverges from the real arrival
 *                                   clock by ~8.33 ms/frame at a 30-vs-24 fps
 *                                   mismatch); GREEN once Option B (Task 5) stamps
 *                                   PTS = ts directly.
 *
 *   framerate_mismatch_no_renegotiate
 *                                   A real PLAYING pipeline drives the mock feeder
 *                                   at MOCK_UVC_FRAME_INTERVAL_US (~24 fps) while
 *                                   the descriptor advertises 30 fps. Asserts the
 *                                   streaming path forwards exactly one CAPS event
 *                                   (no mid-stream renegotiation), every delivered
 *                                   buffer keeps the strict +1 OFFSET sequence (no
 *                                   silent drops), and exactly the requested
 *                                   number of buffers is delivered. GREEN today
 *                                   and after Option B - the element already
 *                                   never renegotiates or drops to coerce cadence.
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
#include "mock_libuvc.h"

/* Negotiated (nominal) framerate: the mock advertises one 30 fps interval, so
 * negotiate() sets frame_interval = 1e9 / 30. The direct-callback case seeds the
 * same value the running element would. This is the caps-derived DURATION too. */
#define NOMINAL_INTERVAL_NS (GST_SECOND / 30)

/* The device's real, slower delivery cadence: ~24 fps. The mismatch under test
 * is the 30-vs-24 rate gap, NOT jitter, so the schedule is a clean constant. */
#define DEVICE_INTERVAL_NS (GST_SECOND / 24)

/* Enough frames to cross the estimator's resync window a few times so the RED
 * verdict is unambiguous; >100 delivered as the drift guard requires. */
#define NUM_FRAMES 120

/* Nominal arrival of frame 0. Above one frame interval so the first-frame
 * baseline latch (prev_pts = ts - frame_interval) makes PTS_0 == ts_0 cleanly,
 * and above 0 so the test clock (which starts at 0) only moves forward. */
#define ARRIVAL_ORIGIN_NS GST_SECOND

/* base_time the element subtracts to get running-time. Zero keeps ts == clock. */
#define BASE_TIME_NS 0

/* The drift bound under test: each PTS must sit within 5 ms of the running-time
 * the frame actually arrived at. */
#define INSTANT_BOUND_NS (5 * GST_MSECOND)

/* ------------------------------------------------------------------------- */
/* Shared fixture                                                            */
/* ------------------------------------------------------------------------- */

static void
load_core_elements (void)
{
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }
}

static void
register_element (void)
{
  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }
}

/* ------------------------------------------------------------------------- */
/* framerate_mismatch_pts: deterministic PTS / DURATION / OFFSET assertions  */
/* ------------------------------------------------------------------------- */

/* One bare H.264 IDR access unit: 4-byte start code, IDR NAL header (0x65),
 * then a 0xAB payload that can never form a start code - exactly one NAL, one
 * PTS-bearing buffer, no SPS/PPS so frame_callback() never touches the disk. */
static size_t
craft_idr (uint8_t * b)
{
  b[0] = 0x00; b[1] = 0x00; b[2] = 0x00; b[3] = 0x01;
  b[4] = 0x65;
  memset (b + 5, 0xAB, 48);
  return 5 + 48;
}

GST_START_TEST (test_framerate_mismatch_pts)
{
  register_element ();

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->frame_interval = NOMINAL_INTERVAL_NS;   /* negotiated 30 fps */
  self->base_time = BASE_TIME_NS;               /* not G_MAXUINT64: skip the latch */
  /* prev_pts stays G_MAXUINT64 from init() so frame 0 seeds PTS_0 == ts_0. */

  GstTestClock *tclock =
      GST_TEST_CLOCK (gst_test_clock_new_with_start_time (0));
  self->clock = GST_CLOCK (tclock);             /* frame_callback() refs its own */

  /* Recorded on the single test thread; asserted only after full teardown so a
   * fail_unless() longjmp can never leak the element, clock, or buffers. */
  static gint64 ts_arr[NUM_FRAMES];
  static gint64 pts_arr[NUM_FRAMES];
  static gint64 dur_arr[NUM_FRAMES];
  static guint64 off_arr[NUM_FRAMES];
  static guint64 off_end_arr[NUM_FRAMES];
  int delivered = 0;
  gboolean pts_invalid = FALSE;
  gboolean missing_buffer = FALSE;

  uint8_t au[128];

  for (int i = 0; i < NUM_FRAMES; i++) {
    /* Inject at a constant ~24 fps while the element believes it is 30 fps. */
    GstClockTime arrival = (GstClockTime) ((gint64) ARRIVAL_ORIGIN_NS +
        (gint64) i * (gint64) DEVICE_INTERVAL_NS);
    gst_test_clock_set_time (tclock, arrival);

    /* ts_i exactly as the element computes it (frame_pipeline.c). */
    GstClockTime now = gst_clock_get_time (self->clock);
    gint64 ts = (gint64) now - (gint64) self->base_time;

    size_t len = craft_idr (au);
    uvc_frame_t frame;
    memset (&frame, 0, sizeof (frame));
    frame.data = au;
    frame.data_bytes = len;
    frame.frame_format = UVC_FRAME_FORMAT_H264;
    frame.sequence = (uint32_t) i;
    frame_callback (&frame, self);

    GstBuffer *buf = g_async_queue_try_pop (self->frame_queue);
    if (buf == NULL) {
      missing_buffer = TRUE;
    } else {
      GstClockTime pts = GST_BUFFER_PTS (buf);
      if (!GST_CLOCK_TIME_IS_VALID (pts))
        pts_invalid = TRUE;
      if (delivered < NUM_FRAMES) {
        ts_arr[delivered] = ts;
        pts_arr[delivered] = (gint64) pts;
        dur_arr[delivered] = (gint64) GST_BUFFER_DURATION (buf);
        off_arr[delivered] = GST_BUFFER_OFFSET (buf);
        off_end_arr[delivered] = GST_BUFFER_OFFSET_END (buf);
        delivered++;
      }
      gst_buffer_unref (buf);
    }

    /* Defensive: a single IDR must yield exactly one buffer; drain any extra. */
    GstBuffer *extra;
    while ((extra = g_async_queue_try_pop (self->frame_queue)) != NULL)
      gst_buffer_unref (extra);
  }

  /* --- reduce to verdicts (no asserts until after teardown) --- */
  gint64 max_inst_err = 0;      /* max |PTS_i - ts_i| */
  gboolean duration_tracks_rate = FALSE;  /* a DURATION != caps interval and != NONE */
  gboolean offset_broken = FALSE;         /* OFFSET not a strict +1 / END != OFF+1 */
  for (int i = 0; i < delivered; i++) {
    gint64 inst = pts_arr[i] - ts_arr[i];
    if (inst < 0)
      inst = -inst;
    if (inst > max_inst_err)
      max_inst_err = inst;

    /* DURATION must be the constant caps-derived interval, or NONE - never the
     * real inter-arrival cadence. */
    if (GST_CLOCK_TIME_IS_VALID ((GstClockTime) dur_arr[i])
        && dur_arr[i] != (gint64) NOMINAL_INTERVAL_NS)
      duration_tracks_rate = TRUE;

    if (off_end_arr[i] != off_arr[i] + 1)
      offset_broken = TRUE;
    if (i > 0 && off_arr[i] != off_arr[i - 1] + 1)
      offset_broken = TRUE;
  }

  /* --- full teardown BEFORE any assertion (fail_unless longjmps) --- */
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);

  /* --- sanity: the stimulus actually ran --- */
  fail_unless (!missing_buffer,
      "an IDR access unit produced no buffer (harness wiring bug)");
  fail_unless (!pts_invalid, "a buffer carried an invalid PTS");

  /* No silent drops: every injected frame produced exactly one buffer. */
  fail_unless (delivered == NUM_FRAMES,
      "delivered %d buffers for %d injected frames (silent drop/duplicate)",
      delivered, NUM_FRAMES);

  /* OFFSET stays a strict +1 sequence regardless of the rate mismatch. */
  fail_unless (!offset_broken,
      "GST_BUFFER_OFFSET broke the strict +1 sequence (or OFFSET_END != OFFSET+1)");

  /* DURATION is the nominal caps-derived interval (or NONE), never the real
   * ~24 fps inter-arrival cadence. */
  fail_unless (!duration_tracks_rate,
      "DURATION tracked the mismatched real cadence instead of staying the "
      "constant caps-derived %" G_GINT64_FORMAT " us (1/fps)",
      (gint64) NOMINAL_INTERVAL_NS / 1000);

  /* The core contract: PTS tracks running-time within 5 ms even though the
   * device delivers slower than negotiated. RED under the estimator (PTS built
   * from prev_pts + frame_interval drifts ~8.33 ms/frame from the real arrival
   * clock at a 30-vs-24 fps mismatch); GREEN after Option B (Task 5). */
  fail_unless (max_inst_err <= INSTANT_BOUND_NS,
      "PTS did not track running-time: max |PTS_i - ts_i| = %" G_GINT64_FORMAT
      " us > %" G_GINT64_FORMAT " us (5 ms). The estimator stamps PTS from "
      "prev_pts + nominal frame_interval and ignores the real arrival ts, so a "
      "slower device makes PTS drift off the running clock. Fixed by Option B "
      "(Task 5).", max_inst_err / 1000, (gint64) INSTANT_BOUND_NS / 1000);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* framerate_mismatch_no_renegotiate: caps stable + no drops over the pipe   */
/* ------------------------------------------------------------------------- */

#define NO_RENEG_NUM_BUFFERS 20

static gint g_buffers_seen;     /* atomic */
static gint g_caps_events;      /* atomic */
static gint g_offset_violation; /* atomic */
static guint64 nr_off_prev;     /* streaming-thread local */
static gboolean nr_off_have_prev;

static GstPadProbeReturn
mismatch_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    guint64 off = GST_BUFFER_OFFSET (buf);
    guint64 off_end = GST_BUFFER_OFFSET_END (buf);

    if (off == GST_BUFFER_OFFSET_NONE) {
      g_atomic_int_set (&g_offset_violation, 1);
    } else {
      if (nr_off_have_prev && off != nr_off_prev + 1)
        g_atomic_int_set (&g_offset_violation, 1);
      if (off_end != off + 1)
        g_atomic_int_set (&g_offset_violation, 1);
      nr_off_prev = off;
      nr_off_have_prev = TRUE;
    }
    g_atomic_int_inc (&g_buffers_seen);
  } else if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
    if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS)
      g_atomic_int_inc (&g_caps_events);
  }
  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_framerate_mismatch_no_renegotiate)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();            /* picks up MOCK_UVC_FRAME_INTERVAL_US from env */

  g_atomic_int_set (&g_buffers_seen, 0);
  g_atomic_int_set (&g_caps_events, 0);
  g_atomic_int_set (&g_offset_violation, 0);
  nr_off_prev = 0;
  nr_off_have_prev = FALSE;

  GstElement *pipeline = gst_pipeline_new ("mismatch-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");
  fail_unless (pipeline && src && sink, "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "index", "0", "num-buffers", NO_RENEG_NUM_BUFFERS, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *spad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (spad,
      GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      mismatch_probe, NULL, NULL);
  gst_object_unref (spad);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  GstMessageType msg_type = msg ? GST_MESSAGE_TYPE (msg) : GST_MESSAGE_UNKNOWN;
  if (msg)
    gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  gint buffers = g_atomic_int_get (&g_buffers_seen);
  gint caps_events = g_atomic_int_get (&g_caps_events);

  fail_unless (msg_type == GST_MESSAGE_EOS,
      "expected EOS, got %s", gst_message_type_get_name (msg_type));

  /* No mid-stream renegotiation: the slower device must not trigger a fresh
   * caps negotiation - exactly one CAPS event reaches downstream. */
  fail_unless (caps_events == 1,
      "expected exactly 1 CAPS event (no renegotiation), saw %d", caps_events);

  /* No silent drops: every requested buffer was delivered with a strict +1
   * OFFSET sequence despite the cadence mismatch. */
  fail_unless (buffers == NO_RENEG_NUM_BUFFERS,
      "delivered %d buffers; expected %d (silent drop under cadence mismatch)",
      buffers, NO_RENEG_NUM_BUFFERS);
  fail_unless (!g_atomic_int_get (&g_offset_violation),
      "GST_BUFFER_OFFSET broke the strict +1 sequence under cadence mismatch");
}

GST_END_TEST;

static Suite *
framerate_mismatch_suite (void)
{
  Suite *s = suite_create ("libuvch264src-framerate-mismatch");

  TCase *tc_pts = tcase_create ("framerate_mismatch_pts");
  tcase_set_timeout (tc_pts, 60);
  tcase_add_test (tc_pts, test_framerate_mismatch_pts);
  suite_add_tcase (s, tc_pts);

  TCase *tc_nr = tcase_create ("framerate_mismatch_no_renegotiate");
  tcase_set_timeout (tc_nr, 60);
  tcase_add_test (tc_nr, test_framerate_mismatch_no_renegotiate);
  suite_add_tcase (s, tc_nr);

  return s;
}

GST_CHECK_MAIN (framerate_mismatch);
