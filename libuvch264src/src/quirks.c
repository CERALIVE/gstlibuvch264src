#include "quirks.h"
#include "gstlibuvch264src_internal.h" // IWYU pragma: keep

static const uvc_quirk_entry_t *quirks_find_row(const uvc_quirk_entry_t *table,
                                                gsize n, guint16 vid,
                                                guint16 pid) {
    if (table == NULL) {
        return NULL;
    }
    for (gsize i = 0; i < n; i++) {
        if (table[i].vid == vid && table[i].pid == pid) {
            return &table[i];
        }
    }
    return NULL;
}

guint32 quirks_lookup_in(const uvc_quirk_entry_t *table, gsize n,
                         guint16 vid, guint16 pid) {
    const uvc_quirk_entry_t *row = quirks_find_row(table, n, vid, pid);
    return row != NULL ? row->flags : 0;
}

void quirks_limits_in(const uvc_quirk_entry_t *table, gsize n,
                      guint16 vid, guint16 pid, uvc_quirk_limits_t *out) {
    g_return_if_fail(out != NULL);

    out->flags = 0;
    out->max_pixel_rate = 0;

    const uvc_quirk_entry_t *row = quirks_find_row(table, n, vid, pid);
    if (row == NULL) {
        return;
    }

    out->flags = row->flags;
    /* Resolving the flag HERE is what lets every caller test max_pixel_rate
     * alone; a row that carries a rate without the flag imposes no cap. */
    if (row->flags & QUIRK_MAX_PIXEL_RATE) {
        out->max_pixel_rate = row->max_pixel_rate;
    }
}

gboolean uvc_quirk_mode_selectable(const uvc_quirk_limits_t *limits,
                                   guint width, guint height, guint fps) {
    if (limits == NULL || limits->max_pixel_rate == 0) {
        return TRUE;
    }
    /* 64-bit product: 4K@60 alone is 497 664 000, and a future 8K row would
     * overflow 32 bits outright. */
    guint64 rate = (guint64) width * (guint64) height * (guint64) fps;
    return rate <= limits->max_pixel_rate;
}

guint uvc_quirk_max_fps(const uvc_quirk_limits_t *limits,
                        guint width, guint height) {
    if (limits == NULL || limits->max_pixel_rate == 0) {
        return G_MAXUINT;
    }
    guint64 pixels = (guint64) width * (guint64) height;
    if (pixels == 0) {
        return G_MAXUINT;
    }
    guint64 max_fps = limits->max_pixel_rate / pixels;
    return max_fps > G_MAXUINT ? G_MAXUINT : (guint) max_fps;
}

