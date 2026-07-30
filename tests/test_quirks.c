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
 *   quirks_production_table_*        every vid:pid WITHOUT a shipped row gets no
 *   ..._untouched                    flags and no limits, so the quirk cannot
 *                                    leak onto another camera.
 *   quirks_double_probe              a device keyed to QUIRK_DOUBLE_PROBE makes
 *                                    negotiate() call uvc_get_stream_ctrl_format_
 *                                    size() exactly TWICE (libuvc #242).
 *   quirks_empty_table_single_probe  with no matching quirk the count stays at
 *                                    exactly 1 (default byte-identical probe).
 *   quirks_osmo_row_caps_phantom_    the SHIPPED 2ca3:0023 row caps at
 *   rates                            3840x2160x30 and excludes 4K@60/50/48.
 *   quirks_max_fps_ceiling           uvc_quirk_max_fps() per resolution, incl.
 *                                    the zero-dimension divide-by-zero guard.
 *   quirks_limits_pure_lookup        quirks_limits_in() over a test-local table,
 *                                    incl. a rate that is NOT armed by its flag.
 *   quirks_ladder_without_quirk_*    red/green pair over the Osmo's REAL H.264
 *   quirks_ladder_with_osmo_quirk_*  ladder: unquirked still picks 3840x2160@60,
 *                                    the quirked Osmo lands on 3840x2160@30.
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"
#include "quirks.h"

/* The quirked device's USB IDs; the test table below keys QUIRK_DOUBLE_PROBE to
 * this pair and mock_uvc_set_device_descriptor() advertises it. */
#define QUIRK_TEST_VID 0x1234u
#define QUIRK_TEST_PID 0x5678u

/* The DJI Osmo Pocket 3, which owns the one row in the SHIPPED quirk table. The
 * cap is asserted as an explicit literal rather than read back from the table, so
 * that silently changing the shipped number fails a test instead of passing.
 *
 * The tripwire has already earned its keep: raising quirks.c from 62 208 000 to
 * the board-proven 248 832 000 (4K@30) turned FOUR cases red on its own - this
 * literal, the three max_fps ceilings, and both end-to-end ladder outcomes - so
 * the shipped number could not move silently. Those expectations were re-POINTED
 * at the new value, never relaxed: the exclusions below are still exact, the
 * ladder cases still assert one exact mode, nothing is skipped. Any future move
 * of this cap must move this literal with it, deliberately. */
#define OSMO_VID 0x2ca3u
#define OSMO_PID 0x0023u
#define OSMO_MAX_PIXEL_RATE 248832000u

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

GST_START_TEST (test_quirks_production_table_unrelated_devices_untouched)
{
  /* No test table injected (setup() cleared it), so this reads the SHIPPED table.
   * Every device without a row must come back with no flags and no limits, which
   * is what keeps negotiation byte-for-byte unchanged for all other cameras. */
  const guint16 others[][2] = {
    { QUIRK_TEST_VID, QUIRK_TEST_PID },
    { 0x0000, 0x0000 },
    { 0xffff, 0xffff },
    { 0x19f7, 0x0080 },         /* RODE HDMI to USB-C, the other camera on the bench */
    { OSMO_VID, 0x0020 },       /* the Osmo's non-UVC RNDIS+MSC mode */
    { OSMO_VID, OSMO_PID + 1 }, /* right vendor, neighbouring product */
    { OSMO_VID - 1, OSMO_PID }, /* neighbouring vendor, right product */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (others); i++) {
    guint16 vid = others[i][0], pid = others[i][1];

    fail_unless (uvc_quirks_lookup (vid, pid) == 0,
        "%04x:%04x must match no production quirk row", vid, pid);

    uvc_quirk_limits_t limits;
    uvc_quirks_limits (vid, pid, &limits);
    fail_unless (limits.flags == 0 && limits.max_pixel_rate == 0,
        "%04x:%04x must get zeroed limits", vid, pid);

    /* An uncapped device may select anything, including the rate that is phantom
     * on the Osmo - the quirk must not leak across vid:pid. */
    fail_unless (uvc_quirk_mode_selectable (&limits, 3840, 2160, 60),
        "%04x:%04x must still be allowed 3840x2160@60", vid, pid);
    fail_unless (uvc_quirk_max_fps (&limits, 3840, 2160) == G_MAXUINT,
        "%04x:%04x must report an unlimited fps ceiling", vid, pid);
  }
}

GST_END_TEST;

/* ------------------------------------------------------------------------- *
 * QUIRK_MAX_PIXEL_RATE: the shipped Osmo row, and the pure cap predicates.
 * ------------------------------------------------------------------------- */

GST_START_TEST (test_quirks_osmo_row_caps_phantom_rates)
{
  uvc_quirk_limits_t limits;
  uvc_quirks_limits (OSMO_VID, OSMO_PID, &limits);

  fail_unless (limits.flags & QUIRK_MAX_PIXEL_RATE,
      "the shipped %04x:%04x row must set QUIRK_MAX_PIXEL_RATE", OSMO_VID,
      OSMO_PID);
  fail_unless (limits.max_pixel_rate == OSMO_MAX_PIXEL_RATE,
      "expected a %u px/s cap, got %" G_GUINT64_FORMAT, OSMO_MAX_PIXEL_RATE,
      limits.max_pixel_rate);

  /* The three PHANTOM rates: advertised at 4K, provably deliver nothing. */
  fail_if (uvc_quirk_mode_selectable (&limits, 3840, 2160, 60),
      "3840x2160@60 is the phantom mode the quirk exists to exclude");
  fail_if (uvc_quirk_mode_selectable (&limits, 3840, 2160, 50),
      "3840x2160@50 must be excluded");
  fail_if (uvc_quirk_mode_selectable (&limits, 3840, 2160, 48),
      "3840x2160@48 must be excluded");

  /* The board-proven mode the cap is set to permit (300/300 AUs, 2026-07-30). */
  fail_unless (uvc_quirk_mode_selectable (&limits, 3840, 2160, 30),
      "3840x2160@30 is the confirmed-good ceiling and MUST be selectable here");

  /* Every rate at or below the confirmed-good ceiling stays selectable. */
  fail_unless (uvc_quirk_mode_selectable (&limits, 1920, 1080, 30),
      "1920x1080@30 is the confirmed-good mode and MUST remain selectable");
  fail_unless (uvc_quirk_mode_selectable (&limits, 1920, 1080, 25), "1080p25");
  fail_unless (uvc_quirk_mode_selectable (&limits, 1920, 1080, 24), "1080p24");
  fail_unless (uvc_quirk_mode_selectable (&limits, 1080, 1920, 30),
      "the portrait twin has the same pixel count and must behave the same");
  fail_unless (uvc_quirk_mode_selectable (&limits, 1280, 720, 30), "720p30");
  fail_unless (uvc_quirk_mode_selectable (&limits, 720, 1280, 25), "portrait 720p25");
}

GST_END_TEST;

GST_START_TEST (test_quirks_max_fps_ceiling)
{
  uvc_quirk_limits_t limits;
  uvc_quirks_limits (OSMO_VID, OSMO_PID, &limits);

  /* The continuous-frame-interval branch of negotiate() clamps a whole fps RANGE
   * rather than filtering a list, so it needs the ceiling as a number. */
  fail_unless (uvc_quirk_max_fps (&limits, 1920, 1080) == 120,
      "1080p ceiling must be 248832000/2073600 = 120 fps, got %u",
      uvc_quirk_max_fps (&limits, 1920, 1080));
  fail_unless (uvc_quirk_max_fps (&limits, 1280, 720) == 270,
      "720p ceiling must be 248832000/921600 = 270 fps, got %u",
      uvc_quirk_max_fps (&limits, 1280, 720));
  fail_unless (uvc_quirk_max_fps (&limits, 3840, 2160) == 30,
      "4K ceiling must be 248832000/8294400 = exactly 30 fps, got %u",
      uvc_quirk_max_fps (&limits, 3840, 2160));

  /* A zero dimension must not divide by zero. */
  fail_unless (uvc_quirk_max_fps (&limits, 0, 1080) == G_MAXUINT,
      "a zero width must be treated as uncapped, not a division by zero");
  fail_unless (uvc_quirk_max_fps (&limits, 1920, 0) == G_MAXUINT,
      "a zero height must be treated as uncapped");
}

GST_END_TEST;

GST_START_TEST (test_quirks_limits_pure_lookup)
{
  static const uvc_quirk_entry_t table[] = {
    { 0x0bda, 0x5830, QUIRK_DOUBLE_PROBE, 0 },
    { QUIRK_TEST_VID, QUIRK_TEST_PID, QUIRK_MAX_PIXEL_RATE, 1000u },
    /* A rate WITHOUT the flag: the flag is what arms the cap, so this row must
     * impose no limit at all. */
    { 0x1111, 0x2222, QUIRK_DOUBLE_PROBE, 1000u },
  };
  uvc_quirk_limits_t limits;

  quirks_limits_in (table, G_N_ELEMENTS (table), QUIRK_TEST_VID, QUIRK_TEST_PID,
      &limits);
  fail_unless (limits.flags == QUIRK_MAX_PIXEL_RATE, "flags must come through");
  fail_unless (limits.max_pixel_rate == 1000u, "the row's rate must come through");
  fail_if (uvc_quirk_mode_selectable (&limits, 100, 100, 1), "10000 > 1000");
  fail_unless (uvc_quirk_mode_selectable (&limits, 10, 10, 10), "1000 <= 1000");

  quirks_limits_in (table, G_N_ELEMENTS (table), 0x1111, 0x2222, &limits);
  fail_unless (limits.flags == QUIRK_DOUBLE_PROBE, "flags must come through");
  fail_unless (limits.max_pixel_rate == 0,
      "a max_pixel_rate without QUIRK_MAX_PIXEL_RATE must be ignored");
  fail_unless (uvc_quirk_mode_selectable (&limits, 3840, 2160, 60),
      "an unarmed cap must select everything");

  quirks_limits_in (table, G_N_ELEMENTS (table), 0xdead, 0xbeef, &limits);
  fail_unless (limits.flags == 0 && limits.max_pixel_rate == 0,
      "a miss must zero the limits");

  quirks_limits_in (NULL, 0, QUIRK_TEST_VID, QUIRK_TEST_PID, &limits);
  fail_unless (limits.flags == 0 && limits.max_pixel_rate == 0,
      "a NULL table must zero the limits");

  /* NULL limits means "no device quirk known", which cannot constrain anything. */
  fail_unless (uvc_quirk_mode_selectable (NULL, 3840, 2160, 60),
      "NULL limits must select everything");
  fail_unless (uvc_quirk_max_fps (NULL, 3840, 2160) == G_MAXUINT,
      "NULL limits must report an unlimited ceiling");
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

/* ------------------------------------------------------------------------- *
 * End to end: negotiate() against the Osmo's REAL advertised H.264 ladder.
 * ------------------------------------------------------------------------- */

/* Play until a buffer flows, then report the caps the source actually negotiated
 * (read off the sink pad, so it is the fixated result downstream received). */
static gboolean
play_and_get_negotiated (GstElement * pipeline, gint * width, gint * height,
    gint * fps_n, gint * fps_d)
{
  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);

  gboolean ok = FALSE;
  if (gst_element_set_state (pipeline, GST_STATE_PLAYING) !=
      GST_STATE_CHANGE_FAILURE) {
    gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
    while (g_atomic_int_get (&g_buffers_seen) <= 0
        && g_get_monotonic_time () < deadline) {
      g_usleep (2 * G_TIME_SPAN_MILLISECOND);
    }

    GstCaps *caps = gst_pad_get_current_caps (pad);
    if (caps != NULL) {
      GstStructure *s = gst_caps_get_structure (caps, 0);
      ok = gst_structure_get_int (s, "width", width)
          && gst_structure_get_int (s, "height", height)
          && gst_structure_get_fraction (s, "framerate", fps_n, fps_d);
      gst_caps_unref (caps);
    }
  }

  gst_object_unref (pad);
  gst_object_unref (sink);
  return ok && g_atomic_int_get (&g_buffers_seen) > 0;
}

/* THE DEFECT, reproduced. With no quirk row the max-area-then-max-fps preference
 * lands on 3840x2160@60 - the mode that negotiates cleanly on real hardware and
 * then delivers zero frames. This is the red half of the pair: it must keep
 * passing, because it pins the untouched behavior every OTHER camera still gets. */
GST_START_TEST (test_quirks_ladder_without_quirk_picks_phantom_4k60)
{
  mock_uvc_set_format_mode (MOCK_UVC_FORMAT_OSMO_LADDER);
  mock_uvc_set_device_descriptor (0, QUIRK_TEST_VID, QUIRK_TEST_PID, NULL, 0, 0);

  GstElement *pipeline = build_pipeline ();
  gint w = 0, h = 0, fps_n = 0, fps_d = 0;
  gboolean got = play_and_get_negotiated (pipeline, &w, &h, &fps_n, &fps_d);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "the unquirked device must negotiate and stream");
  fail_unless (w == 3840 && h == 2160 && fps_n == 60 && fps_d == 1,
      "an unquirked device must still pick the top advertised mode "
      "(3840x2160@60); got %dx%d@%d/%d", w, h, fps_n, fps_d);
}

