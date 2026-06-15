/* Disconnect-detection and opt-in reconnect tests for the libuvch264src element
 * (Task 18). Like test_pts_monotonic.c, this statically links the element
 * translation units, the libuvc mock, and the driver into ONE executable and
 * registers the element type directly, so the mock feeder drives the real
 * create() disconnect path in the test process and the mock's open counter is
 * observable in-process (mock_uvc_open_count).
 *
 *   test_disconnect_error   With reconnect off (the default), a device that goes
 *                           silent mid-stream must surface as a RESOURCE/READ bus
 *                           error. The mock's DISCONNECT mode delivers one frame
 *                           then stops feeding (real libuvc passes no NULL frame
 *                           on unplug in callback mode), so create() infers the
 *                           disconnect from sustained silence and errors out.
 *
 *   test_reconnect_resume   With reconnect=TRUE, the same silent-source scenario
 *                           must NOT error: the element tears the dead handle
 *                           down and reopens it. The test simulates a successful
 *                           replug by switching the mock to a healthy feed once
 *                           the first frame has been seen, then asserts the
 *                           device was reopened (open count grew) and frames
 *                           resumed flowing.
 *
 * GST_CHECKS selects a single test per ctest invocation (see tests/CMakeLists.txt).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

/* gstcheck.h installs its own GST_CAT_DEFAULT (check_debug); drop it so the
 * element's internal header can install the element category without a warning.
 * The internal header exposes the instance struct, the reconnect() entry point,
 * and the backoff test seam the backoff-timing tests below drive directly. */
#undef GST_CAT_DEFAULT
#include "gstlibuvch264src_internal.h"

/* The teardown-order variant links a dedicated libusb mock (MOCK_LIBUSB_TEARDOWN)
 * so uvc_close() models the single libusb_close() and the close counters prove
 * force_usb_release() never double-closed on the reconnect path. The plain
 * variant links the real libusb and skips those assertions. */
#ifdef MOCK_LIBUSB_TEARDOWN
#include "mock_libusb.h"
#endif

/* The harness blanks GST_PLUGIN_SYSTEM_PATH; load just core-elements so fakesink
 * is available without scanning unrelated plugins. */
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

/* The element is linked in, not loaded from a plugin .so; register its type once
 * so gst_element_factory_make() finds it. */
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

static gint buffers_seen;       /* atomic: buffers that reached the sink */

static GstPadProbeReturn
count_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&buffers_seen);
  return GST_PAD_PROBE_OK;
}