/* Production quirk table. One row per device that needs a device-specific limit,
 * each citing the device and the evidence. Probe retry policy is universal and
 * lives in negotiate(); this table only constrains modes for matched devices.
 *
 * ---------------------------------------------------------------------------
 * DJI Osmo Pocket 3 (2ca3:0023) - advertises H.264 modes it cannot deliver.
 *
 * Its H.264 descriptor offers 3840x2160 at 60/50/48/30/25/24 fps. The 60/50/48
 * rates are PHANTOM for the shipping path: advertised, but not a safe deliverable
 * ladder. The descriptor really does carry dwFrameInterval
 * 166666/200000/208333 - verified byte-for-byte from a raw USB descriptor dump
 * on both the USB-A and USB-C ports, and independently by the kernel uvcvideo
 * parser. In the original 2026-07-30 failure, permissive caps selected 4K@60 BY
 * CONSTRUCTION and no frame arrived before the silence watchdog ended the stream.
 * The camera appears to advertise its RECORDING capability here; corroborating
 * that, its MJPEG descriptor for the same geometry lists only 30/25/24.
 *
 * The cap is 3840x2160x30 = 248 832 000 px/s, and it was first CONFIRMED on
 * hardware rather than inferred from the descriptor. Board 192.168.78.131,
 * 2026-07-30, plugin
 * .so deployed alone so the board's libuvc stayed at 4868e57 and this cap was
 * the only variable; two independent runs of
 * `num-buffers=300 ! video/x-h264,width=3840,height=2160,framerate=30/1`:
 *
 *   run 1  clean EOS, exit 0, 10.71 s, h264parse read 3840x2160 / high / 5.2
 *   run 2  exactly 300/300 access units counted at an identity probe, 10.79 s,
 *          same SPS-derived 3840x2160 / high / level 5.2 / 4:2:0
 *
 * Zero errors, zero RESOURCE/READ, no silence-watchdog disconnect on either
 * run. That is exactly the bar this table demands and nothing less: frames
 * ADVANCED for the full ~10.7 s window, at a resolution read out of the SPS
 * rather than the requested caps echoed back - a successful
 * uvc_start_streaming() proves nothing on this device.
 *
 * The value is 4K@30 EXACTLY, not the full 4K@60 descriptor range, and that is
 * what makes those runs conclusive rather than suggestive: negotiate() prefers
 * max area then max fps, so at 248 832 000 the surviving top mode is
 * 3840x2160@30 BY CONSTRUCTION and the capture cannot have silently measured
 * 4K@60 instead. Both runs logged `max pixel rate 248832000` and `quirk:
 * dropped 3 non-deliverable rate(s) at 3840x2160`, confirming 60/50/48 were
 * still excluded while the measured rate streamed.
 *
 * A same-day UNCAPPED run did deliver 600 access units at ~56 fps
 * (camera-compat.md S2 Step 5), so that historical evidence alone could not
 * authorize either deleting or raising the cap. The 2026-08-27 Rule C campaign
 * therefore re-tested the advertised 4K ladder under the recorded G1 retry
 * policy. Shipping-candidate after-smaller runs failed 45/45 at EACH of
 * 4K@60, 4K@50, and 4K@48 with `Unable to negotiate common caps` /
 * `not-negotiated`, zero access units, and no INVALID_MODE signal; the 4K@30
 * control passed 5/5. The harness recorded C2 at 248 832 000 as the highest
 * contiguous safe ceiling. This row is consequently a fresh drill result, not
 * merely the retained historical assumption.
 *
 * A scalar cap admits every advertised rate at or below it. To raise this ONE
 * number, a future campaign must therefore prove the candidate and every lower
 * advertised 4K mode with advancing frames, bounded access-unit counts, and
 * SPS-verified geometry; it may not skip over a failed lower mode. The literal
 * in tests/test_quirks.c is a deliberate tripwire and has to move with the row.
 *
 * Only pid 0023 needs a row. The camera also enumerates as 2ca3:0020, but that
 * is its RNDIS + mass-storage "connect to computer" mode with no UVC interface
 * at all, so it never reaches negotiation.
 *
 * The pixel-rate cap is separate from the stale-readback defect. libuvc's
 * uvc_probe_stream_ctrl() SET_CURs the requested control, GET_CURs it back, and
 * rejects a disagreement with UVC_ERROR_INVALID_MODE. The Osmo can answer the
 * first GET_CUR from its PREVIOUSLY committed mode, so a larger-mode transition
 * needs a second attempt.
 *
 * Measured on hardware (192.168.78.131, 2026-07-30), one probe vs two, same
 * binary otherwise:
 *
 *   1280x720@30  -> 1920x1080@30   1 probe: 3/3 FAIL    2 probes: 3/3 pass
 *   1920x1080@30 -> 3840x2160@60   1 probe: 20/20 FAIL  2 probes: 4/4 pass
 *   same mode again, or SMALLER    1 probe: pass        (readback already agrees)
 *
 * The failure is deterministic and direction-specific, not flaky: 23/23 on a
 * mode increase, 0 otherwise. The later Rule G drill tested the bounded
 * INVALID_MODE retry across every transition class and recorded G1 with zero
 * failures, so negotiate() now applies that retry to every device. No row flag
 * is needed; successful first probes still stop after one attempt.
 * --------------------------------------------------------------------------- */
static const uvc_quirk_entry_t g_uvc_quirk_table[] = {
    { 0x2ca3, 0x0023, QUIRK_MAX_PIXEL_RATE, 248832000u },
};

#ifdef LIBUVCH264SRC_TESTING
/* Test override (A14). NULL restores the production table above. Compiled only
 * into the test targets that define LIBUVCH264SRC_TESTING. */
static const uvc_quirk_entry_t *g_quirk_test_table = NULL;
static gsize g_quirk_test_table_len = 0;

void uvc_quirks_set_test_table(const uvc_quirk_entry_t *table, gsize n) {
    g_quirk_test_table = table;
    g_quirk_test_table_len = n;
}
#endif

static const uvc_quirk_entry_t *quirks_active_table(gsize *n) {
#ifdef LIBUVCH264SRC_TESTING
    if (g_quirk_test_table != NULL) {
        *n = g_quirk_test_table_len;
        return g_quirk_test_table;
    }
#endif
    *n = G_N_ELEMENTS(g_uvc_quirk_table);
    return g_uvc_quirk_table;
}

void uvc_quirks_limits(guint16 vid, guint16 pid, uvc_quirk_limits_t *out) {
    g_return_if_fail(out != NULL);

    gsize n = 0;
    const uvc_quirk_entry_t *table = quirks_active_table(&n);

    quirks_limits_in(table, n, vid, pid, out);
    if (out->flags != 0) {
        GST_INFO("UVC quirk match for %04x:%04x -> flags 0x%08x, "
                 "max pixel rate %" G_GUINT64_FORMAT,
                 vid, pid, out->flags, out->max_pixel_rate);
    }
}

