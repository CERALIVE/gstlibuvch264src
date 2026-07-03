/* vid:pid quirk-seam tests for the libuvch264src element (Task 12).
 *
 * Like test_negotiate.c, the element TUs, the libuvc mock, and the driver are
 * linked into ONE statically-registered executable so the mock's device
 * descriptor (mock_uvc_set_device_descriptor) and its
 * uvc_get_stream_ctrl_format_size() call counter are observable in-process. This
 * target is the ONLY one compiled with -DLIBUVCH264SRC_TESTING (see
 * tests/CMakeLists.txt), so uvc_quirks_set_test_table() - the A14 test seam -
 * is visible here and nowhere else. Each gst-check test is its own ctest entry
 * via GST_CHECKS.
 *
 *   quirks_lookup_hit / _miss        the pure quirks_lookup_in() over a
 *                                    test-local table.
 *   quirks_production_table_empty    the shipped uvc_quirks_lookup() table has
 *                                    ZERO entries, so every vid:pid returns 0.
 *   quirks_double_probe              a device keyed to QUIRK_DOUBLE_PROBE makes
 *                                    negotiate() call uvc_get_stream_ctrl_format_
 *                                    size() exactly TWICE (libuvc #242).
 *   quirks_empty_table_single_probe  with no matching quirk the count stays at
 *                                    exactly 1 (default byte-identical probe).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"
#include "quirks.h"

/* The quirked device's USB IDs; the test table below keys QUIRK_DOUBLE_PROBE to
 * this pair and mock_uvc_set_device_descriptor() advertises it. */
#define QUIRK_TEST_VID 0x1234u
#define QUIRK_TEST_PID 0x5678u

static gint g_buffers_seen;

static GstPadProbeReturn
count_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&g_buffers_seen);
  return GST_PAD_PROBE_OK;
}

static void
setup (void)
{
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }

  mock_uvc_reset ();
  /* Always start from the production (empty) table; a test that wants a quirk
   * injects its own table and the next setup() clears it again. */
  uvc_quirks_set_test_table (NULL, 0);
  g_atomic_int_set (&g_buffers_seen, 0);
}

static GstElement *
build_pipeline (void)
{
  GstElement *pipeline = gst_pipeline_new ("quirks-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "index", "0", NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");
  return pipeline;
}

/* Drive PLAYING against the mock feeder and return TRUE once a buffer flows
 * (which proves negotiate() ran and streaming started). Caller drops to NULL. */
