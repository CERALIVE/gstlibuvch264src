#include <string.h>
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"

nal_unit_type_t convert_unit_type(enum uvc_frame_format format, int type) {
    if (format == UVC_FRAME_FORMAT_H264) {
        switch (type) {
            case 1:
                return UNIT_FRAME_NON_IDR;
            case 5:
                return UNIT_FRAME_IDR;
            case 7:
                return UNIT_SPS;
            case 8:
                return UNIT_PPS;
            case 9:
                return UNIT_AUD;
      }

    } else if (format == UVC_FRAME_FORMAT_H265) {
        switch (type) {
            case 1:
                return UNIT_FRAME_NON_IDR;
            /* Both IDR NAL types are keyframes (ITU-T H.265 Table 7-1).
               IDR_W_RADL (19) is the type x265 and most hardware encoders emit;
               mapping only IDR_N_LP (20) left those keyframes as UNIT_INVALID, so
               the IDR gate never armed and SPS/PPS/VPS were never prepended. */
            case 19:  /* IDR_W_RADL */
            case 20:  /* IDR_N_LP */
                return UNIT_FRAME_IDR;
            case 32:
                return UNIT_VPS;
            case 33:
                return UNIT_SPS;
            case 34:
                return UNIT_PPS;
            case 35:  /* AUD_NUT */
                return UNIT_AUD;
        }
    }

    return UNIT_INVALID;
}

/* Locate the next Annex-B NAL unit at or after `start`.
 *
 * Detects BOTH the 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes. The
 * 3-byte form is legal Annex-B and is emitted by real DJI/UVC cameras; missing
 * it merges two slices into one oversized NAL (L3). With search != 0 the scan
 * walks forward to the first start code anywhere in the buffer, so a frame that
 * does not begin exactly at offset 0 is found rather than dropped.
 *
 * On success returns the NAL type, sets *offset to the first byte of the start
 * code and *sc_len to its length (3 or 4). Lengths are gsize so a frame larger
 * than INT_MAX cannot wrap to a negative length and be skipped (L4). */
int find_nal_unit(enum uvc_frame_format format,
                  unsigned char *buf, gsize buflen, gsize start, int search,
                  gsize *offset, gsize *sc_len) {
    if (format != UVC_FRAME_FORMAT_H264 && format != UVC_FRAME_FORMAT_H265) return -1;
    if (buf == NULL) return -1;
    /* A unit needs at least a 3-byte start code plus one NAL header byte. */
    if (buflen < 4 || start > buflen - 4) return -1;

    for (gsize i = start; i <= buflen - 4; i++) {
        gsize hdr;
        gsize code_len;

        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
            if (i + 4 >= buflen) break;   /* 4-byte start code with no header byte */
            hdr = i + 4;
            code_len = 4;
        } else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            if (i + 3 >= buflen) break;   /* 3-byte start code with no header byte */
            hdr = i + 3;
            code_len = 3;
        } else {
            if (!search) break;
            continue;
        }

        if (offset) *offset = i;
        if (sc_len) *sc_len = code_len;
        if (format == UVC_FRAME_FORMAT_H264) {
            return convert_unit_type(format, buf[hdr] & 0x1F);
        }
        return convert_unit_type(format, (buf[hdr] >> 1) & 0x3F);
    }

    return -1;
}

/* Count the NAL units in `buf` so the caller can size an exact allocation and
 * never drop slices past a fixed cap (L2). */
gsize count_nal_units(enum uvc_frame_format format,
                      unsigned char *buf, gsize buflen) {
    gsize count = 0;
    gsize nal_offset = 0;
    gsize sc_len = 0;
    int next_type = find_nal_unit(format, buf, buflen, 0, 1, &nal_offset, &sc_len);
    while (next_type >= 0) {
        count++;
        next_type = find_nal_unit(format, buf, buflen, nal_offset + sc_len, 1,
                                  &nal_offset, &sc_len);
    }
    return count;
}

