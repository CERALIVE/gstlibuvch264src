/* Opt-in transfer-buffers property tests for the libuvch264src element (Task 11).
 * Like test_reconnect.c, this statically links the element translation units, the
 * libuvc mock, and the driver into ONE executable and registers the element type
 * directly, so the mock's uvc_set_transfer_buffers() call counter and the count
 * latched at uvc_start_streaming() are observable in-process.
 *
 * The property mirrors the max-payload contract: 0 is the sentinel that never
 * touches the fork API (negotiation byte-for-byte unchanged), a nonzero value is
 * clamped to [2,100] in the apply helper (NOT the param spec, which is the full
 * 0..255 uint8 range so an in-range set never trips a GObject range warning), and
 * the applied value is re-armed on a successful reconnect and reported by a
 * read-back.
 *
 * The suite is built in two shapes selected by a compile-time macro:
 *   - the normal target compiles the real fork apply path (HAVE_UVC_TRANSFER_
 *     BUFFERS) and runs the ON cases;
 *   - test_transfer_buffers_off defines LIBUVCH264SRC_NO_TRANSFER_BUFFERS_API to
 *     simulate a libuvc WITHOUT the symbol (LIBUVC_USE_FORK=OFF) and runs the OFF
 *     case, which asserts the one-GST_WARNING no-op instead of a setter call.
 *
 * GST_CHECKS selects a single test per ctest invocation (see tests/CMakeLists.txt).
 */

#include <string.h>

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

#if defined(HAVE_UVC_TRANSFER_BUFFERS) && !defined(LIBUVCH264SRC_NO_TRANSFER_BUFFERS_API)
#define TB_API_AVAILABLE 1
#else
#define TB_API_AVAILABLE 0
#endif

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

static gint buffers_seen;               /* atomic: buffers that reached the sink */
static gint saw_transfer_warning;       /* atomic: a "transfer-buffers" WARNING seen */

static GstPadProbeReturn
count_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&buffers_seen);
  return GST_PAD_PROBE_OK;
}

static void
transfer_warning_log_func (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  (void) category; (void) file; (void) function; (void) line; (void) object;
  (void) user_data;
  if (level <= GST_LEVEL_WARNING) {
    const gchar *m = gst_debug_message_get (message);
    if (m != NULL && strstr (m, "transfer-buffers") != NULL)
      g_atomic_int_set (&saw_transfer_warning, 1);
  }
}

static GstElement *
build_pipeline (GstElement ** src_out)
{
  GstElement *pipeline = gst_pipeline_new ("transfer-buffers-pipeline");
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

  if (src_out != NULL)
    *src_out = src;
  return pipeline;
}

static gboolean
play_until_buffer (GstElement * pipeline)
{
  if (gst_element_set_state (pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    return FALSE;

  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&buffers_seen) <= 0
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  return g_atomic_int_get (&buffers_seen) > 0;
}

/* ------------------------------------------------------------------------- */
/* test_transfer_buffers_sentinel_no_call                                    */
/*                                                                           */
/* Default (unset) transfer-buffers is the 0 sentinel: the element must never */
/* call the fork API, so the mock setter call count stays 0 and the count     */
/* latched at streaming start is 0 - negotiation is byte-for-byte unchanged.  */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_transfer_buffers_sentinel_no_call)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);

  gboolean got = play_until_buffer (pipeline);

  gint calls = mock_uvc_transfer_buffers_call_count ();
  guint8 started = mock_uvc_last_started_transfer_buffers ();

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "stream did not start with default (unset) transfer-buffers");
  fail_unless (calls == 0,
      "unset transfer-buffers must issue NO uvc_set_transfer_buffers call "
      "(byte-for-byte unchanged negotiation); got %d call(s)", calls);
  fail_unless (started == 0,
      "unset transfer-buffers must leave the library default in place; got %u",
      started);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_transfer_buffers_applied_before_start                                */