guint32 uvc_quirks_lookup(guint16 vid, guint16 pid) {
    uvc_quirk_limits_t limits = {0};
    uvc_quirks_limits(vid, pid, &limits);
    return limits.flags;
}

/* The integer rate a caps fraction represents, rounded UP. A non-integral rate
 * (30000/1001) is judged by the rate it can peak at rather than by a truncation
 * that would let it slip under the cap. */
static guint quirks_fraction_fps(const GValue *fraction) {
    gint num = gst_value_get_fraction_numerator(fraction);
    gint den = gst_value_get_fraction_denominator(fraction);

    if (num <= 0 || den <= 0) {
        return 0;
    }
    return (guint)(((gint64)num + den - 1) / den);
}

/* Filter ONE mode's framerate field in place. FALSE means the mode has no
 * deliverable rate left and the caller must drop the structure. */
static gboolean quirks_filter_structure(const uvc_quirk_limits_t *limits,
                                        GstStructure *structure) {
    gint width = 0, height = 0;
    if (!gst_structure_get_int(structure, "width", &width)
        || !gst_structure_get_int(structure, "height", &height)) {
        /* No geometry means no pixel rate to judge it by, so it passes through
         * untouched rather than being guessed at. */
        return TRUE;
    }

    const GValue *rates = gst_structure_get_value(structure, "framerate");
    if (rates == NULL) {
        return TRUE;
    }

    if (GST_VALUE_HOLDS_FRACTION_RANGE(rates)) {
        /* A continuous-frame-interval descriptor advertises a whole RANGE, so
         * the cap clamps its top instead of removing entries. */
        guint cap = uvc_quirk_max_fps(limits, (guint)width, (guint)height);
        const GValue *min = gst_value_get_fraction_range_min(rates);
        const GValue *max = gst_value_get_fraction_range_max(rates);

        if (cap == G_MAXUINT || quirks_fraction_fps(max) <= cap) {
            return TRUE;
        }

        gint min_num = gst_value_get_fraction_numerator(min);
        gint min_den = gst_value_get_fraction_denominator(min);
        if (min_den <= 0 || quirks_fraction_fps(min) > cap) {
            GST_INFO("quirk: %dx%d dropped, its whole interval range exceeds "
                     "the cap", width, height);
            return FALSE;
        }

        gst_structure_set(structure, "framerate", GST_TYPE_FRACTION_RANGE,
                          min_num, min_den, (gint)cap, 1, NULL);
        return TRUE;
    }

    if (GST_VALUE_HOLDS_FRACTION(rates)) {
        return uvc_quirk_mode_selectable(limits, (guint)width, (guint)height,
                                         quirks_fraction_fps(rates));
    }

    if (!GST_VALUE_HOLDS_LIST(rates)) {
        return TRUE;
    }

    GValue kept = G_VALUE_INIT;
    g_value_init(&kept, GST_TYPE_LIST);

    guint excluded = 0;
    for (guint i = 0; i < gst_value_list_get_size(rates); i++) {
        const GValue *rate = gst_value_list_get_value(rates, i);

        if (!uvc_quirk_mode_selectable(limits, (guint)width, (guint)height,
                                       quirks_fraction_fps(rate))) {
            excluded++;
            continue;
        }
        gst_value_list_append_value(&kept, rate);
    }

    if (excluded > 0) {
        GST_INFO("quirk: dropped %u non-deliverable rate(s) at %dx%d",
                 excluded, width, height);
    }

    if (gst_value_list_get_size(&kept) == 0) {
        /* Every advertised rate is above the cap, so the whole mode is
         * unusable; an empty framerate list would otherwise fixate to nothing. */
        g_value_unset(&kept);
        return FALSE;
    }

    gst_structure_set_value(structure, "framerate", &kept);
    g_value_unset(&kept);
    return TRUE;
}

GstCaps *uvc_quirks_filter_caps(const uvc_quirk_limits_t *limits,
                                const GstCaps *advertised) {
    g_return_val_if_fail(advertised != NULL, NULL);

    GstCaps *deliverable = gst_caps_copy(advertised);

    if (limits == NULL || limits->max_pixel_rate == 0) {
        /* No cap is armed, so every advertised mode is deliverable. Every camera
         * without a quirk row must keep getting its ladder back unchanged. */
        return deliverable;
    }

    guint i = 0;
    while (i < gst_caps_get_size(deliverable)) {
        if (quirks_filter_structure(limits,
                                    gst_caps_get_structure(deliverable, i))) {
            i++;
        } else {
            gst_caps_remove_structure(deliverable, i);
        }
    }
    return deliverable;
}