gsize parse_nal_units(enum uvc_frame_format format,
                      nal_unit_t *units, gsize max, unsigned char *buf, gsize buflen) {
    gsize i = 0;
    gsize nal_offset = 0;
    gsize sc_len = 0;

    /* First scan searches (search=1) so an offset-shifted frame is still found
       rather than dropped when it does not start exactly at offset 0 (L3). */
    int next_type = find_nal_unit(format, buf, buflen, 0, 1, &nal_offset, &sc_len);
    while (next_type >= 0 && i < max) {
        int type = next_type;
        gsize start = nal_offset;

        /* Advance past THIS start code only, so a 3-byte code sitting right
           after a 4-byte one is not skipped. */
        next_type = find_nal_unit(format, buf, buflen, nal_offset + sc_len, 1,
                                  &nal_offset, &sc_len);
        gsize end = (next_type >= 0) ? nal_offset : buflen;

        units[i].type = type;
        units[i].len = end - start;
        units[i].ptr = &buf[start];

        i++;
    }

    /* next_type >= 0 here can only mean the loop stopped on i == max with a NAL
       still pending: the array was under-sized and the tail is dropped. Log it
       rather than truncate silently (diagnostic only; return value unchanged). */
    if (next_type >= 0) {
        GST_WARNING("NAL unit count exceeds max=%" G_GSIZE_FORMAT "; stored %"
                    G_GSIZE_FORMAT " unit(s) and dropped the remainder. "
                    "Size the array with count_nal_units().", max, i);
    }

    return i;
}

static gboolean nal_is_parameter_set(nal_unit_type_t type) {
    return type == UNIT_VPS || type == UNIT_SPS || type == UNIT_PPS;
}

static gboolean nal_is_slice(nal_unit_type_t type) {
    return type == UNIT_FRAME_IDR || type == UNIT_FRAME_NON_IDR;
}

/* Offset of the NAL header byte inside a parsed unit. parse_nal_units() always
 * puts unit->ptr on the first byte of the Annex-B start code, so the header sits
 * just past it: 3 bytes in for 00 00 01, 4 for 00 00 00 01. */
static gsize nal_header_offset(const nal_unit_t *unit) {
    return (unit->len >= 3 && unit->ptr[2] == 0x01) ? 3 : 4;
}

/* TRUE when this slice NAL opens a new picture - the AU boundary signal used
 * when the device emits no Access Unit Delimiter.
 *
 * H.264 slice headers start with first_mb_in_slice, H.265 slice segment headers
 * with first_slice_segment_in_pic_flag; either way the "this is the first slice"
 * condition is carried by the very first bit of the slice payload. An exp-Golomb
 * ue(v) of 0 is coded as the single bit `1`, and the H.265 flag is a raw u(1), so
 * both reduce to "the top bit of the first payload byte is set" - no bit reader
 * needed. Emulation prevention cannot disturb that byte either: a 0x03 is only
 * inserted after two 0x00 bytes, and the NAL header immediately preceding it is
 * non-zero for every slice NAL. */
static gboolean nal_opens_new_picture(enum uvc_frame_format format,
                                      const nal_unit_t *unit) {
    gsize payload = nal_header_offset(unit) +
                    ((format == UVC_FRAME_FORMAT_H265) ? 2 : 1);
    if (unit->len <= payload) return FALSE;
    return (unit->ptr[payload] & 0x80) != 0;
}

/* Partition `units` into access units, writing the index of each AU's first unit
 * into `starts` and returning the number of AUs found. AU k spans
 * [starts[k], starts[k+1]), the last one running to `count`.
 *
 * Two boundary signals, in priority order:
 *   AUD present  the delimiter IS the first NAL of its access unit, so it is an
 *                exact boundary - no heuristic involved.
 *   AUD absent   the standard fallback: a slice that opens a new picture ends the
 *                AU that already holds one. Any non-VCL run immediately before
 *                that slice (SEI, parameter sets) belongs to the NEW access unit,
 *                so the cut is placed at the head of that run, not at the slice.
 *
 * A device that emits neither an AUD nor a decodable first-slice bit simply never
 * splits, which degrades to the whole delivery as one AU - the same grouping a
 * single-picture frame gets, and never a mid-picture cut. */
