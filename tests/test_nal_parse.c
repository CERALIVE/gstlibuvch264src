/*
 * Unit tests for the Annex-B NAL parser in frame_pipeline.c.
 *
 * The parser (find_nal_unit / count_nal_units / parse_nal_units) is exercised
 * directly against crafted byte buffers - no UVC device, no GStreamer runtime,
 * no mock feeder. frame_pipeline.c also defines frame_callback(), whose only
 * element-internal references are the debug category and store_spspps(); both
 * are stubbed below so the parser TU links standalone (the parser functions
 * under test touch neither). Three argv-selected suites:
 *
 *   multislice  >10 slice NALs in one frame are ALL parsed, none dropped (L2)
 *   startcode   3-byte (00 00 01) and offset-shifted frames parse (L3)
 *   bounds      truncated / oversized / merged buffers stay in bounds (ASAN)
 *
 * Buffers are allocated to their EXACT used length with g_malloc so the ASAN
 * redzone sits immediately after the last valid byte - that is what gives the
 * bounds suite teeth against an over-read.
 */

#include <glib.h>
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>
#include <libuvc/libuvc.h>

#include "frame_pipeline.h"
#include "spspps_cache.h"

/* frame_pipeline.c pulls these in through frame_callback(), which this test
   never calls; trivial definitions satisfy the linker without dragging in the
   element or the SPS/PPS cache TU. */
GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);
void spspps_key_snapshot(GstLibuvcH264Src *self, spspps_key_t *key) { (void)self; (void)key; }
void store_spspps(GstLibuvcH264Src *self, const spspps_key_t *key) { (void)self; (void)key; }