GST_END_TEST;

/* The Osmo needs the double probe too, and BOTH quirks have to survive being ORed
 * into one row: the cap must still land the mode it allows (3840x2160@30) while
 * the probe runs twice.
 *
 * Measured on hardware (192.168.78.131, 2026-07-30): with a SINGLE probe, the
 * first negotiation after the device was committed to a SMALLER mode fails
 * `Unable to get stream control: Invalid mode` 23 times out of 23 - including
 * 720p30 -> 1920x1080@30, i.e. the element's own shipping mode. libuvc compares
 * the requested control against the device's GET_CUR readback
 * (_uvc_stream_params_negotiated, stream.c) and the Osmo answers the first probe
 * from the previously committed mode. Two probes: 7 of 7 pass. */
GST_START_TEST (test_quirks_osmo_row_double_probes_and_still_caps)
{
  mock_uvc_set_format_mode (MOCK_UVC_FORMAT_OSMO_LADDER);
  mock_uvc_set_device_descriptor (0, OSMO_VID, OSMO_PID, NULL, 0, 0);

  uvc_quirk_limits_t limits;
  uvc_quirks_limits (OSMO_VID, OSMO_PID, &limits);
  fail_unless (limits.flags & QUIRK_DOUBLE_PROBE,
      "the shipped %04x:%04x row must set QUIRK_DOUBLE_PROBE", OSMO_VID,
      OSMO_PID);
  fail_unless (limits.flags & QUIRK_MAX_PIXEL_RATE,
      "the double probe must not displace the pixel-rate cap");

  GstElement *pipeline = build_pipeline ();
  gint w = 0, h = 0, fps_n = 0, fps_d = 0;
  gboolean got = play_and_get_negotiated (pipeline, &w, &h, &fps_n, &fps_d);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "the Osmo must negotiate and stream");
  fail_unless (mock_uvc_format_size_call_count () == 2,
      "the shipped Osmo row must probe TWICE; got %d",
      mock_uvc_format_size_call_count ());
  fail_unless (w == 3840 && h == 2160 && fps_n == 30 && fps_d == 1,
      "the cap must still land on 3840x2160@30 with the double probe armed; "
      "got %dx%d@%d/%d", w, h, fps_n, fps_d);
}

