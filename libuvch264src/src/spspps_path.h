#ifndef GST_LIBUVC_H264_SRC_SPSPPS_PATH_H
#define GST_LIBUVC_H264_SRC_SPSPPS_PATH_H

/*
 * Pure, dependency-free SPS/PPS cache path builder.
 *
 * Kept free of GLib/GStreamer so it can be unit-tested in isolation
 * (tests/test_cache.c) without constructing a GObject. spspps_cache.c wraps
 * this with the element's logging and instance state.
 *
 * Three hardening properties live here:
 *
 *   M8 (path traversal): the device `index` is never interpolated verbatim.
 *   Every byte outside [A-Za-z0-9-] - in particular '/' and '.' - is escaped as
 *   "_<HH>" (two hex digits), so a hostile index such as "../.." or "/etc/passwd"
 *   carries no path separator and no ".." traversal and can never escape
 *   ~/.spspps.
 *
 *   Per-selector key (collision fix): the escape is injective - "_" itself is
 *   escaped - so distinct selectors (serial:CAM-A vs serial:CAM-B, two different
 *   vid:pid, two bus: addresses) always map to DISTINCT keys. The old strtol()
 *   key collapsed every non-ordinal selector onto "0", so distinct cameras shared
 *   one cache file. A plain ordinal is pure digits, which pass through verbatim,
 *   so "0" stays "0" and the existing on-disk layout is preserved (backward
 *   compatible).
 *
 *   L5 (resolution key): the negotiated codec and WxH are folded into the file
 *   name, so a cache entry written for one resolution can never be loaded for a
 *   different one.
 */

#include <stdio.h>
#include <stdlib.h>

/*
 * Build the cache path into `out` (capacity `outlen`).
 *
 *   index == NULL -> the cache directory itself ("<home>/.spspps").
 *   index != NULL -> a resolution-keyed file
 *                    ("<home>/.spspps/<idx>_<codec>_<width>x<height>").
 *
 * Returns the number of characters written (excluding the NUL) on success, or
 * -1 if `out` is unusable or the path would be truncated. On -1 the caller MUST
 * skip the cache rather than touch the filesystem.
 */
static inline int spspps_build_path(char *out, size_t outlen,
                                    const char *home_dir, const char *index,
                                    int is_h265, int width, int height) {
    if (out == NULL || outlen == 0) {
        return -1;
    }
    if (home_dir == NULL) {
        home_dir = "";
    }

    int ret;
    if (index == NULL) {
        ret = snprintf(out, outlen, "%s/.spspps", home_dir);
    } else {
        /* M8 + per-selector key: escape, do not interpolate. Every byte outside
         * [A-Za-z0-9-] becomes "_<HH>", which (a) strips any '/' or "." so no
         * path separator or ".." traversal survives, and (b) is injective ("_"
         * is itself escaped to "_5F"), so distinct selectors never collide. Pure
         * digits pass through verbatim, keeping the ordinal layout ("0" -> "0"). */
        static const char hexd[] = "0123456789ABCDEF";
        char key[768];
        size_t k = 0;
        for (const unsigned char *p = (const unsigned char *)index; *p; p++) {
            unsigned char c = *p;
            int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-';
            if (safe) {
                if (k + 1 >= sizeof(key)) return -1;
                key[k++] = (char)c;
            } else {
                if (k + 3 >= sizeof(key)) return -1;
                key[k++] = '_';
                key[k++] = hexd[(c >> 4) & 0xF];
                key[k++] = hexd[c & 0xF];
            }
        }
        key[k] = '\0';

        /* L5: codec + resolution are part of the key. */
        const char *codec = is_h265 ? "h265" : "h264";
        ret = snprintf(out, outlen, "%s/.spspps/%s_%s_%dx%d",
                       home_dir, key, codec, width, height);
    }

    if (ret < 0 || (size_t)ret >= outlen) {
        return -1;
    }
    return ret;
}

#endif /* GST_LIBUVC_H264_SRC_SPSPPS_PATH_H */