static int g_failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("  ok   - %s\n", msg);                                      \
        } else {                                                               \
            printf("  FAIL - %s\n", msg);                                      \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* H.264 NAL header bytes: forbidden_zero_bit 0, nal_ref_idc, nal_unit_type. */
#define NH_SPS    0x67  /* type 7 */
#define NH_PPS    0x68  /* type 8 */
#define NH_IDR    0x65  /* type 5 */
#define NH_NONIDR 0x61  /* type 1 */

/* Append one Annex-B NAL: a sc_len-byte start code, a header byte, then `pay`
   bytes of 0xAB (non-zero payload can never form a 00 00 01 sequence, so the
   only start codes in the buffer are the ones placed here). Returns new pos. */
static gsize append_nal(unsigned char *buf, gsize pos, int sc_len,
                        unsigned char header, gsize pay) {
    if (sc_len == 4) buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;
    buf[pos++] = header;
    for (gsize k = 0; k < pay; k++) buf[pos++] = 0xAB;
    return pos;
}

/* H.265 NAL unit types (ITU-T H.265 Table 7-1). VPS/SPS/PPS sit in the 32..34
   range that H.264 never reaches, and there are two IDR types - both keyframes. */
#define H265_TRAIL_R    1   /* non-IDR slice */
#define H265_IDR_W_RADL 19
#define H265_IDR_N_LP   20
#define H265_VPS        32
#define H265_SPS        33
#define H265_PPS        34

/* The H.265 NAL header is TWO bytes (H.264's is one): the type is the 6 bits
   nal_unit_type = (header[0] >> 1) & 0x3F. header[0] is the forbidden_zero_bit,
   the 6 type bits, then the top bit of nuh_layer_id; header[1] carries the rest
   of the layer id and nuh_temporal_id_plus1. The parser inspects only header[0]
   for the type, so header[1] is just leading payload to it. */
#define H265_NH0(type) ((unsigned char)(((type) & 0x3F) << 1))
#define H265_NH1       0x01  /* layer 0, temporal_id_plus1 = 1; never zero */

/* Append one Annex-B H.265 NAL: sc_len-byte start code, the 2-byte NAL header,
   then `pay` bytes of 0xAB. No emitted byte (header or payload) is 0x00, so the
   only start codes in the buffer are the ones placed here. Returns new pos. */
static gsize append_nal_h265(unsigned char *buf, gsize pos, int sc_len,
                             int nal_type, gsize pay) {
    if (sc_len == 4) buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;
    buf[pos++] = H265_NH0(nal_type);
    buf[pos++] = H265_NH1;
    for (gsize k = 0; k < pay; k++) buf[pos++] = 0xAB;
    return pos;
}

static int run_multislice(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* One access unit: SPS + PPS + IDR + 12 non-IDR slices = 15 NAL units. The
       retired fixed cap was 10, so >10 is the case that used to drop slices. */
    gsize size = 0;
    size += 4 + 1 + 8;            /* SPS */
    size += 4 + 1 + 4;            /* PPS */
    size += 4 + 1 + 32;           /* IDR */
    size += 12 * (4 + 1 + 16);    /* 12 non-IDR slices */
    unsigned char *buf = g_malloc(size);

    gsize pos = 0;
    pos = append_nal(buf, pos, 4, NH_SPS, 8);
    pos = append_nal(buf, pos, 4, NH_PPS, 4);
    pos = append_nal(buf, pos, 4, NH_IDR, 32);
    for (int s = 0; s < 12; s++)
        pos = append_nal(buf, pos, 4, NH_NONIDR, 16);
    CHECK(pos == size, "multislice buffer filled to its exact length");

    gsize n = count_nal_units(fmt, buf, size);
    CHECK(n == 15, "count_nal_units returns all 15 units (not capped at 10)");

    nal_unit_t *units = g_new(nal_unit_t, n ? n : 1);
    gsize c = parse_nal_units(fmt, units, n, buf, size);
    CHECK(c == 15, "parse_nal_units delivers all 15 units when sized to the count");
    CHECK(c >= 3 && units[0].type == UNIT_SPS, "unit 0 is SPS");
    CHECK(c >= 3 && units[1].type == UNIT_PPS, "unit 1 is PPS");
    CHECK(c >= 3 && units[2].type == UNIT_FRAME_IDR, "unit 2 is IDR");

    int all_nonidr = (c == 15);
    for (gsize i = 3; i < c; i++)
        if (units[i].type != UNIT_FRAME_NON_IDR) all_nonidr = 0;
    CHECK(all_nonidr, "units 3..14 are all non-IDR slices (none dropped past the cap)");

    /* The unit spans must tile the whole buffer with no gaps or overruns. */
    gsize sum = 0;
    for (gsize i = 0; i < c; i++) sum += units[i].len;
    CHECK(sum == size, "unit lengths tile the whole buffer exactly");
    CHECK(c > 0 && units[c - 1].ptr + units[c - 1].len == buf + size,
          "last unit reaches the end of the buffer");

    /* A caller-supplied small max still caps (the cap is now the caller's
       choice, not a hidden constant) - proving the count is the real fix. */
    nal_unit_t capped[10];
    gsize cc = parse_nal_units(fmt, capped, 10, buf, size);
    CHECK(cc == 10, "a small max caps at 10, yet count_nal_units reported 15");

    g_free(units);
    g_free(buf);
    return g_failures;
}

static int run_startcode(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* (a) 3-byte start codes only must parse just like 4-byte ones. */
    {
        gsize size = (3 + 1 + 8) + (3 + 1 + 4) + (3 + 1 + 32);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal(buf, pos, 3, NH_SPS, 8);
        pos = append_nal(buf, pos, 3, NH_PPS, 4);
        pos = append_nal(buf, pos, 3, NH_IDR, 32);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 3, "3-byte start codes: all 3 units found");
        nal_unit_t u[3];
        gsize c = parse_nal_units(fmt, u, 3, buf, size);
        CHECK(c == 3 && u[0].type == UNIT_SPS && u[1].type == UNIT_PPS &&
              u[2].type == UNIT_FRAME_IDR,
              "3-byte start codes: types parsed correctly");
        g_free(buf);
    }

    /* (b) A frame that does not begin at offset 0 (leading junk before the
       first start code) must be found, not dropped - this is the search=1 fix. */
    {
        gsize junk = 6;
        gsize size = junk + (4 + 1 + 8) + (4 + 1 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        for (gsize k = 0; k < junk; k++) buf[pos++] = 0xFF;  /* no 00 00 01 here */
        gsize first_sc = pos;
        pos = append_nal(buf, pos, 4, NH_SPS, 8);
        pos = append_nal(buf, pos, 4, NH_IDR, 16);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 2, "offset-shifted frame: units found despite leading junk");
        nal_unit_t u[2];
        gsize c = parse_nal_units(fmt, u, 2, buf, size);
        CHECK(c == 2, "offset-shifted frame: parsed, not dropped");
        CHECK(c == 2 && u[0].ptr == buf + first_sc,
              "first unit begins at the start code, not at offset 0");
        CHECK(c == 2 && u[0].type == UNIT_SPS && u[1].type == UNIT_FRAME_IDR,
              "offset-shifted frame: types parsed correctly");
        g_free(buf);
    }

    /* (c) A 3-byte start code sitting immediately after a 4-byte one must not be
       skipped (advance-by-start-code-length, not a fixed +5). */
    {
        gsize size = (4 + 1 + 8) + (3 + 1 + 4) + (4 + 1 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal(buf, pos, 4, NH_SPS, 8);
        pos = append_nal(buf, pos, 3, NH_PPS, 4);
        pos = append_nal(buf, pos, 4, NH_IDR, 16);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 3, "mixed 3+4-byte start codes: all 3 units found");
        nal_unit_t u[3];
        gsize c = parse_nal_units(fmt, u, 3, buf, size);
        CHECK(c == 3 && u[0].type == UNIT_SPS && u[1].type == UNIT_PPS &&
              u[2].type == UNIT_FRAME_IDR,
              "mixed start codes: types parsed correctly");
        g_free(buf);
    }

    /* (d) find_nal_unit reports the start-code length for both forms. */
    {
        unsigned char b4[8] = {0, 0, 0, 1, NH_IDR, 0xAB, 0xAB, 0xAB};
        unsigned char b3[8] = {0, 0, 1, NH_SPS, 0xAB, 0xAB, 0xAB, 0xAB};
        gsize off = 99, sc = 0;
        int t = find_nal_unit(fmt, b4, sizeof(b4), 0, 1, &off, &sc);
        CHECK(t == UNIT_FRAME_IDR && off == 0 && sc == 4,
              "find_nal_unit reports a 4-byte start code (sc_len == 4)");
        off = 99;
        sc = 0;
        t = find_nal_unit(fmt, b3, sizeof(b3), 0, 1, &off, &sc);
        CHECK(t == UNIT_SPS && off == 0 && sc == 3,
              "find_nal_unit reports a 3-byte start code (sc_len == 3)");
    }

    return g_failures;
}

