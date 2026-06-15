/* Device-selection tests for the libuvch264src element's start() path.
 *
 * Unlike test_mock_smoke.c (which loads a mock-backed plugin .so and drives it
 * via env vars), this links the element translation units, the libuvc mock, and
 * the driver into ONE executable and registers the element type statically. The
 * mock's state therefore lives in the same process, so the device-list leak
 * counter (mock_uvc_device_lists_outstanding) is directly observable and device
 * counts can be set programmatically. Each gst-check test is exposed as its own
 * ctest entry through GST_CHECKS.
 *
 * Covered (Task 6):
 *   test_device_zero          zero devices -> fatal RESOURCE/NOT_FOUND on the bus
 *   test_index_validate       non-ordinal index -> fatal RESOURCE/SETTINGS
 *   test_device_restart_leak  repeated start/stop frees every device list
 *                             (ref-before-free), nothing left outstanding
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

static void
setup (void)
{
  /* fakesink lives in coreelements; the harness blanks the system plugin path
   * for isolation, so load just that one plugin explicitly. */
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  /* The element is linked in, not loaded from a plugin .so; register its type
   * once so gst_element_factory_make() finds it. */
  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }

  mock_uvc_reset ();
}

static GstElement *
build_pipeline (const gchar * index_value)
{
  GstElement *pipeline = gst_pipeline_new ("test-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  if (index_value != NULL)
    g_object_set (src, "index", index_value, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");
  return pipeline;
}

/* Drive the pipeline to PAUSED, assert start() failed, and return the GError
 * carried by the fatal bus message. Caller frees it with g_clear_error(). */
static GError *
expect_start_error (GstElement * pipeline)
{
  GstStateChangeReturn sret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
  fail_unless (sret == GST_STATE_CHANGE_FAILURE,
      "expected start() to fail the state change");

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg =
      gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  fail_unless (msg != NULL, "expected a fatal ERROR message on the bus");

  GError *gerr = NULL;
  gchar *dbg = NULL;
  gst_message_parse_error (msg, &gerr, &dbg);
  g_free (dbg);
  gst_message_unref (msg);
  gst_object_unref (bus);
  return gerr;
}

GST_START_TEST (test_device_zero)
{
  mock_uvc_set_device_count (0);

  GstElement *pipeline = build_pipeline ("0");
  GError *gerr = expect_start_error (pipeline);

  fail_unless (g_error_matches (gerr, GST_RESOURCE_ERROR,
          GST_RESOURCE_ERROR_NOT_FOUND),
      "expected RESOURCE/NOT_FOUND, got %s (%d): %s",
      gerr ? g_quark_to_string (gerr->domain) : "(none)",
      gerr ? gerr->code : -1, gerr ? gerr->message : "(none)");

  g_clear_error (&gerr);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
}

GST_END_TEST;

GST_START_TEST (test_index_validate)
{
  /* A bare, non-negative integer is the only valid index; everything else must
   * fail loudly instead of silently selecting device 0 (the old atoi() bug). */
  const gchar *bad[] = {
    "not-a-number",
    "",
    "-1",
    "12abc",
    "99999999999999999999",      /* overflows long -> ERANGE */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++) {
    mock_uvc_reset ();

    GstElement *pipeline = build_pipeline (bad[i]);
    GError *gerr = expect_start_error (pipeline);

    fail_unless (g_error_matches (gerr, GST_RESOURCE_ERROR,
            GST_RESOURCE_ERROR_SETTINGS),
        "index \"%s\": expected RESOURCE/SETTINGS, got %s (%d)", bad[i],
        gerr ? g_quark_to_string (gerr->domain) : "(none)",
        gerr ? gerr->code : -1);

    g_clear_error (&gerr);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
  }
}

GST_END_TEST;

GST_START_TEST (test_device_restart_leak)
{
  mock_uvc_set_device_count (1);

  const int baseline = mock_uvc_device_lists_outstanding ();
  GstElement *pipeline = build_pipeline ("0");

  gboolean start_failed = FALSE;
  int worst_outstanding = baseline;

  for (int cycle = 0; cycle < 3; cycle++) {
    if (gst_element_set_state (pipeline, GST_STATE_PAUSED) ==
        GST_STATE_CHANGE_FAILURE)
      start_failed = TRUE;

    /* The list is released inside start() (the selected device survives via its
     * own reference), so nothing stays outstanding after a successful start. */
    int outstanding = mock_uvc_device_lists_outstanding ();
    if (outstanding > worst_outstanding)
      worst_outstanding = outstanding;

    gst_element_set_state (pipeline, GST_STATE_NULL);
  }

  const int final_outstanding = mock_uvc_device_lists_outstanding ();

  /* Tear down before asserting: a failed fail_unless longjmps out of the test,
   * and a still-running control thread would otherwise keep the non-forking
   * process alive until the ctest timeout. Asserting last keeps regressions
   * fast and clean. */
  gst_object_unref (pipeline);

  fail_if (start_failed, "start() failed during a restart cycle (a missing "
      "ref-before-free makes uvc_open reject the freed device)");
  fail_unless (worst_outstanding == baseline,
      "device list leaked during a restart (worst outstanding=%d, baseline=%d)",
      worst_outstanding, baseline);
  fail_unless (final_outstanding == baseline,
      "device list leaked across restarts (outstanding=%d, baseline=%d)",
      final_outstanding, baseline);
}

GST_END_TEST;

static Suite *
device_select_suite (void)
{
  Suite *s = suite_create ("libuvch264src-device-select");
  TCase *tc = tcase_create ("device-select");

  tcase_set_timeout (tc, 60);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_device_zero);
  tcase_add_test (tc, test_index_validate);
  tcase_add_test (tc, test_device_restart_leak);

  return s;
}

GST_CHECK_MAIN (device_select);
