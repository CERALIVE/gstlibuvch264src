/* Live-source polish tests (Task 19).
 *
 * Statically links the element translation units, the libuvc mock, and the
 * driver into ONE executable (like test_pts_monotonic) so the tests can both
 * drive a real PLAYING pipeline against the mock feeder AND reach into the
 * instance struct / call frame_callback() directly. GST_CHECKS selects one case
 * per ctest invocation (see tests/CMakeLists.txt).
 *
 *   live_source_polish      Drive PLAYING against the mock (30 fps H.264). Assert
 *                           the LATENCY query reports a live source with a
 *                           minimum latency of exactly one frame interval (not
 *                           the GstBaseSrc default of 0), and that consecutive
 *                           buffers carry a strictly monotonic GST_BUFFER_OFFSET
 *                           with OFFSET_END == OFFSET + 1.
 *
 *   spspps_write_on_change  Call frame_callback() directly with crafted access
 *                           units. The SPS/PPS cache file must be written on the
 *                           first (changed) parameter set, NOT rewritten when an
 *                           identical set repeats, and written again only when
 *                           the SPS actually changes (L10).
 */

#include <gst/check/gstcheck.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

/* gstcheck.h already defines GST_CAT_DEFAULT (check_debug); drop it so the
 * element's internal header can install its own category without a warning. */
#undef GST_CAT_DEFAULT
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"
#include "spspps_path.h"
#include "mock_libuvc.h"

/* The mock advertises a single 30 fps interval (333333 * 100 ns), so negotiate()
 * computes frame_interval = 1e9 / 30. */
#define EXPECTED_FRAME_INTERVAL_NS (GST_SECOND / 30)

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
/* live_source_polish: LATENCY query + monotonic buffer offsets              */
/* ------------------------------------------------------------------------- */

static gint offset_violation;   /* atomic: an OFFSET broke the +1 sequence */
static gint offset_checked;     /* atomic: buffers with a valid OFFSET examined */
static gint seen_first;         /* atomic: at least one buffer has arrived */
static guint64 off_prev;        /* streaming-thread local */
static gboolean off_have_prev;  /* streaming-thread local */