static int run_bounds(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* (a) A dangling 4-byte start code at the very end (no header byte) must not
       trigger a read past the buffer while looking for the NAL header. */
    {
        gsize size = (4 + 1 + 3) + 4;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_IDR, 3);
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01;
        CHECK(pos == size, "bounds(a): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(a): dangling 4-byte start code is not a unit");
        nal_unit_t u[4];
        gsize c = parse_nal_units(fmt, u, 4, buf, size);
        CHECK(c == 1 && u[0].type == UNIT_FRAME_IDR,
              "bounds(a): the one complete unit is parsed");
        g_free(buf);
    }

    /* (b) A trailing partial start code (00 00) must not be over-read. */
    {
        gsize size = (4 + 1 + 4) + 2;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_IDR, 4);
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        CHECK(pos == size, "bounds(b): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(b): trailing partial start code is not a unit");
        g_free(buf);
    }

    /* (c) Buffers shorter than a minimal unit (and a zero-length buffer) must
       never index into the buffer at all. */
    {
        unsigned char *b = g_malloc(3);
        b[0] = 0x00; b[1] = 0x00; b[2] = 0x01;
        CHECK(count_nal_units(fmt, b, 3) == 0, "bounds(c): 3-byte buffer yields no unit");
        gsize off = 0, sc = 0;
        CHECK(find_nal_unit(fmt, b, 3, 0, 1, &off, &sc) == -1,
              "bounds(c): find_nal_unit on 3 bytes returns -1");
        g_free(b);

        unsigned char *z = g_malloc(1);
        CHECK(count_nal_units(fmt, z, 0) == 0, "bounds(c): zero-length buffer yields no unit");
        g_free(z);
    }

    /* (d) One oversized merged NAL (payload far larger than the SPS/PPS clamp
       limit). The parser must report the whole span as one unit without reading
       past the exact-size allocation; the frame_callback clamp (Task 5) is what
       later refuses to copy it, not the parser. */
    {
        gsize pay = 5000;             /* > SPSPPSBUFSZ (1024) */
        gsize size = 4 + 1 + pay;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_SPS, pay);
        CHECK(pos == size, "bounds(d): oversized buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(d): one oversized merged unit counted");
        nal_unit_t u[1];
        gsize c = parse_nal_units(fmt, u, 1, buf, size);
        CHECK(c == 1 && u[0].len == size,
              "bounds(d): oversized unit length spans the whole buffer");
        CHECK(c == 1 && u[0].len > SPSPPSBUFSZ,
              "bounds(d): oversized unit exceeds the SPS/PPS clamp limit");
        g_free(buf);
    }

    /* (e) A malformed separator (00 00 02, not a real start code) must not split
       the unit, and its bytes must not be over-read. */
    {
        gsize size = (4 + 1 + 6) + 5;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_IDR, 6);
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x02;
        buf[pos++] = 0xAB; buf[pos++] = 0xAB;
        CHECK(pos == size, "bounds(e): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(e): malformed separator does not split the unit");
        g_free(buf);
    }

    return g_failures;
}

/* The H.265 suites below mirror the H.264 ones one-for-one so the second codec
   reaches parity: same multi-slice count proof, same 3-/4-byte start-code and
   offset-shift coverage, same size_t bounds teeth under ASAN. The only shape
   difference is the extra VPS NAL that leads every H.265 access unit and the
   2-byte NAL header. */