/*                                                                           */
/* A nonzero value is pushed via uvc_set_transfer_buffers() BEFORE the stream */
/* starts (the fork rejects it mid-stream), and a read-back reports it.       */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_transfer_buffers_applied_before_start)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "transfer-buffers", 4u, NULL);

  gboolean got = play_until_buffer (pipeline);

  gint calls = mock_uvc_transfer_buffers_call_count ();
  guint8 started = mock_uvc_last_started_transfer_buffers ();
  guint effective = 0;
  g_object_get (src, "transfer-buffers", &effective, NULL);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "stream must start with an in-range transfer-buffers value");
  fail_unless (calls >= 1,
      "a nonzero transfer-buffers must call uvc_set_transfer_buffers; got %d",
      calls);
  fail_unless (started == 4,
      "transfer-buffers 4 must be applied BEFORE streaming start; got %u",
      started);
  fail_unless (effective == 4u,
      "read-back must report the applied transfer-buffers (4); got %u", effective);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_transfer_buffers_clamped                                             */
/*                                                                           */
/* A value inside the 0..255 param spec but above the [2,100] band (200) is   */
/* clamped to 100 by the apply helper with a warning, and the read-back       */
/* reports the effective clamped value. The set stays inside the spec so no   */
/* GObject range warning (and no gst-check longjmp) is triggered.             */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_transfer_buffers_clamped)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();

  g_atomic_int_set (&buffers_seen, 0);
  g_atomic_int_set (&saw_transfer_warning, 0);
  gst_debug_set_active (TRUE);
  gst_debug_set_threshold_for_name ("libuvch264src", GST_LEVEL_WARNING);
  gst_debug_add_log_function (transfer_warning_log_func, NULL, NULL);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "transfer-buffers", 200u, NULL);

  gboolean got = play_until_buffer (pipeline);

  guint effective = 0;
  g_object_get (src, "transfer-buffers", &effective, NULL);
  guint8 started = mock_uvc_last_started_transfer_buffers ();

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_debug_remove_log_function (transfer_warning_log_func);
  gst_object_unref (pipeline);

  fail_unless (got, "stream must start with an out-of-band transfer-buffers value");
  fail_unless (g_atomic_int_get (&saw_transfer_warning),
      "an out-of-range transfer-buffers must log a clamp warning");
  fail_unless (started == 100,
      "transfer-buffers 200 must clamp to 100 before streaming start; got %u",
      started);
  fail_unless (effective == 100u,
      "read-back must report the clamped transfer-buffers (100); got %u",
      effective);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_transfer_buffers_reconnect_reapply                                   */
/*                                                                           */
/* A successful reconnect must re-arm BOTH opt-in overrides right before the  */
/* resumed uvc_start_streaming: transfer-buffers is pushed again, and         */
/* max-payload is re-committed (closing the pre-existing max-payload          */
/* reconnect-success coverage gap). The mock's reconnect-SUCCESS mode fails    */
/* one reopen then recovers, so the reconnect genuinely succeeds after a       */
/* failure.                                                                    */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_transfer_buffers_reconnect_reapply)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_DISCONNECT);
  mock_uvc_set_max_frames (1);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "reconnect", TRUE, "transfer-buffers", 4u,
      "max-payload", 32768u, NULL);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&buffers_seen) < 1
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  fail_unless (g_atomic_int_get (&buffers_seen) >= 1,
      "the initial stream never delivered a frame");

  /* Simulate a replug that returns after one failed reopen: healthy frames,
   * one injected reopen failure, then recovery. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_VALID);
  mock_uvc_set_max_frames (0);
  mock_uvc_set_reopen_fail_count (1);

  gint baseline = g_atomic_int_get (&buffers_seen);

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
  gint open_count = mock_uvc_open_count ();
  gint tb_calls = mock_uvc_transfer_buffers_call_count ();
  guint8 started_tb = mock_uvc_last_started_transfer_buffers ();
  guint32 started_payload = mock_uvc_last_started_payload ();
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (!errored,
      "reconnect=TRUE should suppress the disconnect error, but the pipeline "
      "errored out");
  fail_unless (resumed && open_count >= 2,
      "stream did not reopen+resume after reconnect (open count %d)", open_count);
  fail_unless (tb_calls >= 2,
      "transfer-buffers must be re-applied on the successful reconnect "
      "(>= 2 setter calls: initial + reconnect); got %d", tb_calls);
  fail_unless (started_tb == 4,
      "the resumed stream must start on the re-applied transfer-buffers (4); "
      "got %u", started_tb);
  fail_unless (started_payload == 32768u,
      "the resumed stream must start on the re-applied max-payload (32768); "
      "got %u", started_payload);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_transfer_buffers_start_streaming_io_error                            */