static GstElement *
build_pipeline (GstElement ** src_out)
{
  GstElement *pipeline = gst_pipeline_new ("reconnect-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);

  /* The bin owns src; the caller borrows the pointer while the pipeline lives. */
  if (src_out != NULL)
    *src_out = src;
  return pipeline;
}

/* ------------------------------------------------------------------------- */
/* test_disconnect_error                                                     */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_disconnect_error)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  /* One frame, then the feed goes silent - the mock's stand-in for an unplug. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_DISCONNECT);
  mock_uvc_set_max_frames (1);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  /* reconnect defaults to FALSE, so the disconnect must surface as an error. */

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* DISCONNECT_TIMEOUT_COUNT (5) consecutive 1 s pop timeouts must elapse before
   * create() declares the disconnect, so allow generous headroom past ~5 s. */
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 12 * GST_SECOND,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  gboolean is_disconnect_error = FALSE;
  if (msg != NULL && GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *gerr = NULL;
    gchar *dbg = NULL;
    gst_message_parse_error (msg, &gerr, &dbg);
    /* The element posts its RESOURCE/READ disconnect error before returning
     * GST_FLOW_ERROR, so it is the first ERROR on the bus. */
    is_disconnect_error =
        g_error_matches (gerr, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ);
    g_clear_error (&gerr);
    g_free (dbg);
  }
  if (msg != NULL)
    gst_message_unref (msg);
  gst_object_unref (bus);

  /* Tear down before asserting: a failed fail_unless() longjmps past teardown
   * under CK_FORK=no, leaving the streaming task alive until the ctest timeout. */
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (msg != NULL,
      "expected a disconnect error within 12 s, got none "
      "(create() never detected the silent source)");
  fail_unless (is_disconnect_error,
      "expected GST_RESOURCE_ERROR_READ on disconnect; got a different message");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_reconnect_resume                                                     */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_reconnect_resume)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  /* Start in DISCONNECT mode (one frame, then silence) so the first feeder goes
   * quiet and create() detects a disconnect. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_DISCONNECT);
  mock_uvc_set_max_frames (1);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "reconnect", TRUE, NULL);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait for the first (pre-disconnect) frame. Its arrival proves the first
   * feeder already ran in DISCONNECT mode and has now gone silent, so switching
   * the mock to a healthy feed below only affects the reopened stream. */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&buffers_seen) < 1
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  fail_unless (g_atomic_int_get (&buffers_seen) >= 1,
      "the initial stream never delivered a frame");

  /* Simulate a successful replug: the reopened device feeds healthy frames
   * continuously. The reconnect feeder reads this when uvc_start_streaming
   * spawns it, so the resumed stream runs without going silent again. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_VALID);
  mock_uvc_set_max_frames (0);

  gint baseline = g_atomic_int_get (&buffers_seen);

  /* The element should detect the disconnect (~5 s), back off (~1 s), reopen,
   * and resume. Wait for frames to flow well past the single pre-disconnect one
   * AND for a second uvc_open() to confirm a real reopen happened. Fail fast if
   * an error reaches the bus instead (reconnect should suppress it). */
  GstBus *bus = gst_element_get_bus (pipeline);
  gboolean resumed = FALSE;
  gboolean errored = FALSE;
  deadline = g_get_monotonic_time () + 40 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline) {
    GstMessage *msg =
        gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (msg != NULL) {
      errored = (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
      gst_message_unref (msg);
      break;
    }
    if (g_atomic_int_get (&buffers_seen) >= baseline + 5
        && mock_uvc_open_count () >= 2) {
      resumed = TRUE;
      break;
    }
    g_usleep (20 * G_TIME_SPAN_MILLISECOND);
  }
  gint final_buffers = g_atomic_int_get (&buffers_seen);
  gint open_count = mock_uvc_open_count ();
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (!errored,
      "reconnect=TRUE should suppress the disconnect error, but the pipeline "
      "errored out");
  fail_unless (open_count >= 2,
      "expected the device to be reopened (open count >= 2), got %d", open_count);
  fail_unless (resumed && final_buffers >= baseline + 5,
      "stream did not resume after reconnect: %d buffers (baseline %d), "
      "open count %d", final_buffers, baseline, open_count);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* Reconnect exhaustion + teardown-order (Task 8)                            */
/* ------------------------------------------------------------------------- */

/* Counters captured at the moment the disconnect error reaches the bus, before
 * the pipeline is torn down to NULL (a failed fail_unless longjmps past the
 * unref, and a snapshot taken first keeps the assertions order-independent). */
typedef struct
{
  gint read_errors;            /* RESOURCE/READ errors posted by the element */
  gboolean got_flow_error;     /* basesrc STREAM error: create() returned GST_FLOW_ERROR */
  gboolean other_error;        /* an unexpected error domain (neither of the above) */
  gboolean got_eos;            /* EOS appeared (exhaustion must error, not EOS) */
  gint open_attempts;          /* total uvc_open() calls incl. injected failures */
  gint open_count;             /* successful uvc_open() calls */
  gint uvc_closes;             /* uvc_close() calls */
  gint usb_opens;              /* libusb handles opened (-1 without the libusb mock) */
  gint usb_closes;             /* libusb_close() calls (-1 without the libusb mock) */
  gint cfg_queries;            /* force_usb_release() config-descriptor queries (-1) */
} ExhaustionResult;

/* Drive the reconnect-exhaustion scenario: one frame then silence, reconnect on,
 * and every reopen fails. create() must detect the disconnect (~5 s), exhaust the
 * bounded backoff (1+2+4+8+16 s) reopening, then post exactly one RESOURCE/READ
 * error and return GST_FLOW_ERROR. Fills *res with bus + mock-counter snapshots
 * taken before teardown. */
static void
run_exhaustion_scenario (ExhaustionResult * res)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
#ifdef MOCK_LIBUSB_TEARDOWN
  mock_libusb_reset ();
#endif
  /* One frame, then the feed goes silent (the unplug stand-in). */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_DISCONNECT);
  mock_uvc_set_max_frames (1);
  /* The initial start() open succeeds; every reconnect reopen fails, so the
   * retry loop runs all RECONNECT_MAX_RETRIES attempts and then gives up. */
  mock_uvc_set_open_fail_after (1);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "reconnect", TRUE, NULL);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait for the single pre-disconnect frame so we know the first feeder ran and
   * went quiet; the reopen attempts below are what then fail. */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&buffers_seen) < 1
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  fail_unless (g_atomic_int_get (&buffers_seen) >= 1,
      "the initial stream never delivered a frame");

  /* Detection (~5 s) + backoff (1+2+4+8+16 = 31 s) then exhaustion. Count
   * RESOURCE/READ errors and keep draining ~2 s past the first to prove a second
   * one never follows (the error is posted once, right before GST_FLOW_ERROR). */
  GstBus *bus = gst_element_get_bus (pipeline);
  gint read_errors = 0;
  gboolean got_flow_error = FALSE;
  gboolean other_error = FALSE;
  gboolean got_eos = FALSE;
  gint64 drain_until = 0;
  deadline = g_get_monotonic_time () + 55 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline) {
    GstMessage *msg =
        gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (msg != NULL) {
      if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
        got_eos = TRUE;
      } else {
        GError *gerr = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error (msg, &gerr, &dbg);
        if (g_error_matches (gerr, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ)) {
          /* The element's own disconnect error. */
          read_errors++;
        } else if (gerr != NULL && gerr->domain == GST_STREAM_ERROR) {
          /* basesrc posts STREAM/FAILED ("Internal data stream error") once
           * create() returns GST_FLOW_ERROR - the expected propagation, not a
           * second element error. */
          got_flow_error = TRUE;
        } else {
          other_error = TRUE;
        }
        g_clear_error (&gerr);
        g_free (dbg);
        /* Keep draining ~2 s past the first error to prove the element never
         * posts its RESOURCE/READ a second time. */
        if (drain_until == 0)
          drain_until = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
      }
      gst_message_unref (msg);
    } else {
      if (drain_until != 0 && g_get_monotonic_time () >= drain_until)
        break;
      g_usleep (20 * G_TIME_SPAN_MILLISECOND);
    }
  }

  res->read_errors = read_errors;
  res->got_flow_error = got_flow_error;
  res->other_error = other_error;
  res->got_eos = got_eos;
  res->open_attempts = mock_uvc_open_attempt_count ();
  res->open_count = mock_uvc_open_count ();
  res->uvc_closes = mock_uvc_close_count ();