static int run_multislice_h265(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H265;

    /* One access unit: VPS + SPS + PPS + IDR + 12 non-IDR slices = 16 NAL units.
       The leading VPS is the H.265-only parameter set; the IDR is IDR_W_RADL
       (19), the type real encoders emit, which the parser must classify as a
       keyframe just like H.264 type 5. */
    gsize size = 0;
    size += 4 + 2 + 8;            /* VPS */
    size += 4 + 2 + 8;            /* SPS */
    size += 4 + 2 + 4;            /* PPS */
    size += 4 + 2 + 32;           /* IDR */
    size += 12 * (4 + 2 + 16);    /* 12 non-IDR slices */
    unsigned char *buf = g_malloc(size);

    gsize pos = 0;
    pos = append_nal_h265(buf, pos, 4, H265_VPS, 8);
    pos = append_nal_h265(buf, pos, 4, H265_SPS, 8);
    pos = append_nal_h265(buf, pos, 4, H265_PPS, 4);
    pos = append_nal_h265(buf, pos, 4, H265_IDR_W_RADL, 32);
    for (int s = 0; s < 12; s++)
        pos = append_nal_h265(buf, pos, 4, H265_TRAIL_R, 16);
    CHECK(pos == size, "h265 multislice buffer filled to its exact length");

    gsize n = count_nal_units(fmt, buf, size);
    CHECK(n == 16, "h265 count_nal_units returns all 16 units (VPS+SPS+PPS+IDR+12)");

    nal_unit_t *units = g_new(nal_unit_t, n ? n : 1);
    gsize c = parse_nal_units(fmt, units, n, buf, size);
    CHECK(c == 16, "h265 parse_nal_units delivers all 16 units when sized to the count");
    CHECK(c >= 4 && units[0].type == UNIT_VPS, "h265 unit 0 is VPS");
    CHECK(c >= 4 && units[1].type == UNIT_SPS, "h265 unit 1 is SPS");
    CHECK(c >= 4 && units[2].type == UNIT_PPS, "h265 unit 2 is PPS");
    CHECK(c >= 4 && units[3].type == UNIT_FRAME_IDR, "h265 unit 3 is IDR (IDR_W_RADL)");

    int all_nonidr = (c == 16);
    for (gsize i = 4; i < c; i++)
        if (units[i].type != UNIT_FRAME_NON_IDR) all_nonidr = 0;
    CHECK(all_nonidr, "h265 units 4..15 are all non-IDR slices (none dropped)");

    /* The unit spans must tile the whole buffer with no gaps or overruns. */
    gsize sum = 0;
    for (gsize i = 0; i < c; i++) sum += units[i].len;
    CHECK(sum == size, "h265 unit lengths tile the whole buffer exactly");
    CHECK(c > 0 && units[c - 1].ptr + units[c - 1].len == buf + size,
          "h265 last unit reaches the end of the buffer");

    /* A caller-supplied small max still caps - proving the count is the real fix. */
    nal_unit_t capped[10];
    gsize cc = parse_nal_units(fmt, capped, 10, buf, size);
    CHECK(cc == 10, "h265 a small max caps at 10, yet count_nal_units reported 16");

    g_free(units);
    g_free(buf);
    return g_failures;
}