gsize split_access_units(enum uvc_frame_format format,
                         const nal_unit_t *units, gsize count,
                         gsize *starts, gsize max_starts) {
    gsize n = 0;
    gboolean au_has_slice = FALSE;
    gsize nonvcl_run = G_MAXSIZE;   /* first non-VCL NAL after the last slice */

    for (gsize i = 0; i < count; i++) {
        gboolean is_slice = nal_is_slice(units[i].type);
        gboolean boundary = TRUE;
        gsize at = i;

        if (n == 0) {
            at = 0;
        } else if (units[i].type == UNIT_AUD) {
            at = i;
        } else if (is_slice && au_has_slice && nal_opens_new_picture(format, &units[i])) {
            at = (nonvcl_run != G_MAXSIZE) ? nonvcl_run : i;
        } else {
            boundary = FALSE;
        }

        if (boundary) {
            if (n >= max_starts) {
                GST_WARNING("Access-unit count exceeds max=%" G_GSIZE_FORMAT
                            "; stored %" G_GSIZE_FORMAT " and merged the remainder "
                            "into the last one. Size the array with the NAL count.",
                            max_starts, n);
                break;
            }
            starts[n++] = at;
            au_has_slice = FALSE;
        }

        if (is_slice) {
            au_has_slice = TRUE;
            nonvcl_run = G_MAXSIZE;
        } else if (nonvcl_run == G_MAXSIZE) {
            nonvcl_run = i;
        }
    }

    return n;
}

/* Latch one parameter set into the element's cache.
 *
 * An oversized or empty set is refused outright (heap-overflow guard against the
 * fixed SPSPPSBUFSZ arrays), and the cached copy is rewritten only when it
 * actually CHANGED (L10): SPS/PPS/VPS repeat before every IDR, so an
 * unconditional store rewrites the disk cache each GOP and wears the flash for
 * nothing. send_sps_pps latches on every set regardless, so the parameter sets
 * are always re-prepended in band even when the cache write is skipped. */
static void cache_parameter_set(GstLibuvcH264Src *self, const nal_unit_t *unit,
                                gboolean *updated) {
    unsigned char *dst;
    gint *dst_len;
    const char *label;

    switch (unit->type) {
        case UNIT_VPS: dst = self->vps; dst_len = &self->vps_length; label = "VPS"; break;
        case UNIT_SPS: dst = self->sps; dst_len = &self->sps_length; label = "SPS"; break;
        case UNIT_PPS: dst = self->pps; dst_len = &self->pps_length; label = "PPS"; break;
        default: return;
    }

    if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
        GST_WARNING_OBJECT(self, "Dropping oversized/invalid %s NAL (%" G_GSIZE_FORMAT
            " bytes; max %d) to prevent heap overflow", label, unit->len, SPSPPSBUFSZ);
        return;
    }

    if ((gsize)*dst_len != unit->len || memcmp(dst, unit->ptr, unit->len) != 0) {
        *dst_len = (gint)unit->len;
        memcpy(dst, unit->ptr, unit->len);
        *updated = TRUE;
    }
    self->send_sps_pps = TRUE;
}

/* Framerate-mismatch behavior (harden-v2 Task 9; Oracle Option B).
 *
 * The negotiated framerate (caps 1/fps, used for DURATION and the live-source
 * latency report) is only a nominal contract with downstream. The device's real
 * delivery cadence routinely differs from it: a "30 fps" camera may settle at
 * ~24 fps, run jittery, or stall and burst. This element does NOT coerce the
 * nominal cadence onto a non-conforming device. The policy is:
 *
 *   - PTS is stamped from the real running-time the frame arrived at
 *     (ts = gst_clock_get_time(clock) - base_time, computed below), regardless
 *     of the negotiated fps. A slow/fast/jittery device is reflected faithfully
 *     in the timestamps instead of being snapped onto an idealized grid.
 *   - DURATION stays the constant caps-derived 1/fps. It is a nominal hint, not
 *     a measurement of the real inter-arrival delta, so it never tracks the
 *     mismatched rate.
 *   - The element never renegotiates caps and never drops or duplicates frames
 *     to force the nominal cadence. Every delivered frame is forwarded exactly
 *     once with a strictly monotonic GST_BUFFER_OFFSET, so downstream can still
 *     detect real drops on the wire.
 *   - A material, sustained divergence between the measured cadence and the
 *     negotiated fps is surfaced once via a one-time GST_INFO/WARNING for
 *     diagnostics; it does not change the stamping policy.
 *
 * Rationale: downstream (h264parse/mux/srt) wants honest arrival timing far more
 * than a synthetic constant rate; rewriting PTS onto the nominal grid is what
 * caused the historical skip/stall artifacts. Regression-guarded by
 * tests/test_framerate_mismatch.c (and tests/test_pts_drift.c). */