static GstPadProbeReturn
offset_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (!(GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER))
    return GST_PAD_PROBE_OK;

  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  guint64 off = GST_BUFFER_OFFSET (buf);
  guint64 off_end = GST_BUFFER_OFFSET_END (buf);

  if (off != GST_BUFFER_OFFSET_NONE) {
    if (off_have_prev && off != off_prev + 1)
      g_atomic_int_set (&offset_violation, 1);
    if (off_end != off + 1)
      g_atomic_int_set (&offset_violation, 1);
    off_prev = off;
    off_have_prev = TRUE;
    g_atomic_int_inc (&offset_checked);
  } else {
    g_atomic_int_set (&offset_violation, 1);
  }

  g_atomic_int_set (&seen_first, 1);
  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_live_source_polish)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();            /* VALID mode, 30 fps H.264 */

  g_atomic_int_set (&offset_violation, 0);
  g_atomic_int_set (&offset_checked, 0);
  g_atomic_int_set (&seen_first, 0);
  off_have_prev = FALSE;
  off_prev = 0;

  GstElement *pipeline = gst_pipeline_new ("live-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");
  fail_unless (pipeline && src && sink, "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "num-buffers", 60, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *spad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (spad, GST_PAD_PROBE_TYPE_BUFFER, offset_probe, NULL, NULL);
  gst_object_unref (spad);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait until streaming so negotiate() has set frame_interval before querying. */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!g_atomic_int_get (&seen_first) && g_get_monotonic_time () < deadline)
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  gboolean streamed = g_atomic_int_get (&seen_first);

  /* Capture the negotiated interval and the LATENCY query result, but do not
     assert yet: a fail_unless() here would longjmp past the pipeline teardown
     below and leave the feeder thread running, hanging the test until ctest's
     timeout. Record, tear down, then assert last. */
  GstClockTime fi = (GstClockTime) GST_LIBUVC_H264_SRC (src)->frame_interval;

  gboolean queried = FALSE, live = FALSE;
  GstClockTime min_latency = GST_CLOCK_TIME_NONE, max_latency = GST_CLOCK_TIME_NONE;
  if (streamed) {
    GstQuery *q = gst_query_new_latency ();
    GstPad *srcpad = gst_element_get_static_pad (src, "src");
    queried = gst_pad_query (srcpad, q);
    gst_object_unref (srcpad);
    if (queried)
      gst_query_parse_latency (q, &live, &min_latency, &max_latency);
    gst_query_unref (q);
  }

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  GstMessageType msg_type = msg ? GST_MESSAGE_TYPE (msg) : GST_MESSAGE_UNKNOWN;
  if (msg)
    gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (streamed, "no buffer arrived; mock idle");
  fail_unless (fi == EXPECTED_FRAME_INTERVAL_NS,
      "frame_interval %" G_GUINT64_FORMAT " != expected %" G_GUINT64_FORMAT,
      (guint64) fi, (guint64) EXPECTED_FRAME_INTERVAL_NS);
  fail_unless (queried, "src pad refused the LATENCY query");
  fail_unless (live, "LATENCY query did not report a live source");
  fail_unless (min_latency == fi,
      "min latency %" G_GUINT64_FORMAT " != frame interval %" G_GUINT64_FORMAT,
      (guint64) min_latency, (guint64) fi);
  /* Option B: max latency must be a finite bound (the element does not buffer
     ahead, so the handler sets max == min == the caps-derived frame interval),
     never GST_CLOCK_TIME_NONE, and never below min. */
  fail_unless (GST_CLOCK_TIME_IS_VALID (max_latency),
      "max latency is GST_CLOCK_TIME_NONE; expected a finite bound");
  fail_unless (max_latency >= min_latency,
      "max latency %" G_GUINT64_FORMAT " < min latency %" G_GUINT64_FORMAT,
      (guint64) max_latency, (guint64) min_latency);
  fail_unless (msg_type == GST_MESSAGE_EOS,
      "expected EOS, got %s", gst_message_type_get_name (msg_type));
  fail_unless (g_atomic_int_get (&offset_checked) >= 10,
      "only %d buffers carried an OFFSET", g_atomic_int_get (&offset_checked));
  fail_unless (!g_atomic_int_get (&offset_violation),
      "GST_BUFFER_OFFSET was not a strict +1 sequence (or OFFSET_END mismatch)");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* latency_pre_negotiate: LATENCY query before negotiate() defers gracefully */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_latency_pre_negotiate)
{
  register_element ();

  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  fail_unless (src != NULL, "failed to create libuvch264src");

  /* frame_interval is 0 until negotiate() derives it from the caps fps, so the
     handler's frame_interval>0 branch is skipped and the query defers to the
     GstBaseSrc default. The graceful contract: the query is still answered for a
     live source, and the element fabricates NO caps-derived latency — min stays
     at the base-class default of 0, never EXPECTED_FRAME_INTERVAL_NS (that value
     only appears once negotiate() runs, asserted by live_source_polish). */
  GstQuery *q = gst_query_new_latency ();
  GstPad *srcpad = gst_element_get_static_pad (src, "src");
  gboolean queried = gst_pad_query (srcpad, q);
  gst_object_unref (srcpad);

  gboolean live = FALSE;
  GstClockTime min_latency = 0, max_latency = 0;
  if (queried)
    gst_query_parse_latency (q, &live, &min_latency, &max_latency);
  gst_query_unref (q);

  gst_object_unref (src);

  fail_unless (queried, "src pad refused the pre-negotiate LATENCY query");
  fail_unless (live, "pre-negotiate LATENCY query did not report a live source");
  fail_unless (min_latency == 0,
      "pre-negotiate min latency %" G_GUINT64_FORMAT
      " != base-class default 0 (handler fabricated a latency)",
      (guint64) min_latency);
  fail_unless (min_latency != EXPECTED_FRAME_INTERVAL_NS,
      "pre-negotiate latency leaked the caps-derived frame interval");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* spspps_write_on_change: cache written only when parameter sets change     */
/* ------------------------------------------------------------------------- */

/* Append an Annex-B NAL (4-byte start code + header byte + payload). */
static size_t
put_nal (uint8_t * b, uint8_t header, uint8_t fill, size_t payload_len)
{
  b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 1; b[4] = header;
  memset (b + 5, fill, payload_len);
  return 5 + payload_len;
}

/* SPS(type 7) + PPS(type 8) + IDR(type 5). sps_fill selects the SPS payload so
 * two access units can carry identical or differing parameter sets. */
static size_t
craft_au (uint8_t * buf, uint8_t sps_fill)
{
  size_t n = 0;
  n += put_nal (buf + n, 0x67, sps_fill, 12);
  n += put_nal (buf + n, 0x68, 0xAB, 4);
  n += put_nal (buf + n, 0x65, 0xCD, 48);
  return n;
}

static void
drain_queue (GstLibuvcH264Src * self)
{
  GstBuffer *b;
  while ((b = g_async_queue_try_pop (self->frame_queue)) != NULL)
    gst_buffer_unref (b);
}

static gboolean
cache_exists (const char *path)
{
  struct stat st;
  return stat (path, &st) == 0;
}

static void
feed_one (GstLibuvcH264Src * self, uint8_t * buf, size_t len)
{
  uvc_frame_t frame;
  memset (&frame, 0, sizeof (frame));
  frame.data = buf;
  frame.data_bytes = len;
  frame.frame_format = UVC_FRAME_FORMAT_H264;
  frame_callback (&frame, self);
  drain_queue (self);
}

GST_START_TEST (test_spspps_write_on_change)
{
  register_element ();

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->negotiated_width = 1920;
  self->negotiated_height = 1080;
  self->frame_interval = EXPECTED_FRAME_INTERVAL_NS;
  self->base_time = 0;          /* skip the first-frame base_time latch */
  self->clock = gst_system_clock_obtain ();

  char path[4096];
  fail_unless (spspps_build_path (path, sizeof (path), g_getenv ("HOME"),
          self->index, 0, 1920, 1080) > 0, "could not build cache path");
  unlink (path);

  uint8_t buf[512];

  /* First parameter set differs from the init() default, so it is written. */
  feed_one (self, buf, craft_au (buf, 0x11));
  fail_unless (cache_exists (path), "cache not written for the first SPS");

  /* Identical parameter set: delete the file and prove it is NOT rewritten. */
  unlink (path);
  feed_one (self, buf, craft_au (buf, 0x11));
  fail_if (cache_exists (path),
      "cache rewritten for an unchanged SPS (L10 regression)");

  /* Changed SPS: the cache must be written again. */
  feed_one (self, buf, craft_au (buf, 0x22));
  fail_unless (cache_exists (path), "cache not written after the SPS changed");

  unlink (path);
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* h265_vps_write_on_change: VPS+SPS+PPS cache write-on-change for H.265      */
/*                                                                           */
/* Drives the H.265 path directly through frame_callback() - the same        */
/* deterministic pattern as test_spspps_write_on_change above. The element's  */
/* parser keys off self->frame_format, so setting it to UVC_FRAME_FORMAT_H265 */
/* IS what selects the H.265 path here; the mock feeder is not used (it feeds  */
/* a fixed VPS every frame and cannot model a mid-stream VPS change, which     */
/* this test requires). The ctest entry still exports MOCK_UVC_FRAME_FORMAT=   */
/* H265 so the harness env declares the codec the case exercises.             */
/* ------------------------------------------------------------------------- */

/* Append an H.265 Annex-B NAL: 4-byte start code + 2-byte NAL header + payload.
 * The parser reads the type from the first header byte as (byte0 >> 1) & 0x3F,
 * so byte0 == (type << 1). byte1 (layer_id/tid) is leading payload to the
 * parser; 0x01 keeps it non-zero so no spurious start code can form. */
static size_t
put_nal_h265 (uint8_t * b, uint8_t type, uint8_t fill, size_t payload_len)
{
  b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 1;
  b[4] = (uint8_t) ((type & 0x3F) << 1);
  b[5] = 0x01;
  memset (b + 6, fill, payload_len);
  return 6 + payload_len;
}

/* VPS(32) + SPS(33) + PPS(34) + IDR_W_RADL(19). vps_fill selects the VPS payload
 * so two access units can carry an identical or a differing VPS while SPS/PPS
 * stay fixed - isolating the VPS as the sole change driver. */
static size_t
craft_au_h265 (uint8_t * buf, uint8_t vps_fill)
{
  size_t n = 0;
  n += put_nal_h265 (buf + n, 32, vps_fill, 8);   /* VPS */
  n += put_nal_h265 (buf + n, 33, 0x11, 12);      /* SPS */
  n += put_nal_h265 (buf + n, 34, 0x22, 4);       /* PPS */
  n += put_nal_h265 (buf + n, 19, 0xCD, 48);      /* IDR_W_RADL (Task 3 maps 19) */
  return n;
}

static void
feed_one_h265 (GstLibuvcH264Src * self, uint8_t * buf, size_t len)
{
  uvc_frame_t frame;
  memset (&frame, 0, sizeof (frame));
  frame.data = buf;
  frame.data_bytes = len;
  frame.frame_format = UVC_FRAME_FORMAT_H265;
  frame_callback (&frame, self);
  drain_queue (self);
}

/* Stamp the cache file's mtime far in the past (~2001) so any subsequent
 * fopen("wb") rewrite jumps it to "now" and is detectable regardless of the
 * filesystem's mtime granularity. */
static void
stamp_old_mtime (const char *path)
{
  struct timeval tv[2];
  tv[0].tv_sec = 1000000000; tv[0].tv_usec = 0;
  tv[1].tv_sec = 1000000000; tv[1].tv_usec = 0;
  fail_unless (utimes (path, tv) == 0, "could not stamp cache mtime");
}

static time_t
mtime_of (const char *path)
{
  struct stat st;
  return (stat (path, &st) == 0) ? st.st_mtime : (time_t) -1;
}

static ino_t
inode_of (const char *path)
{
  struct stat st;
  return (stat (path, &st) == 0) ? st.st_ino : (ino_t) 0;
}

GST_START_TEST (test_h265_vps_write_on_change)
{
  register_element ();

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H265;
  self->negotiated_width = 1920;
  self->negotiated_height = 1080;
  self->frame_interval = EXPECTED_FRAME_INTERVAL_NS;
  self->base_time = 0;          /* skip the first-frame base_time latch */
  self->clock = gst_system_clock_obtain ();

  char path[4096];
  fail_unless (spspps_build_path (path, sizeof (path), g_getenv ("HOME"),
          self->index, 1 /* is_h265 */, 1920, 1080) > 0,
      "could not build the H.265 cache path");

  /* The cache key must embed the codec tag and the resolution (L5, extended to
     H.265): filename carries "h265" + "WxH". */
  fail_unless (strstr (path, "h265") != NULL,
      "H.265 cache path missing the 'h265' codec tag: %s", path);
  fail_unless (strstr (path, "1920x1080") != NULL,
      "H.265 cache path missing the WxH resolution: %s", path);

  unlink (path);

  uint8_t buf[512];

  /* IDR #1: the parameter set differs from the init() default, so it is
     written exactly once. */
  feed_one_h265 (self, buf, craft_au_h265 (buf, 0x55));
  fail_unless (cache_exists (path), "cache not written for the first H.265 IDR");
  ino_t ino0 = inode_of (path);

  /* IDR #2 and #3: identical VPS+SPS+PPS. Stamp the mtime into the past and
     prove neither feed advances it (no fopen("wb") truncate occurred) and the
     inode is unchanged - i.e. the file is written exactly once for 3 identical
     IDRs (L10, extended to VPS). */
  stamp_old_mtime (path);
  time_t old_m = mtime_of (path);

  feed_one_h265 (self, buf, craft_au_h265 (buf, 0x55));  /* IDR #2 */
  fail_unless (mtime_of (path) == old_m,
      "cache rewritten for an unchanged H.265 parameter set (IDR #2)");
  fail_unless (inode_of (path) == ino0, "cache inode changed on IDR #2");

  feed_one_h265 (self, buf, craft_au_h265 (buf, 0x55));  /* IDR #3 */
  fail_unless (mtime_of (path) == old_m,
      "cache rewritten for an unchanged H.265 parameter set (IDR #3)");
  fail_unless (inode_of (path) == ino0, "cache inode changed on IDR #3");

  /* IDR #4: the VPS payload changes, so the cache MUST be rewritten. fopen("wb")
     truncates in place (inode is preserved across a rewrite on most file
     systems), so mtime advancing past the stamped epoch is the rewrite witness. */
  feed_one_h265 (self, buf, craft_au_h265 (buf, 0x66));
  fail_unless (mtime_of (path) > old_m,
      "cache not rewritten after the VPS changed (IDR #4)");

  /* The cached blob must include the VPS NAL: it leads the file (VPS||SPS||PPS),
     its H.265 NAL header carries type 32 (byte0 == 32 << 1), the changed VPS
     payload (0x66) is present in the VPS region, and the total size equals
     vps+sps+pps so the VPS genuinely participates in the blob. */
  FILE *fp = fopen (path, "rb");
  fail_unless (fp != NULL, "could not reopen the cached parameter-set file");
  unsigned char blob[1024];
  size_t got = fread (blob, 1, sizeof (blob), fp);
  fclose (fp);

  fail_unless (self->vps_length > 0, "VPS was never latched into the element");
  gsize expect =
      (gsize) self->vps_length + self->sps_length + self->pps_length;
  fail_unless (got == expect,
      "cached blob size %" G_GSIZE_FORMAT " != vps+sps+pps %" G_GSIZE_FORMAT,
      (gsize) got, expect);
  fail_unless (blob[0] == 0 && blob[1] == 0 && blob[2] == 0 && blob[3] == 1,
      "cached blob does not begin with a 4-byte Annex-B start code");
  fail_unless (blob[4] == (32 << 1),
      "first cached NAL is not a VPS (header byte0 %u != %u)",
      (unsigned) blob[4], (unsigned) (32 << 1));
  fail_unless (memchr (blob, 0x66, (size_t) self->vps_length) != NULL,
      "changed VPS payload byte not found in the cached VPS NAL");

  unlink (path);
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);
}

GST_END_TEST;

static Suite *
live_source_suite (void)
{
  Suite *s = suite_create ("libuvch264src-live-source");

  TCase *tc_polish = tcase_create ("live_source_polish");
  tcase_set_timeout (tc_polish, 60);
  tcase_add_test (tc_polish, test_live_source_polish);
  suite_add_tcase (s, tc_polish);

  TCase *tc_latency = tcase_create ("latency_pre_negotiate");
  tcase_set_timeout (tc_latency, 60);
  tcase_add_test (tc_latency, test_latency_pre_negotiate);
  suite_add_tcase (s, tc_latency);

  TCase *tc_write = tcase_create ("spspps_write_on_change");
  tcase_set_timeout (tc_write, 60);
  tcase_add_test (tc_write, test_spspps_write_on_change);
  suite_add_tcase (s, tc_write);

  TCase *tc_h265 = tcase_create ("h265_vps_write_on_change");
  tcase_set_timeout (tc_h265, 60);
  tcase_add_test (tc_h265, test_h265_vps_write_on_change);
  suite_add_tcase (s, tc_h265);

  return s;
}

GST_CHECK_MAIN (live_source);
