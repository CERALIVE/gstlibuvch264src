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

    return i;
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

    for (gsize i = 0; i < c; i++) {
        nal_unit_t *unit = &units[i];
        GstBuffer *buffer = NULL;
        gsize buffer_offset = 0;

        switch (unit->type) {
            case UNIT_VPS:
                if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
                    GST_WARNING_OBJECT(self, "Dropping oversized/invalid VPS NAL "
                        "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                        unit->len, SPSPPSBUFSZ);
                    continue;
                }
                // L10: only flag a disk write when the parameter set actually
                // changed. SPS/PPS/VPS repeat before every IDR, so an
                // unconditional store rewrites the cache file each GOP and wears
                // the flash for nothing. send_sps_pps still latches every time so
                // the sets are re-prepended in-band; only the cache write is gated.
                if ((gsize)self->vps_length != unit->len ||
                    memcmp(self->vps, unit->ptr, unit->len) != 0) {
                    self->vps_length = unit->len;
                    memcpy(self->vps, unit->ptr, self->vps_length);
                    updated_sps_pps = TRUE;
                }
                self->send_sps_pps = TRUE;
                continue;
            case UNIT_SPS:
                if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
                    GST_WARNING_OBJECT(self, "Dropping oversized/invalid SPS NAL "
                        "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                        unit->len, SPSPPSBUFSZ);
                    continue;
                }
                if ((gsize)self->sps_length != unit->len ||
                    memcmp(self->sps, unit->ptr, unit->len) != 0) {
                    self->sps_length = unit->len;
                    memcpy(self->sps, unit->ptr, self->sps_length);
                    updated_sps_pps = TRUE;
                }
                self->send_sps_pps = TRUE;
                continue;
            case UNIT_PPS:
                if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
                    GST_WARNING_OBJECT(self, "Dropping oversized/invalid PPS NAL "
                        "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                        unit->len, SPSPPSBUFSZ);
                    continue;
                }
                if ((gsize)self->pps_length != unit->len ||
                    memcmp(self->pps, unit->ptr, unit->len) != 0) {
                    self->pps_length = unit->len;
                    memcpy(self->pps, unit->ptr, self->pps_length);
                    updated_sps_pps = TRUE;
                }
                self->send_sps_pps = TRUE;
                continue;
            case UNIT_FRAME_IDR: {
                if (!self->had_idr || self->send_sps_pps) {
                    buffer_offset = self->sps_length + self->pps_length;
                    if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                        buffer_offset += self->vps_length;
                    }

                    buffer = gst_buffer_new_allocate(NULL, buffer_offset + unit->len, NULL);
                    int offset = 0;
                    if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                        gst_buffer_fill(buffer, offset, self->vps, self->vps_length);
                        offset += self->vps_length;
                    }
                    gst_buffer_fill(buffer, offset, self->sps, self->sps_length);
                    offset += self->sps_length;

                    gst_buffer_fill(buffer, offset, self->pps, self->pps_length);
                    self->send_sps_pps = FALSE;
                }
                if (!self->had_idr) {
                    self->had_idr = TRUE;
                }
                break;
            }
            default:
                if (!self->had_idr) {
                    continue;
                }
        }

        if (!buffer) {
          buffer = gst_buffer_new_allocate(NULL, unit->len, NULL);
        }
        gst_buffer_fill(buffer, buffer_offset, unit->ptr, unit->len);

        // Set timestamps on the buffer
        if (units[i].type == UNIT_FRAME_IDR || units[i].type == UNIT_FRAME_NON_IDR) {
            /* Option B: stamp the running-time the frame actually arrived at,
               ts = now - base_time (computed above). The arrival clock IS the PTS
               clock, so PTS can never drift from real time and no interval
               estimator/stretch/resync is needed. */
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
               Rare in normal flow (clock swap/relatch/reconnect); also covers the
               multi-slice case where slices of one access unit share a ts. */
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

        // Monotonic frame counter so downstream can detect drops. Only the
        // feeder thread runs frame_callback, so this needs no lock.
        GST_BUFFER_OFFSET(buffer) = self->frame_offset;
        GST_BUFFER_OFFSET_END(buffer) = self->frame_offset + 1;
        self->frame_offset++;

        g_async_queue_push(self->frame_queue, buffer);
    }

    g_free(units);

    if (updated_sps_pps) {
        store_spspps(self);
    }
}
