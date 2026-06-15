/*
 * libFuzzer harness for the Annex-B NAL parser in frame_pipeline.c.
 *
 * Builds ONLY under -DENABLE_FUZZERS=ON (clang -fsanitize=fuzzer,address); see
 * tests/CMakeLists.txt. libFuzzer's -fsanitize=fuzzer conflicts with the ctest
 * suite's -fsanitize=thread, so this target is ASan-only and gated behind its
 * own option.
 *
 * Each input is fed to the parser entry points (count_nal_units ->
 * parse_nal_units, plus a manual find_nal_unit walk) for BOTH codecs, so one
 * corpus file exercises the H.264 and H.265 paths. The harness proves no input -
 * truncated, oversized, garbage, empty - makes the parser read past the frame
 * buffer; ASan is the oracle, and the exact-size input copy is its teeth.
 *
 * frame_pipeline.c pulls in frame_callback(), whose only element-internal
 * references (the debug category and the SPS/PPS store) are stubbed here exactly
 * as tests/test_nal_parse.c does, so the parser TU links standalone without the
 * GObject element or the SPS/PPS cache TU.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <libuvc/libuvc.h>

#include "frame_pipeline.h"
#include "spspps_cache.h"

GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);
void spspps_key_snapshot(GstLibuvcH264Src *self, spspps_key_t *key) { (void) self; (void) key; }
void store_spspps(GstLibuvcH264Src *self, const spspps_key_t *key) { (void) self; (void) key; }

static void run_one_format(enum uvc_frame_format fmt, unsigned char *buf, gsize len)
{
    gsize n = count_nal_units(fmt, buf, len);
    nal_unit_t *units = g_new(nal_unit_t, n ? n : 1);
    gsize c = parse_nal_units(fmt, units, n, buf, len);
    for (gsize i = 0; i < c; i++) {
        if (units[i].len > 0 && units[i].ptr != NULL) {
            volatile unsigned char a = units[i].ptr[0];
            volatile unsigned char b = units[i].ptr[units[i].len - 1];
            (void) a;
            (void) b;
        }
    }
    g_free(units);

    gsize off = 0, sc = 0;
    int t = find_nal_unit(fmt, buf, len, 0, 1, &off, &sc);
    while (t >= 0) {
        gsize next = off + sc;
        if (next <= off) break;
        t = find_nal_unit(fmt, buf, len, next, 1, &off, &sc);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    unsigned char *buf = malloc(size ? size : 1);
    if (buf == NULL) return 0;
    if (size) memcpy(buf, data, size);

    run_one_format(UVC_FRAME_FORMAT_H264, buf, (gsize) size);
    run_one_format(UVC_FRAME_FORMAT_H265, buf, (gsize) size);

    free(buf);
    return 0;
}