#ifdef MOCK_LIBUSB_TEARDOWN
  res->usb_opens = mock_libusb_open_count ();
  res->usb_closes = mock_libusb_close_count ();
  res->cfg_queries = mock_libusb_config_query_count ();
#else
  res->usb_opens = -1;
  res->usb_closes = -1;
  res->cfg_queries = -1;
#endif

  gst_object_unref (bus);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
}

GST_START_TEST (test_reconnect_exhaustion)
{
  ExhaustionResult res;
  run_exhaustion_scenario (&res);

  fail_if (res.other_error,
      "exhaustion posted an unexpected error (not RESOURCE/READ or STREAM)");
  fail_unless (res.read_errors == 1,
      "expected exactly one GST_RESOURCE_ERROR_READ after exhaustion, got %d",
      res.read_errors);
  /* basesrc posts STREAM/FAILED and pushes EOS downstream only AFTER create()
   * returns GST_FLOW_ERROR, so the STREAM error proves the error path was taken;
   * the drain loop only ends once an error is seen, so a clean EOS-without-error
   * would still trip read_errors == 0 above. */
  fail_unless (res.got_flow_error,
      "create() must return GST_FLOW_ERROR after exhaustion (basesrc STREAM "
      "error never reached the bus)");
  fail_unless (res.open_count == 1,
      "only the initial open should succeed, got %d successful opens",
      res.open_count);
  fail_unless (res.open_attempts == 6,
      "expected 1 initial + 5 failed reopen attempts (6 total), got %d",
      res.open_attempts);
}

GST_END_TEST;