static int run_startcode_h265(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H265;

    /* (a) 3-byte start codes only must parse just like 4-byte ones. */
    {
        gsize size = (3 + 2 + 8) + (3 + 2 + 8) + (3 + 2 + 4) + (3 + 2 + 32);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal_h265(buf, pos, 3, H265_VPS, 8);
        pos = append_nal_h265(buf, pos, 3, H265_SPS, 8);
        pos = append_nal_h265(buf, pos, 3, H265_PPS, 4);
        pos = append_nal_h265(buf, pos, 3, H265_IDR_N_LP, 32);
        CHECK(pos == size, "h265 startcode(a): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 4, "h265 3-byte start codes: all 4 units found");
        nal_unit_t u[4];
        gsize c = parse_nal_units(fmt, u, 4, buf, size);
        CHECK(c == 4 && u[0].type == UNIT_VPS && u[1].type == UNIT_SPS &&
              u[2].type == UNIT_PPS && u[3].type == UNIT_FRAME_IDR,
              "h265 3-byte start codes: types parsed correctly");
        g_free(buf);
    }

    /* (b) A frame that does not begin at offset 0 (leading junk before the
       first start code) must be found, not dropped. */
    {
        gsize junk = 6;
        gsize size = junk + (4 + 2 + 8) + (4 + 2 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        for (gsize k = 0; k < junk; k++) buf[pos++] = 0xFF;  /* no 00 00 01 here */
        gsize first_sc = pos;
        pos = append_nal_h265(buf, pos, 4, H265_VPS, 8);
        pos = append_nal_h265(buf, pos, 4, H265_IDR_W_RADL, 16);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 2, "h265 offset-shifted frame: units found despite leading junk");
        nal_unit_t u[2];
        gsize c = parse_nal_units(fmt, u, 2, buf, size);
        CHECK(c == 2, "h265 offset-shifted frame: parsed, not dropped");
        CHECK(c == 2 && u[0].ptr == buf + first_sc,
              "h265 first unit begins at the start code, not at offset 0");
        CHECK(c == 2 && u[0].type == UNIT_VPS && u[1].type == UNIT_FRAME_IDR,
              "h265 offset-shifted frame: types parsed correctly");
        g_free(buf);
    }

    /* (c) A 3-byte start code sitting immediately after a 4-byte one must not be
       skipped (advance-by-start-code-length, not a fixed offset). */
    {
        gsize size = (4 + 2 + 8) + (3 + 2 + 4) + (4 + 2 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal_h265(buf, pos, 4, H265_SPS, 8);
        pos = append_nal_h265(buf, pos, 3, H265_PPS, 4);
        pos = append_nal_h265(buf, pos, 4, H265_IDR_W_RADL, 16);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 3, "h265 mixed 3+4-byte start codes: all 3 units found");
        nal_unit_t u[3];
        gsize c = parse_nal_units(fmt, u, 3, buf, size);
        CHECK(c == 3 && u[0].type == UNIT_SPS && u[1].type == UNIT_PPS &&
              u[2].type == UNIT_FRAME_IDR,
              "h265 mixed start codes: types parsed correctly");
        g_free(buf);
    }

    /* (d) find_nal_unit reports the start-code length for both forms, reading
       the H.265 two-byte header to derive the type. */
    {
        unsigned char b4[8] = {0, 0, 0, 1, H265_NH0(H265_IDR_W_RADL), H265_NH1, 0xAB, 0xAB};
        unsigned char b3[8] = {0, 0, 1, H265_NH0(H265_VPS), H265_NH1, 0xAB, 0xAB, 0xAB};
        gsize off = 99, sc = 0;
        int t = find_nal_unit(fmt, b4, sizeof(b4), 0, 1, &off, &sc);
        CHECK(t == UNIT_FRAME_IDR && off == 0 && sc == 4,
              "h265 find_nal_unit reports a 4-byte start code (sc_len == 4)");
        off = 99;
        sc = 0;
        t = find_nal_unit(fmt, b3, sizeof(b3), 0, 1, &off, &sc);
        CHECK(t == UNIT_VPS && off == 0 && sc == 3,
              "h265 find_nal_unit reports a 3-byte start code (sc_len == 3)");
    }

    /* (e) Both IDR NAL types (IDR_W_RADL 19 and IDR_N_LP 20) must classify as a
       keyframe. IDR_W_RADL is what real encoders emit; only mapping IDR_N_LP
       left those keyframes as UNIT_INVALID, so this is the parity assertion. */
    {
        unsigned char w[8] = {0, 0, 0, 1, H265_NH0(H265_IDR_W_RADL), H265_NH1, 0xAB, 0xAB};
        unsigned char l[8] = {0, 0, 0, 1, H265_NH0(H265_IDR_N_LP),   H265_NH1, 0xAB, 0xAB};
        gsize off = 0, sc = 0;
        int tw = find_nal_unit(fmt, w, sizeof(w), 0, 1, &off, &sc);
        CHECK(tw == UNIT_FRAME_IDR, "h265 IDR_W_RADL (19) classifies as IDR");
        off = 0;
        sc = 0;
        int tl = find_nal_unit(fmt, l, sizeof(l), 0, 1, &off, &sc);
        CHECK(tl == UNIT_FRAME_IDR, "h265 IDR_N_LP (20) classifies as IDR");
    }

    return g_failures;
}

static int run_bounds_h265(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H265;

    /* (a) A dangling 4-byte start code at the very end (no header byte) must not
       trigger a read past the buffer while looking for the NAL header. */
    {
        gsize size = (4 + 2 + 3) + 4;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal_h265(buf, 0, 4, H265_IDR_W_RADL, 3);
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01;
        CHECK(pos == size, "h265 bounds(a): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "h265 bounds(a): dangling 4-byte start code is not a unit");
        nal_unit_t u[4];
        gsize c = parse_nal_units(fmt, u, 4, buf, size);
        CHECK(c == 1 && u[0].type == UNIT_FRAME_IDR,
              "h265 bounds(a): the one complete unit is parsed");
        g_free(buf);
    }

    /* (b) A trailing partial start code (00 00) must not be over-read. */
    {
        gsize size = (4 + 2 + 4) + 2;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal_h265(buf, 0, 4, H265_IDR_W_RADL, 4);
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        CHECK(pos == size, "h265 bounds(b): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "h265 bounds(b): trailing partial start code is not a unit");
        g_free(buf);
    }

    /* (c) Buffers shorter than a minimal unit (and a zero-length buffer) must
       never index into the buffer at all. */
    {
        unsigned char *b = g_malloc(3);
        b[0] = 0x00; b[1] = 0x00; b[2] = 0x01;
        CHECK(count_nal_units(fmt, b, 3) == 0, "h265 bounds(c): 3-byte buffer yields no unit");
        gsize off = 0, sc = 0;
        CHECK(find_nal_unit(fmt, b, 3, 0, 1, &off, &sc) == -1,
              "h265 bounds(c): find_nal_unit on 3 bytes returns -1");
        g_free(b);

        unsigned char *z = g_malloc(1);
        CHECK(count_nal_units(fmt, z, 0) == 0, "h265 bounds(c): zero-length buffer yields no unit");
        g_free(z);
    }

    /* (d) One oversized merged NAL (payload far larger than the SPS/PPS clamp
       limit). The parser must report the whole span as one unit without reading
       past the exact-size allocation; the frame_callback clamp is what later
       refuses to copy it, not the parser. */
    {
        gsize pay = 5000;             /* > SPSPPSBUFSZ (1024) */
        gsize size = 4 + 2 + pay;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal_h265(buf, 0, 4, H265_VPS, pay);
        CHECK(pos == size, "h265 bounds(d): oversized buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "h265 bounds(d): one oversized merged unit counted");
        nal_unit_t u[1];
        gsize c = parse_nal_units(fmt, u, 1, buf, size);
        CHECK(c == 1 && u[0].len == size,
              "h265 bounds(d): oversized unit length spans the whole buffer");
        CHECK(c == 1 && u[0].len > SPSPPSBUFSZ,
              "h265 bounds(d): oversized unit exceeds the SPS/PPS clamp limit");
        g_free(buf);
    }

    /* (e) A malformed separator (00 00 02, not a real start code) must not split
       the unit, and its bytes must not be over-read. */
    {
        gsize size = (4 + 2 + 6) + 5;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal_h265(buf, 0, 4, H265_IDR_W_RADL, 6);
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x02;
        buf[pos++] = 0xAB; buf[pos++] = 0xAB;
        CHECK(pos == size, "h265 bounds(e): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "h265 bounds(e): malformed separator does not split the unit");
        g_free(buf);
    }

    return g_failures;
}

/* spspps_clamp_len() is the store-side bound that keeps store_spspps()'s fwrite
   from running off the fixed SPSPPSBUFSZ self->{vps,sps,pps} arrays when a corrupt
   or unclamped *_length reaches it. It is pure, so it is asserted directly; the
   ASAN teeth are the exact-size source buffer below - a broken clamp returning the
   raw oversized length would over-read it and abort the run. */
static int run_store_bounds(void) {
    CHECK(spspps_clamp_len(-1) == 0, "negative length floors to 0");
    CHECK(spspps_clamp_len(-100000) == 0, "large negative length floors to 0");
    CHECK(spspps_clamp_len(0) == 0, "zero length stays 0");
    CHECK(spspps_clamp_len(1) == 1, "minimal length passes through");
    CHECK(spspps_clamp_len(512) == 512, "in-range length passes through");
    CHECK(spspps_clamp_len(SPSPPSBUFSZ) == (gsize)SPSPPSBUFSZ,
          "exact-size length passes through");
    CHECK(spspps_clamp_len(SPSPPSBUFSZ + 1) == (gsize)SPSPPSBUFSZ,
          "one past the bound caps at SPSPPSBUFSZ");
    CHECK(spspps_clamp_len(SPSPPSBUFSZ + 4096) == (gsize)SPSPPSBUFSZ,
          "far-oversized length caps at SPSPPSBUFSZ");

    unsigned char *src = g_malloc(SPSPPSBUFSZ);
    memset(src, 0xAB, SPSPPSBUFSZ);
    gsize safe = spspps_clamp_len(SPSPPSBUFSZ + 4096);
    CHECK(safe == (gsize)SPSPPSBUFSZ, "clamped copy length stays within the buffer");
    /* Model store_spspps()'s fwrite(self->vps, 1, len): read `safe` bytes out of
       the exact-size source. A broken clamp returning the raw oversized length
       reads past src and trips ASAN (heap-buffer-overflow). */
    volatile unsigned long sink = 0;
    for (gsize i = 0; i < safe; i++) {
        sink += src[i];
    }
    (void) sink;
    g_free(src);
    return g_failures;
}

/* Drive the full parser (count -> parse) over `buf` for one codec and prove every
   emitted unit stays inside [buf, buf+size). Touching the first/last byte of each
   span gives ASAN teeth: an out-of-bounds unit hits the redzone right after the
   exact-size allocation and aborts, not merely the arithmetic check. */
static void drive_parser_bounds(enum uvc_frame_format fmt, unsigned char *buf,
                                gsize size, const char *label) {
    gsize n = count_nal_units(fmt, buf, size);
    nal_unit_t *units = g_new(nal_unit_t, n ? n : 1);
    gsize c = parse_nal_units(fmt, units, n, buf, size);

    int ok = (c <= n);
    for (gsize i = 0; i < c; i++) {
        if (units[i].ptr < buf) ok = 0;
        if (units[i].len > size) ok = 0;
        if (units[i].ptr + units[i].len > buf + size) ok = 0;
        if (units[i].len > 0) {
            volatile unsigned char first = units[i].ptr[0];
            volatile unsigned char last = units[i].ptr[units[i].len - 1];
            (void)first;
            (void)last;
        }
    }
    CHECK(ok, label);
    g_free(units);
}

/* Deterministic in-test mirror of tests/corpus/nal/: the same five adversarial
   byte patterns the libFuzzer harness (fuzz_nal.c) seeds with, each driven through
   the parser for BOTH codecs. Buffers are the exact used length (g_malloc) so the
   ASAN redzone sits right after the last byte and any over-read aborts. */
static int run_fuzz_seed(void) {
    /* (1) valid H.264 SPS+PPS+IDR */
    {
        gsize size = (4 + 1 + 8) + (4 + 1 + 4) + (4 + 1 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal(buf, pos, 4, NH_SPS, 8);
        pos = append_nal(buf, pos, 4, NH_PPS, 4);
        pos = append_nal(buf, pos, 4, NH_IDR, 16);
        CHECK(pos == size, "seed(1) valid H.264 buffer filled exactly");
        drive_parser_bounds(UVC_FRAME_FORMAT_H264, buf, size, "seed(1) valid H.264 in bounds (H264)");
        drive_parser_bounds(UVC_FRAME_FORMAT_H265, buf, size, "seed(1) valid H.264 in bounds (H265 view)");
        g_free(buf);
    }
    /* (2) valid H.265 VPS+SPS+PPS+IDR */
    {
        gsize size = (4 + 2 + 8) + (4 + 2 + 8) + (4 + 2 + 4) + (4 + 2 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal_h265(buf, pos, 4, H265_VPS, 8);
        pos = append_nal_h265(buf, pos, 4, H265_SPS, 8);
        pos = append_nal_h265(buf, pos, 4, H265_PPS, 4);
        pos = append_nal_h265(buf, pos, 4, H265_IDR_W_RADL, 16);
        CHECK(pos == size, "seed(2) valid H.265 buffer filled exactly");
        drive_parser_bounds(UVC_FRAME_FORMAT_H265, buf, size, "seed(2) valid H.265 in bounds (H265)");
        drive_parser_bounds(UVC_FRAME_FORMAT_H264, buf, size, "seed(2) valid H.265 in bounds (H264 view)");
        g_free(buf);
    }
    /* (3) truncated start code: 00 00 01 with no header byte */
    {
        gsize size = 3;
        unsigned char *buf = g_malloc(size);
        buf[0] = 0x00;
        buf[1] = 0x00;
        buf[2] = 0x01;
        drive_parser_bounds(UVC_FRAME_FORMAT_H264, buf, size, "seed(3) truncated start code in bounds (H264)");
        drive_parser_bounds(UVC_FRAME_FORMAT_H265, buf, size, "seed(3) truncated start code in bounds (H265)");
        g_free(buf);
    }
    /* (4) oversized single NAL: payload far larger than the SPS/PPS clamp limit */
    {
        gsize pay = 5000;
        gsize size = 4 + 1 + pay;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_SPS, pay);
        CHECK(pos == size, "seed(4) oversized buffer filled exactly");
        drive_parser_bounds(UVC_FRAME_FORMAT_H264, buf, size, "seed(4) oversized NAL in bounds (H264)");
        drive_parser_bounds(UVC_FRAME_FORMAT_H265, buf, size, "seed(4) oversized NAL in bounds (H265)");
        g_free(buf);
    }
    /* (5) empty input: a 0-length view must index into nothing */
    {
        unsigned char *buf = g_malloc(1);
        drive_parser_bounds(UVC_FRAME_FORMAT_H264, buf, 0, "seed(5) empty input in bounds (H264)");
        drive_parser_bounds(UVC_FRAME_FORMAT_H265, buf, 0, "seed(5) empty input in bounds (H265)");
        g_free(buf);
    }
    return g_failures;
}

/* Counter raised by overflow_log_fn whenever the parser's truncation GST_WARNING
   reaches the debug system. Asserting on it - not merely on "no crash" - is what
   proves the warning is actually emitted when the NAL count exceeds max. */
static int g_overflow_warnings;

static void overflow_log_fn(GstDebugCategory *category, GstDebugLevel level,
                            const gchar *file, const gchar *function, gint line,
                            GObject *object, GstDebugMessage *message,
                            gpointer user_data) {
    (void)category; (void)file; (void)function; (void)line;
    (void)object; (void)user_data;
    if (level == GST_LEVEL_WARNING) {
        const gchar *text = gst_debug_message_get(message);
        if (text && strstr(text, "exceeds max") != NULL) g_overflow_warnings++;
    }
}

static int run_nal_overflow(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* SPS + PPS + IDR + 2 non-IDR slices = 5 NAL units. */
    gsize size = (4 + 1 + 8) + (4 + 1 + 4) + (4 + 1 + 16) + 2 * (4 + 1 + 8);
    unsigned char *buf = g_malloc(size);
    gsize pos = 0;
    pos = append_nal(buf, pos, 4, NH_SPS, 8);
    pos = append_nal(buf, pos, 4, NH_PPS, 4);
    pos = append_nal(buf, pos, 4, NH_IDR, 16);
    pos = append_nal(buf, pos, 4, NH_NONIDR, 8);
    pos = append_nal(buf, pos, 4, NH_NONIDR, 8);
    CHECK(pos == size, "overflow buffer filled to its exact length");
    CHECK(count_nal_units(fmt, buf, size) == 5, "count_nal_units sees all 5 units");

    /* Raise the category threshold (default NONE keeps the warning suppressed)
       and route it into our counter. */
    gst_debug_category_set_threshold(gst_libuvc_h264_src_debug, GST_LEVEL_WARNING);
    gst_debug_add_log_function(overflow_log_fn, NULL, NULL);

    g_overflow_warnings = 0;
    nal_unit_t small_units[3];
    gsize c = parse_nal_units(fmt, small_units, 3, buf, size);
    CHECK(c == 3, "parse_nal_units returns exactly max when truncating");
    CHECK(g_overflow_warnings >= 1, "truncation warning emitted when count > max");

    g_overflow_warnings = 0;
    nal_unit_t exact_units[5];
    gsize c2 = parse_nal_units(fmt, exact_units, 5, buf, size);
    CHECK(c2 == 5, "parse_nal_units returns every unit when max == count");
    CHECK(g_overflow_warnings == 0, "no warning when max == count");

    g_overflow_warnings = 0;
    nal_unit_t big_units[8];
    gsize c3 = parse_nal_units(fmt, big_units, 8, buf, size);
    CHECK(c3 == 5, "parse_nal_units returns every unit when max > count");
    CHECK(g_overflow_warnings == 0, "no warning when max > count");

    gst_debug_remove_log_function(overflow_log_fn);
    g_free(buf);
    return g_failures;
}

/* count_nal_units is the sizing primitive every caller relies on: it must equal
   the real unit number and bound parse_nal_units so a sized-to-count array gets
   every unit and no oversized `max` ever makes parse exceed the real count. */
static int run_nal_count_bound(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    gsize size = (4 + 1 + 8) + (4 + 1 + 4) + (4 + 1 + 16);
    for (int s = 0; s < 17; s++) size += (4 + 1 + 8);
    unsigned char *buf = g_malloc(size);
    gsize pos = 0;
    pos = append_nal(buf, pos, 4, NH_SPS, 8);
    pos = append_nal(buf, pos, 4, NH_PPS, 4);
    pos = append_nal(buf, pos, 4, NH_IDR, 16);
    for (int s = 0; s < 17; s++) pos = append_nal(buf, pos, 4, NH_NONIDR, 8);
    CHECK(pos == size, "count-bound buffer filled to its exact length");

    gsize total = count_nal_units(fmt, buf, size);
    CHECK(total == 20, "count_nal_units returns the exact unit count (20)");

    nal_unit_t *units = g_new(nal_unit_t, total);
    gsize c = parse_nal_units(fmt, units, total, buf, size);
    CHECK(c == total, "parse_nal_units sized to count returns exactly count");

    gsize over = parse_nal_units(fmt, units, total + 50, buf, size);
    CHECK(over == total, "parse never exceeds the real count even with a larger max");

    gsize half = parse_nal_units(fmt, units, total / 2, buf, size);
    CHECK(half == total / 2, "a smaller max bounds the result below count");
    CHECK(half <= total, "the bounded result never exceeds count");

    CHECK(count_nal_units(fmt, buf, 0) == 0, "zero-length buffer counts 0");

    g_free(units);
    g_free(buf);
    return g_failures;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    GST_DEBUG_CATEGORY_INIT(gst_libuvc_h264_src_debug, "libuvch264src", 0,
                            "libuvch264src NAL parser test");
    if (argc < 2) {
        fprintf(stderr, "usage: %s <multislice|startcode|bounds|"
                        "multislice_h265|startcode_h265|bounds_h265|"
                        "store_bounds|fuzz_seed|overflow>\n", argv[0]);
        return 2;
    }

    int failures;
    if (strcmp(argv[1], "multislice") == 0) {
        printf("nal_parse multislice:\n");
        failures = run_multislice();
    } else if (strcmp(argv[1], "startcode") == 0) {
        printf("nal_parse startcode:\n");
        failures = run_startcode();
    } else if (strcmp(argv[1], "bounds") == 0) {
        printf("nal_parse bounds:\n");
        failures = run_bounds();
    } else if (strcmp(argv[1], "multislice_h265") == 0) {
        printf("nal_parse multislice_h265:\n");
        failures = run_multislice_h265();
    } else if (strcmp(argv[1], "startcode_h265") == 0) {
        printf("nal_parse startcode_h265:\n");
        failures = run_startcode_h265();
    } else if (strcmp(argv[1], "bounds_h265") == 0) {
        printf("nal_parse bounds_h265:\n");
        failures = run_bounds_h265();
    } else if (strcmp(argv[1], "store_bounds") == 0) {
        printf("nal_parse store_bounds:\n");
        failures = run_store_bounds();
    } else if (strcmp(argv[1], "fuzz_seed") == 0) {
        printf("nal_parse fuzz_seed:\n");
        failures = run_fuzz_seed();
    } else if (strcmp(argv[1], "overflow") == 0) {
        printf("nal_parse overflow:\n");
        failures = run_nal_overflow();
    } else if (strcmp(argv[1], "count_bound") == 0) {
        printf("nal_parse count_bound:\n");
        failures = run_nal_count_bound();
    } else {
        fprintf(stderr, "unknown suite: %s\n", argv[1]);
        return 2;
    }

    printf("%s: %d failure(s)\n", argv[1], failures);
    return failures == 0 ? 0 : 1;
}
