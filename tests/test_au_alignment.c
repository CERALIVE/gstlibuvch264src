/* Access-unit alignment tests for frame_callback() (Task 15).
 *
 * The pad template advertises alignment=au for both codecs, so every GstBuffer
 * the element pushes must be exactly ONE access unit. These cases drive
 * frame_callback() directly with crafted Annex-B deliveries - the same
 * deterministic pattern test_live_source.c uses - and assert on the buffers that
 * land in the element's frame queue. Statically linked (element TUs + libuvc
 * mock in one executable) so the queue is reachable in-process. GST_CHECKS
 * selects one case per ctest invocation (see tests/CMakeLists.txt).
 *
 *   au_single_slice_characterization  CHARACTERIZATION. Pins the single-slice
 *                                     1080p path: one delivery carrying one
 *                                     picture of one slice emits exactly one
 *                                     buffer holding byte-for-byte the delivered
 *                                     bytes. It is the control that proves the
 *                                     aggregation below changed nothing for the
 *                                     common case.
 *
 *   au_multi_slice_aud                A picture split across several slices, led
 *                                     by an Access Unit Delimiter, must arrive as
 *                                     ONE buffer - not one buffer per slice.
 *
 *   au_aud_less_fallback              The same guarantee without an AUD in the
 *                                     bitstream, via first-slice-of-new-picture
 *                                     detection: slices of one picture aggregate,
 *                                     a second picture in the same delivery
 *                                     starts a new buffer, and a non-VCL NAL
 *                                     sitting between the two pictures belongs to
 *                                     the SECOND one.
 */

#include <gst/check/gstcheck.h>
#include <string.h>
#include <stdint.h>

/* gstcheck.h already defines GST_CAT_DEFAULT (check_debug); drop it so the
 * element's internal header can install its own category without a warning. */
#undef GST_CAT_DEFAULT
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"

#define EXPECTED_FRAME_INTERVAL_NS (GST_SECOND / 30)

/* H.264 NAL types (ITU-T H.264 Table 7-1). */
#define NT_NONIDR 1
#define NT_IDR    5
#define NT_SEI    6
#define NT_SPS    7
#define NT_PPS    8
#define NT_AUD    9

/* First payload byte of a slice NAL, which is where first_mb_in_slice starts.
 * ue(v) 0 is the single bit `1`, so a set top bit means first_mb_in_slice == 0 -
 * the first slice of a picture - and a clear top bit means a continuation slice. */
#define FIRST_SLICE     0x88
#define CONTINUE_SLICE  0x42

#define MAX_CAPTURED 16

/* Append one Annex-B NAL: 4-byte start code, the H.264 header byte
 * (nal_ref_idc=3), then `payload_len` payload bytes whose first is `first` and
 * whose remainder is 0xAB. No emitted byte is 0x00, so the only start codes in a
 * fixture are the ones placed here. Returns bytes written. */
static size_t
put_nal (uint8_t * b, uint8_t nal_type, uint8_t first, size_t payload_len)
{
  size_t n = 0;
  b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = (uint8_t) (0x60 | (nal_type & 0x1F));
  fail_unless (payload_len >= 1, "a crafted NAL needs at least one payload byte");
  b[n++] = first;
  memset (b + n, 0xAB, payload_len - 1);
  return n + payload_len - 1;
}

typedef struct
{
  GstBuffer *buf[MAX_CAPTURED];
  guint n;
} captured_t;

static void
captured_clear (captured_t * c)
{
  for (guint i = 0; i < c->n; i++)
    gst_buffer_unref (c->buf[i]);
  c->n = 0;
}