GST_START_TEST (test_reconnect_teardown_order)
{
  ExhaustionResult res;
  run_exhaustion_scenario (&res);

  /* Same exhaustion contract as test_reconnect_exhaustion. */
  fail_if (res.other_error,
      "exhaustion posted an unexpected error (not RESOURCE/READ or STREAM)");
  fail_unless (res.read_errors == 1,
      "expected exactly one GST_RESOURCE_ERROR_READ after exhaustion, got %d",
      res.read_errors);
  fail_unless (res.got_flow_error,
      "create() must return GST_FLOW_ERROR after exhaustion");

#ifdef MOCK_LIBUSB_TEARDOWN
  /* The reconnect teardown is native: uvc_stop_streaming() -> uvc_close() ->
   * uvc_unref_device(), with uvc_close() owning the single libusb_close(). The
   * dead handle from the initial open is closed exactly once. */
  fail_unless (res.usb_opens == 1,
      "expected one libusb handle opened (the initial open), got %d",
      res.usb_opens);
  fail_unless (res.usb_closes == res.usb_opens,
      "libusb close/open unbalanced: %d opens, %d closes (double-close vector)",
      res.usb_opens, res.usb_closes);
  fail_unless (res.usb_closes == res.uvc_closes,
      "uvc_close() must own the single libusb_close(): %d uvc_close vs %d "
      "libusb_close", res.uvc_closes, res.usb_closes);
  /* force_usb_release() is the only caller of libusb_get_active_config_descriptor()
   * in this element. It is NEVER invoked on the reconnect path (which would put a
   * release before uvc_close()), and post-exhaustion stop() skips it because the
   * handle is already NULL - so the query count must stay at zero. */
  fail_unless (res.cfg_queries == 0,
      "force_usb_release() must not precede uvc_close() on any retry, but it ran "
      "%d time(s)", res.cfg_queries);
#endif
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_reconnect_backoff_sequence (Task 7)                                  */
/*                                                                           */
/* White-box assertion of the backoff schedule. Unlike the exhaustion test   */
/* above - which pays the real ~5 s detection plus the full 1+2+4+8+16 s      */
/* backoff through a live pipeline - this calls reconnect() directly and      */
/* installs a seam that records each interval and collapses the wall-clock    */
/* wait to zero, so the exponential schedule and the 5-retry cap are asserted */
/* in well under a second. With no device enumerable on any retry every       */
/* reopen fails, so all RECONNECT_MAX_RETRIES backoffs run and reconnect()    */
/* returns FALSE.                                                            */
/* ------------------------------------------------------------------------- */

/* RECONNECT_MAX_RETRIES (5) and the 1,2,4,8,16 s schedule are private to the
 * element; EXPECTED_RETRIES pins that contract and must track the element's
 * RECONNECT_MAX_RETRIES define. */
#define EXPECTED_RETRIES 5

static guint recorded_backoffs[16];
static gint recorded_backoff_count;

static gint64
record_and_skip_backoff (GstLibuvcH264Src * self, gint attempt, guint backoff_s)
{
  (void) self;
  (void) attempt;
  if (recorded_backoff_count < (gint) G_N_ELEMENTS (recorded_backoffs))
    recorded_backoffs[recorded_backoff_count] = backoff_s;
  recorded_backoff_count++;
  return 0;
}

GST_START_TEST (test_reconnect_backoff_sequence)
{
  mock_uvc_reset ();

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));

  /* A context plus one open handle stands in for the live (about-to-be-dead)
   * device, so reconnect()'s native teardown runs its uvc_close() before the
   * retry loop. */
  fail_unless (uvc_init (&self->uvc_ctx, NULL) == UVC_SUCCESS,
      "mock uvc_init failed");
  uvc_device_t **list = NULL;
  fail_unless (uvc_find_devices (self->uvc_ctx, &list, 0, 0, NULL)
      == UVC_SUCCESS && list != NULL, "mock uvc_find_devices failed");
  uvc_ref_device (list[0]);
  fail_unless (uvc_open (list[0], &self->uvc_devh) == UVC_SUCCESS,
      "mock uvc_open failed");
  self->uvc_dev = list[0];
  uvc_free_device_list (list, 1);

  /* The device is now gone: every reopen on the retry path fails, so all five
   * backoffs run and reconnect() exhausts. */
  mock_uvc_set_device_count (0);

  recorded_backoff_count = 0;
  gst_libuvc_h264_src_set_reconnect_backoff_hook (record_and_skip_backoff);

  gboolean ok = gst_libuvc_h264_src_reconnect (self);

  gint count = recorded_backoff_count;
  guint seq[EXPECTED_RETRIES] = { 0 };
  for (int i = 0; i < EXPECTED_RETRIES && i < count; i++)
    seq[i] = recorded_backoffs[i];
  gint opens = mock_uvc_open_count ();
  gint closes = mock_uvc_close_count ();

  gst_libuvc_h264_src_set_reconnect_backoff_hook (NULL);
  gst_object_unref (self);

  fail_unless (!ok,
      "reconnect() with no enumerable device must exhaust and return FALSE");
  fail_unless (count == EXPECTED_RETRIES,
      "expected exactly %d backoff intervals (RECONNECT_MAX_RETRIES), got %d",
      EXPECTED_RETRIES, count);
  fail_unless (seq[0] == 1 && seq[1] == 2 && seq[2] == 4 && seq[3] == 8
      && seq[4] == 16,
      "backoff not exponential 1,2,4,8,16 s: got %u,%u,%u,%u,%u",
      seq[0], seq[1], seq[2], seq[3], seq[4]);
  /* The reconnect teardown must let uvc_close() own the single close (never
   * force_usb_release() before it): one open, one matching close, no double. */
  fail_unless (closes == opens,
      "teardown imbalance: %d uvc_close vs %d uvc_open (double-close risk)",
      closes, opens);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_reconnect_backoff_interrupt (Task 7)                                 */