static gboolean
play_until_buffer (GstElement * pipeline)
{
  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  if (gst_element_set_state (pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    return FALSE;

  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&g_buffers_seen) <= 0
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  return g_atomic_int_get (&g_buffers_seen) > 0;
}

/* ------------------------------------------------------------------------- *
 * Pure lookup (quirks_lookup_in) over a test-local table - no device, no GST.
 * ------------------------------------------------------------------------- */

GST_START_TEST (test_quirks_lookup_hit)
{
  static const uvc_quirk_entry_t table[] = {
    { 0x0bda, 0x5830, QUIRK_DOUBLE_PROBE },
    { QUIRK_TEST_VID, QUIRK_TEST_PID, QUIRK_DOUBLE_PROBE },
  };

  fail_unless (quirks_lookup_in (table, G_N_ELEMENTS (table),
          QUIRK_TEST_VID, QUIRK_TEST_PID) == QUIRK_DOUBLE_PROBE,
      "exact vid:pid match must return the row's flags");
  fail_unless (quirks_lookup_in (table, G_N_ELEMENTS (table),
          0x0bda, 0x5830) == QUIRK_DOUBLE_PROBE,
      "the first row must also match");
}

GST_END_TEST;

GST_START_TEST (test_quirks_lookup_miss)
{
  static const uvc_quirk_entry_t table[] = {
    { QUIRK_TEST_VID, QUIRK_TEST_PID, QUIRK_DOUBLE_PROBE },
  };

  fail_unless (quirks_lookup_in (table, G_N_ELEMENTS (table),
          QUIRK_TEST_VID, 0x9999) == 0,
      "matching vid but wrong pid must miss");
  fail_unless (quirks_lookup_in (table, G_N_ELEMENTS (table),
          0x0000, QUIRK_TEST_PID) == 0,
      "matching pid but wrong vid must miss");
  fail_unless (quirks_lookup_in (table, G_N_ELEMENTS (table),
          0xffff, 0xffff) == 0, "no match must return 0");
  fail_unless (quirks_lookup_in (NULL, 0, QUIRK_TEST_VID, QUIRK_TEST_PID) == 0,
      "a NULL/empty table must return 0");
}

GST_END_TEST;

GST_START_TEST (test_quirks_production_table_empty)
{
  /* No test table injected (setup() cleared it), so uvc_quirks_lookup() consults
   * the shipped static table, which MUST be empty: every vid:pid returns 0. */
  fail_unless (uvc_quirks_lookup (QUIRK_TEST_VID, QUIRK_TEST_PID) == 0,
      "the production quirk table must ship EMPTY (no entry may match)");
  fail_unless (uvc_quirks_lookup (0x0000, 0x0000) == 0,
      "the production quirk table must ship EMPTY");
  fail_unless (uvc_quirks_lookup (0xffff, 0xffff) == 0,
      "the production quirk table must ship EMPTY");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- *
 * Wired-but-gated integration: the double-probe flag reaches negotiate().
 * ------------------------------------------------------------------------- */

GST_START_TEST (test_quirks_double_probe)
{
  static const uvc_quirk_entry_t table[] = {
    { QUIRK_TEST_VID, QUIRK_TEST_PID, QUIRK_DOUBLE_PROBE },
  };

  mock_uvc_set_device_descriptor (0, QUIRK_TEST_VID, QUIRK_TEST_PID, NULL, 0, 0);
  uvc_quirks_set_test_table (table, G_N_ELEMENTS (table));

  GstElement *pipeline = build_pipeline ();
  gboolean got = play_until_buffer (pipeline);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "stream must start for the quirked device");
  fail_unless (mock_uvc_format_size_call_count () == 2,
      "QUIRK_DOUBLE_PROBE must issue uvc_get_stream_ctrl_format_size TWICE; got %d",
      mock_uvc_format_size_call_count ());
}

GST_END_TEST;

GST_START_TEST (test_quirks_empty_table_single_probe)
{
  /* Same device IDs, but the injected table has NO matching row, so no quirk
   * applies and the probe count stays at the default 1 (byte-identical). */
  static const uvc_quirk_entry_t table[] = {
    { 0x0bda, 0x5830, QUIRK_DOUBLE_PROBE },
  };

  mock_uvc_set_device_descriptor (0, QUIRK_TEST_VID, QUIRK_TEST_PID, NULL, 0, 0);
  uvc_quirks_set_test_table (table, G_N_ELEMENTS (table));

  GstElement *pipeline = build_pipeline ();
  gboolean got = play_until_buffer (pipeline);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "stream must start for the unquirked device");
  fail_unless (mock_uvc_format_size_call_count () == 1,
      "an unmatched (empty) quirk table must leave the probe count at 1; got %d",
      mock_uvc_format_size_call_count ());
}

GST_END_TEST;

static Suite *
quirks_suite (void)
{
  Suite *s = suite_create ("libuvch264src-quirks");
  TCase *tc = tcase_create ("quirks");

  tcase_set_timeout (tc, 30);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_quirks_lookup_hit);
  tcase_add_test (tc, test_quirks_lookup_miss);
  tcase_add_test (tc, test_quirks_production_table_empty);
  tcase_add_test (tc, test_quirks_double_probe);
  tcase_add_test (tc, test_quirks_empty_table_single_probe);

  return s;
}

GST_CHECK_MAIN (quirks);