/* Feed one delivery through frame_callback() and collect everything it queued. */
static void
feed (GstLibuvcH264Src * self, const uint8_t * data, size_t len, captured_t * out)
{
  uvc_frame_t frame;
  memset (&frame, 0, sizeof (frame));
  frame.data = (void *) data;
  frame.data_bytes = len;
  frame.frame_format = UVC_FRAME_FORMAT_H264;
  frame_callback (&frame, self);

  captured_clear (out);
  GstBuffer *b;
  while ((b = g_async_queue_try_pop (self->frame_queue)) != NULL) {
    fail_unless (out->n < MAX_CAPTURED, "more buffers queued than the fixture holds");
    out->buf[out->n++] = b;
  }
}

/* Assert buffer `idx` carries exactly `len` bytes equal to `expect`. */
static void
assert_bytes (const captured_t * c, guint idx, const uint8_t * expect, size_t len,
    const char *what)
{
  fail_unless (idx < c->n, "%s: no buffer at index %u (only %u captured)", what,
      idx, c->n);

  GstBuffer *buf = c->buf[idx];
  gsize size = gst_buffer_get_size (buf);
  fail_unless (size == len,
      "%s: buffer %u is %" G_GSIZE_FORMAT " bytes, expected %" G_GSIZE_FORMAT,
      what, idx, size, (gsize) len);

  GstMapInfo map;
  fail_unless (gst_buffer_map (buf, &map, GST_MAP_READ), "%s: buffer %u unmappable",
      what, idx);
  int diff = memcmp (map.data, expect, len);
  gst_buffer_unmap (buf, &map);
  fail_unless (diff == 0, "%s: buffer %u bytes differ from the expected access unit",
      what, idx);
}

static void
assert_offsets (const captured_t * c, guint idx, guint64 expect_offset,
    const char *what)
{
  fail_unless (idx < c->n, "%s: no buffer at index %u", what, idx);
  GstBuffer *buf = c->buf[idx];
  fail_unless (GST_BUFFER_OFFSET (buf) == expect_offset,
      "%s: buffer %u OFFSET is %" G_GUINT64_FORMAT ", expected %" G_GUINT64_FORMAT,
      what, idx, GST_BUFFER_OFFSET (buf), expect_offset);
  fail_unless (GST_BUFFER_OFFSET_END (buf) == expect_offset + 1,
      "%s: buffer %u OFFSET_END is not OFFSET + 1", what, idx);
}

static void
assert_timed (const captured_t * c, guint idx, const char *what)
{
  fail_unless (idx < c->n, "%s: no buffer at index %u", what, idx);
  GstBuffer *buf = c->buf[idx];
  fail_unless (GST_BUFFER_PTS_IS_VALID (buf), "%s: buffer %u carries no PTS", what, idx);
  fail_unless (GST_BUFFER_DTS (buf) == GST_BUFFER_PTS (buf),
      "%s: buffer %u DTS != PTS (this element emits no B-frames)", what, idx);
  fail_unless (GST_BUFFER_DURATION (buf) == EXPECTED_FRAME_INTERVAL_NS,
      "%s: buffer %u DURATION is not the nominal frame interval", what, idx);
}

static GstLibuvcH264Src *
make_src (void)
{
  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->negotiated_width = 1920;
  self->negotiated_height = 1080;
  self->frame_interval = EXPECTED_FRAME_INTERVAL_NS;
  self->base_time = 0;          /* skip the first-frame base_time latch */
  self->clock = gst_system_clock_obtain ();
  return self;
}

static void
free_src (GstLibuvcH264Src * self)
{
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);
}