/*                                                                           */
/* The backoff must be interruptible: a NULL/PAUSED transition (which runs    */
/* the unlock() vmethod) has to wake the wait at once rather than sleeping    */
/* out the interval. A seam returns a 30 s would-be wait, so any wall-clock   */
/* dependence would hang; the real unlock() vmethod must instead return       */
/* reconnect() within a few ms.                                              */
/* ------------------------------------------------------------------------- */

static gint64
long_backoff (GstLibuvcH264Src * self, gint attempt, guint backoff_s)
{
  (void) self;
  (void) attempt;
  (void) backoff_s;
  return (gint64) 30 * G_USEC_PER_SEC;
}

typedef struct
{
  GstLibuvcH264Src *self;
  gboolean ret;
} ReconnectThreadArgs;

static gpointer
reconnect_thread_main (gpointer data)
{
  ReconnectThreadArgs *args = data;
  args->ret = gst_libuvc_h264_src_reconnect (args->self);
  return NULL;
}

GST_START_TEST (test_reconnect_backoff_interrupt)
{
  mock_uvc_reset ();
  /* No enumerable device, so without the interrupt the loop would block on the
   * 30 s backoff every retry - the interrupt is the only thing that returns. */
  mock_uvc_set_device_count (0);

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));

  gst_libuvc_h264_src_set_reconnect_backoff_hook (long_backoff);

  ReconnectThreadArgs args = { self, TRUE };
  GThread *t = g_thread_new ("reconnect", reconnect_thread_main, &args);

  /* Let reconnect() reach the cond wait, then trip the real unlock() vmethod -
   * exactly what a state change to NULL/PAUSED invokes. */
  g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  gint64 t0 = g_get_monotonic_time ();
  GST_BASE_SRC_GET_CLASS (self)->unlock (GST_BASE_SRC (self));
  g_thread_join (t);
  gint64 dt = g_get_monotonic_time () - t0;
  gboolean ret = args.ret;

  /* unlock() pushed a flush sentinel that no create() loop consumed here; drain
   * it via unlock_stop() (the resume path) so finalize() never unrefs it. */
  GST_BASE_SRC_GET_CLASS (self)->unlock_stop (GST_BASE_SRC (self));

  gst_libuvc_h264_src_set_reconnect_backoff_hook (NULL);
  gst_object_unref (self);

  fail_unless (!ret,
      "an interrupted reconnect() must return FALSE, not resume");
  fail_unless (dt < 500 * G_TIME_SPAN_MILLISECOND,
      "interrupt took %" G_GINT64_FORMAT " us, expected < 500 ms - the backoff "
      "wait was not woken by unlock()", dt);
}

GST_END_TEST;

static Suite *
reconnect_suite (void)
{
  Suite *s = suite_create ("libuvch264src-reconnect");

  TCase *tc_disc = tcase_create ("disconnect_error");
  tcase_set_timeout (tc_disc, 30);
  tcase_add_test (tc_disc, test_disconnect_error);
  suite_add_tcase (s, tc_disc);

  TCase *tc_recon = tcase_create ("reconnect_resume");
  tcase_set_timeout (tc_recon, 60);
  tcase_add_test (tc_recon, test_reconnect_resume);
  suite_add_tcase (s, tc_recon);

  TCase *tc_backoff = tcase_create ("backoff_sequence");
  tcase_set_timeout (tc_backoff, 30);
  tcase_add_test (tc_backoff, test_reconnect_backoff_sequence);
  suite_add_tcase (s, tc_backoff);

  TCase *tc_interrupt = tcase_create ("backoff_interrupt");
  tcase_set_timeout (tc_interrupt, 30);
  tcase_add_test (tc_interrupt, test_reconnect_backoff_interrupt);
  suite_add_tcase (s, tc_interrupt);

  /* Exhaustion + teardown-order both run the full ~36 s detect+backoff cycle, so
   * they get a generous per-case timeout. GST_CHECKS selects one per run. */
  TCase *tc_exhaust = tcase_create ("reconnect_exhaustion");
  tcase_set_timeout (tc_exhaust, 90);
  tcase_add_test (tc_exhaust, test_reconnect_exhaustion);
  suite_add_tcase (s, tc_exhaust);

  TCase *tc_order = tcase_create ("reconnect_teardown_order");
  tcase_set_timeout (tc_order, 90);
  tcase_add_test (tc_order, test_reconnect_teardown_order);
  suite_add_tcase (s, tc_order);

  return s;
}

GST_CHECK_MAIN (reconnect);