GST_END_TEST;

/* THE FIX. Same ladder, but the device now identifies as the Osmo, so the
 * QUIRK_MAX_PIXEL_RATE row applies: every rate above the cap is dropped BEFORE the
 * preference runs. At the shipped 4K@30 cap that leaves 3840x2160@30 as the top
 * surviving rate, which is also why the hardware capture behind this value is
 * conclusive: negotiate() prefers max area then max fps and 4K@60/50/48 are
 * dropped, so 4K@30 wins BY CONSTRUCTION even under permissive caps - the board
 * run could not have measured some other mode. */
GST_START_TEST (test_quirks_ladder_with_osmo_quirk_avoids_phantom)
{
  mock_uvc_set_format_mode (MOCK_UVC_FORMAT_OSMO_LADDER);
  mock_uvc_set_device_descriptor (0, OSMO_VID, OSMO_PID, NULL, 0, 0);

  GstElement *pipeline = build_pipeline ();
  gint w = 0, h = 0, fps_n = 0, fps_d = 0;
  gboolean got = play_and_get_negotiated (pipeline, &w, &h, &fps_n, &fps_d);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (got, "the quirked device must still negotiate and stream");

  fail_if (fps_n > 30 && fps_d == 1,
      "the quirk must keep negotiation off the phantom 4K@60/50/48 rates; "
      "got %dx%d@%d/%d", w, h, fps_n, fps_d);
  fail_unless (w == 3840 && h == 2160 && fps_n == 30 && fps_d == 1,
      "the cap must land on the confirmed-good 3840x2160@30; "
      "got %dx%d@%d/%d", w, h, fps_n, fps_d);

  /* The published caps matter as much as the chosen mode: a phantom rate left in
   * the framerate list would still be offered downstream as a valid option. */
  fail_unless ((guint64) w * h * fps_n <= OSMO_MAX_PIXEL_RATE,
      "the negotiated mode must sit at or under the cap");
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
  tcase_add_test (tc, test_quirks_production_table_unrelated_devices_untouched);
  tcase_add_test (tc, test_quirks_double_probe);
  tcase_add_test (tc, test_quirks_empty_table_single_probe);
  tcase_add_test (tc, test_quirks_osmo_row_caps_phantom_rates);
  tcase_add_test (tc, test_quirks_max_fps_ceiling);
  tcase_add_test (tc, test_quirks_limits_pure_lookup);
  tcase_add_test (tc, test_quirks_ladder_without_quirk_picks_phantom_4k60);
  tcase_add_test (tc, test_quirks_ladder_with_osmo_quirk_avoids_phantom);
  tcase_add_test (tc, test_quirks_osmo_row_double_probes_and_still_caps);

  return s;
}

GST_CHECK_MAIN (quirks);