/* ------------------------------------------------------------------------- */
/* au_single_slice_characterization                                          */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_au_single_slice_characterization)
{
  GstLibuvcH264Src *self = make_src ();
  captured_t got = { {NULL}, 0 };
  uint8_t frame[512];
  size_t len;

  /* The 1080p keyframe shape: parameter sets plus one slice. The element caches
     the parameter sets and re-prepends them, so the emitted buffer reproduces
     the delivered bytes exactly. */
  len = 0;
  len += put_nal (frame + len, NT_SPS, 0x64, 27);
  len += put_nal (frame + len, NT_PPS, 0xEE, 4);
  len += put_nal (frame + len, NT_IDR, FIRST_SLICE, 96);
  feed (self, frame, len, &got);

  fail_unless (got.n == 1,
      "single-slice keyframe delivery emitted %u buffers, expected exactly 1", got.n);
  assert_bytes (&got, 0, frame, len, "single-slice keyframe");
  assert_offsets (&got, 0, 0, "single-slice keyframe");
  assert_timed (&got, 0, "single-slice keyframe");
  GstClockTime first_pts = GST_BUFFER_PTS (got.buf[0]);

  /* The 1080p delta shape: one bare slice, no parameter sets. */
  len = 0;
  len += put_nal (frame + len, NT_NONIDR, FIRST_SLICE, 64);
  feed (self, frame, len, &got);

  fail_unless (got.n == 1,
      "single-slice delta delivery emitted %u buffers, expected exactly 1", got.n);
  assert_bytes (&got, 0, frame, len, "single-slice delta");
  assert_offsets (&got, 0, 1, "single-slice delta");
  assert_timed (&got, 0, "single-slice delta");
  fail_unless (GST_BUFFER_PTS (got.buf[0]) > first_pts,
      "the second access unit did not advance the PTS");

  captured_clear (&got);
  free_src (self);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* au_multi_slice_aud                                                        */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_au_multi_slice_aud)
{
  GstLibuvcH264Src *self = make_src ();
  captured_t got = { {NULL}, 0 };
  uint8_t frame[1024];
  size_t len;

  /* AUD-led multi-slice keyframe: one picture, three slices. The AUD marks the
     access unit exactly, so no heuristic is involved. */
  len = 0;
  len += put_nal (frame + len, NT_AUD, 0x10, 1);
  len += put_nal (frame + len, NT_SPS, 0x64, 27);
  len += put_nal (frame + len, NT_PPS, 0xEE, 4);
  len += put_nal (frame + len, NT_IDR, FIRST_SLICE, 80);
  len += put_nal (frame + len, NT_IDR, CONTINUE_SLICE, 72);
  len += put_nal (frame + len, NT_IDR, CONTINUE_SLICE, 64);
  feed (self, frame, len, &got);

  fail_unless (got.n == 1,
      "AUD-led multi-slice keyframe emitted %u buffers, expected exactly 1 "
      "access unit", got.n);
  assert_bytes (&got, 0, frame, len, "AUD multi-slice keyframe");
  assert_offsets (&got, 0, 0, "AUD multi-slice keyframe");
  assert_timed (&got, 0, "AUD multi-slice keyframe");

  /* AUD-led multi-slice delta picture, no parameter sets. */
  len = 0;
  len += put_nal (frame + len, NT_AUD, 0x10, 1);
  len += put_nal (frame + len, NT_NONIDR, FIRST_SLICE, 56);
  len += put_nal (frame + len, NT_NONIDR, CONTINUE_SLICE, 48);
  len += put_nal (frame + len, NT_NONIDR, CONTINUE_SLICE, 40);
  feed (self, frame, len, &got);

  fail_unless (got.n == 1,
      "AUD-led multi-slice delta emitted %u buffers, expected exactly 1 "
      "access unit", got.n);
  assert_bytes (&got, 0, frame, len, "AUD multi-slice delta");
  assert_offsets (&got, 0, 1, "AUD multi-slice delta");
  assert_timed (&got, 0, "AUD multi-slice delta");

  captured_clear (&got);
  free_src (self);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* au_aud_less_fallback                                                      */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_au_aud_less_fallback)
{
  GstLibuvcH264Src *self = make_src ();
  captured_t got = { {NULL}, 0 };
  uint8_t frame[1024];
  size_t len;

  /* No AUD anywhere. Two slices of ONE picture: only the first carries
     first_mb_in_slice == 0, so nothing splits them. */
  len = 0;
  len += put_nal (frame + len, NT_SPS, 0x64, 27);
  len += put_nal (frame + len, NT_PPS, 0xEE, 4);
  len += put_nal (frame + len, NT_IDR, FIRST_SLICE, 80);
  len += put_nal (frame + len, NT_IDR, CONTINUE_SLICE, 72);
  feed (self, frame, len, &got);

  fail_unless (got.n == 1,
      "AUD-less multi-slice keyframe emitted %u buffers, expected exactly 1 "
      "access unit", got.n);
  assert_bytes (&got, 0, frame, len, "AUD-less multi-slice keyframe");
  assert_offsets (&got, 0, 0, "AUD-less multi-slice keyframe");

  /* Two whole pictures in ONE delivery, still no AUD. The second picture's first
     slice is the only boundary signal available, and it must be honoured. */
  size_t pic_a = 0, pic_b = 0;
  len = 0;
  len += put_nal (frame + len, NT_NONIDR, FIRST_SLICE, 56);
  len += put_nal (frame + len, NT_NONIDR, CONTINUE_SLICE, 48);
  pic_a = len;
  len += put_nal (frame + len, NT_NONIDR, FIRST_SLICE, 40);
  len += put_nal (frame + len, NT_NONIDR, CONTINUE_SLICE, 32);
  pic_b = len - pic_a;
  feed (self, frame, len, &got);

  fail_unless (got.n == 2,
      "a delivery carrying two pictures emitted %u buffers, expected 2 access "
      "units", got.n);
  assert_bytes (&got, 0, frame, pic_a, "AUD-less first picture");
  assert_bytes (&got, 1, frame + pic_a, pic_b, "AUD-less second picture");
  assert_offsets (&got, 0, 1, "AUD-less first picture");
  assert_offsets (&got, 1, 2, "AUD-less second picture");
  fail_unless (GST_BUFFER_PTS (got.buf[1]) > GST_BUFFER_PTS (got.buf[0]),
      "two access units from one delivery must not share a PTS");

  /* A non-VCL NAL sitting between two pictures leads the SECOND access unit -
     the cut goes at the head of that run, not at the slice after it. */
  len = 0;
  len += put_nal (frame + len, NT_NONIDR, FIRST_SLICE, 56);
  len += put_nal (frame + len, NT_NONIDR, CONTINUE_SLICE, 48);
  pic_a = len;
  len += put_nal (frame + len, NT_SEI, 0x11, 8);
  len += put_nal (frame + len, NT_NONIDR, FIRST_SLICE, 40);
  len += put_nal (frame + len, NT_NONIDR, CONTINUE_SLICE, 32);
  pic_b = len - pic_a;
  feed (self, frame, len, &got);

  fail_unless (got.n == 2,
      "a delivery with an interleaved SEI emitted %u buffers, expected 2 access "
      "units", got.n);
  assert_bytes (&got, 0, frame, pic_a, "picture preceding the SEI");
  assert_bytes (&got, 1, frame + pic_a, pic_b, "SEI-led picture");

  captured_clear (&got);
  free_src (self);
}

GST_END_TEST;

static Suite *
au_alignment_suite (void)
{
  Suite *s = suite_create ("au_alignment");

  TCase *tc_char = tcase_create ("single_slice_characterization");
  tcase_set_timeout (tc_char, 60);
  tcase_add_test (tc_char, test_au_single_slice_characterization);
  suite_add_tcase (s, tc_char);

  TCase *tc_aud = tcase_create ("multi_slice_aud");
  tcase_set_timeout (tc_aud, 60);
  tcase_add_test (tc_aud, test_au_multi_slice_aud);
  suite_add_tcase (s, tc_aud);

  TCase *tc_fallback = tcase_create ("aud_less_fallback");
  tcase_set_timeout (tc_fallback, 60);
  tcase_add_test (tc_fallback, test_au_aud_less_fallback);
  suite_add_tcase (s, tc_fallback);

  return s;
}

GST_CHECK_MAIN (au_alignment);