/*                                                                           */
/* An injected UVC_ERROR_IO from uvc_start_streaming (the A2 zero-transfer     */
/* error path) must surface as a RESOURCE-domain bus error, not a silent      */
/* stall.                                                                     */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_transfer_buffers_start_streaming_io_error)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  mock_uvc_set_start_streaming_result (UVC_ERROR_IO);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  GstBus *bus = gst_element_get_bus (pipeline);
  gboolean saw_resource_error = FALSE;
  gint64 deadline = g_get_monotonic_time () + 12 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline) {
    GstMessage *msg =
        gst_bus_timed_pop_filtered (bus, 1 * GST_SECOND, GST_MESSAGE_ERROR);
    if (msg != NULL) {
      GError *gerr = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error (msg, &gerr, &dbg);
      if (gerr != NULL && gerr->domain == GST_RESOURCE_ERROR)
        saw_resource_error = TRUE;
      g_clear_error (&gerr);
      g_free (dbg);
      gst_message_unref (msg);
      if (saw_resource_error)
        break;
    }
  }
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (saw_resource_error,
      "an injected UVC_ERROR_IO from uvc_start_streaming must post a "
      "RESOURCE-domain error");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_transfer_buffers_off_noop (LIBUVC_USE_FORK=OFF simulation)           */
/*                                                                           */
/* When the fork symbol is absent the property stays registered, but a        */
/* nonzero value emits ONE GST_WARNING and no-ops: no uvc_set_transfer_buffers */
/* call is made. Registered only for the OFF target (see tests/CMakeLists.txt). */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_transfer_buffers_off_noop)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();

  g_atomic_int_set (&buffers_seen, 0);
  g_atomic_int_set (&saw_transfer_warning, 0);
  gst_debug_set_active (TRUE);
  gst_debug_set_threshold_for_name ("libuvch264src", GST_LEVEL_WARNING);
  gst_debug_add_log_function (transfer_warning_log_func, NULL, NULL);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "transfer-buffers", 4u, NULL);

  gboolean got = play_until_buffer (pipeline);

  gint calls = mock_uvc_transfer_buffers_call_count ();
  gboolean warned = g_atomic_int_get (&saw_transfer_warning);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_debug_remove_log_function (transfer_warning_log_func);
  gst_object_unref (pipeline);

  fail_unless (got,
      "the media path must still stream when the fork API is absent");
  fail_unless (calls == 0,
      "with the fork API absent, transfer-buffers must NOT call the setter; "
      "got %d call(s)", calls);
  fail_unless (warned,
      "with the fork API absent, a nonzero transfer-buffers must log ONE "
      "warning");
}

GST_END_TEST;

static Suite *
transfer_buffers_suite (void)
{
  Suite *s = suite_create ("libuvch264src-transfer-buffers");

  TCase *tc = tcase_create ("transfer_buffers");
  tcase_set_timeout (tc, 90);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_transfer_buffers_sentinel_no_call);
  tcase_add_test (tc, test_transfer_buffers_applied_before_start);
  tcase_add_test (tc, test_transfer_buffers_clamped);
  tcase_add_test (tc, test_transfer_buffers_reconnect_reapply);
  tcase_add_test (tc, test_transfer_buffers_start_streaming_io_error);
  tcase_add_test (tc, test_transfer_buffers_off_noop);

  return s;
}

GST_CHECK_MAIN (transfer_buffers);