void frame_callback(uvc_frame_t *frame, void *ptr) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)ptr;

    if (!frame || !frame->data || frame->data_bytes <= 0) {
        GST_WARNING_OBJECT(self, "Empty or invalid frame received.");
        return;
    }

    /* data_bytes is a size_t; a frame larger than INT_MAX would historically
       truncate to a negative int length and be silently dropped. Such a frame
       is corrupt/absurd, so reject it explicitly up front (L4). */
    if (frame->data_bytes > (gsize)G_MAXINT) {
        GST_WARNING_OBJECT(self, "Dropping oversized frame (%" G_GSIZE_FORMAT
                           " bytes; exceeds G_MAXINT).", (gsize)frame->data_bytes);
        return;
    }

    unsigned char* data = frame->data;
    gsize data_bytes = frame->data_bytes;
    gboolean updated_sps_pps = FALSE;

    /* The clock and the PTS baseline are shared with set_clock(), which can swap
       the clock or reset the baseline from another thread. Snapshot the clock
       under the object lock and take our own ref, so reading the time (the
       expensive part) and dropping the ref happen outside the lock and can never
       race a concurrent unref/free. */
    GstClock *clock = NULL;
    GstClockTime base_time = 0;
    GST_OBJECT_LOCK(self);
    if (self->clock) {
        clock = gst_object_ref(self->clock);
        base_time = self->base_time;
    }
    GST_OBJECT_UNLOCK(self);

    if (!clock) return;
    GstClockTime now = gst_clock_get_time(clock);
    gst_object_unref(clock);

    /* Latch the running base time on the first frame after a (re)start or clock
       change. gst_element_get_base_time() takes the object lock itself, so read
       it before re-entering our critical section, then commit under the lock. */
    if (base_time == G_MAXUINT64) {
        base_time = gst_element_get_base_time(GST_ELEMENT(self));
        GST_OBJECT_LOCK(self);
        self->base_time = base_time;
        GST_OBJECT_UNLOCK(self);
    }
    GstClockTime ts = now - base_time;

    /* Size the array to the actual NAL count so a multi-slice frame (4K can
       carry well over a dozen slices) delivers every slice instead of dropping
       units past a fixed cap (L2). */
    gsize unit_count = count_nal_units(self->frame_format, data, data_bytes);
    nal_unit_t *units = g_new(nal_unit_t, unit_count ? unit_count : 1);
    gsize c = parse_nal_units(self->frame_format, units, unit_count, data, data_bytes);

    /* ONE GstBuffer per ACCESS UNIT, never per NAL. The pad template advertises
       alignment=au, and every downstream consumer that trusts it (h264parse, the
       V4L2/MPP decoders) mis-frames a picture whose slices arrive as separate
       buffers. Single-slice 1080p is unaffected - its access unit is one slice,
       so the emitted bytes are identical to the per-NAL path - but a multi-slice
       or 4K picture used to be split across a dozen buffers all claiming to be
       whole access units. */
    gsize *au_starts = g_new(gsize, c ? c : 1);
    gsize au_count = split_access_units(self->frame_format, units, c, au_starts, c);

    for (gsize a = 0; a < au_count; a++) {
        gsize first = au_starts[a];
        gsize end = (a + 1 < au_count) ? au_starts[a + 1] : c;

        /* Consume this AU's parameter sets into the cache and measure what is
           left to forward. Parameter sets are never forwarded from the wire; the
           cached copy is re-prepended in front of the AU's first IDR below. */
        gboolean has_idr = FALSE;
        gboolean has_slice = FALSE;
        gsize payload_len = 0;
        for (gsize i = first; i < end; i++) {
            nal_unit_t *unit = &units[i];
            if (nal_is_parameter_set(unit->type)) {
                cache_parameter_set(self, unit, &updated_sps_pps);
                continue;
            }
            if (unit->type == UNIT_FRAME_IDR) has_idr = TRUE;
            if (nal_is_slice(unit->type)) has_slice = TRUE;
            payload_len += unit->len;
        }

        /* Drop everything ahead of the first keyframe so downstream never sees an
           undecodable leading fragment, and drop an AU that carried nothing but
           parameter sets. */
        if ((!self->had_idr && !has_idr) || payload_len == 0) {
            continue;
        }

        gsize prefix_len = 0;
        if (has_idr && (!self->had_idr || self->send_sps_pps)) {
            prefix_len = (gsize)self->sps_length + (gsize)self->pps_length;
            if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                prefix_len += (gsize)self->vps_length;
            }
        }

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, prefix_len + payload_len, NULL);
        gsize at = 0;
        gboolean prefixed = FALSE;
        for (gsize i = first; i < end; i++) {
            nal_unit_t *unit = &units[i];
            if (nal_is_parameter_set(unit->type)) continue;

            /* The cached parameter sets go immediately BEFORE the AU's first IDR
               slice, never at the head of the buffer: an AUD, when present, must
               remain the very first NAL of its access unit. */
            if (!prefixed && prefix_len > 0 && unit->type == UNIT_FRAME_IDR) {
                if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                    gst_buffer_fill(buffer, at, self->vps, self->vps_length);
                    at += self->vps_length;
                }
                gst_buffer_fill(buffer, at, self->sps, self->sps_length);
                at += self->sps_length;
                gst_buffer_fill(buffer, at, self->pps, self->pps_length);
                at += self->pps_length;
                prefixed = TRUE;
                self->send_sps_pps = FALSE;
            }
            gst_buffer_fill(buffer, at, unit->ptr, unit->len);
            at += unit->len;
        }
        if (has_idr) {
            self->had_idr = TRUE;
        }

        // Set timestamps on the buffer
        if (has_slice) {
            /* Option B: stamp the running-time the frame actually arrived at,
               ts = now - base_time (computed above). The arrival clock IS the PTS
               clock, so PTS can never drift from real time and no interval
               estimator/stretch/resync is needed.

               PTS convention for an aggregated access unit: the AU carries the
               arrival running-time of the delivery it came from - identically the
               PTS its FIRST slice would have been stamped with, since every NAL of
               one delivery shares a single arrival instant. */
            GstClockTime timestamp = ts;
            GstClockTime duration;

            /* base_time / prev_pts / frame_interval are shared with set_clock()
               and change_state(); take the object lock for this read-modify-write.
               The buffer fields are written afterwards from locals so the alloc/
               fill and the queue push stay outside the lock. */
            GST_OBJECT_LOCK(self);

            /* prev_pts == G_MAXUINT64 is the rebaseline sentinel (first frame
               after start/reconnect/clock-change or a PAUSED->PLAYING relatch):
               latch ts as-is. Otherwise a ts at or behind the last PTS nudges one
               tick forward so downstream never sees a backwards or repeated PTS.
               Rare in normal flow (clock swap/relatch/reconnect); also covers a
               delivery that carried more than one access unit, whose AUs share a
               single arrival ts. Slices of ONE picture no longer reach here
               separately - they are aggregated into the single buffer above. */
            if (self->prev_pts != G_MAXUINT64 && timestamp <= self->prev_pts) {
                timestamp = self->prev_pts + 1;
                GST_WARNING_OBJECT(self, "non-monotonic running-time "
                    "(clock swap/relatch/reconnect?); clamped PTS to prev_pts + 1");
            }
            self->prev_pts = timestamp;

            /* DURATION is the nominal frame interval (1/fps) from the negotiated
               caps framerate, never an inter-arrival delta. GST_CLOCK_TIME_NONE
               until negotiate() resolves the framerate. */
            duration = (self->frame_interval > 0)
                       ? (GstClockTime) self->frame_interval : GST_CLOCK_TIME_NONE;

            GST_OBJECT_UNLOCK(self);

            /* DTS == PTS: DJI/UVC H.264/H.265 has no B-frames */
            GST_BUFFER_PTS(buffer) = timestamp;
            GST_BUFFER_DTS(buffer) = timestamp;
            GST_BUFFER_DURATION(buffer) = duration;
            GST_LOG_OBJECT(self, "PTS %" GST_TIME_FORMAT, GST_TIME_ARGS(timestamp));
        }

        // Monotonic access-unit counter so downstream can detect drops. Only the
        // feeder thread runs frame_callback, so this needs no lock.
        GST_BUFFER_OFFSET(buffer) = self->frame_offset;
        GST_BUFFER_OFFSET_END(buffer) = self->frame_offset + 1;
        self->frame_offset++;

        g_async_queue_push(self->frame_queue, buffer);
    }

    g_free(au_starts);
    g_free(units);

    if (updated_sps_pps) {
        /* store_spspps() runs on the libuvc callback thread; self->index is a
         * gchar* that set_property(PROP_INDEX) can g_free()/replace on the app
         * thread. Snapshot the cache key once under GST_OBJECT_LOCK here, then
         * write the cache off the immutable copy with the lock released. */
        spspps_key_t key;
        spspps_key_snapshot(self, &key);
        store_spspps(self, &key);
    }
}
