#ifndef GST_LIBUVC_H264_SRC_SPSPPS_CACHE_H
#define GST_LIBUVC_H264_SRC_SPSPPS_CACHE_H

#include <stdio.h>
#include "gstlibuvch264src.h"

G_BEGIN_DECLS

/* Upper bound on the device selector copied into a snapshot. A "serial:<sn>"
 * selector carries a USB serial string (<=126 UTF-16 code units in the USB
 * descriptor), so 256 bytes is generous; an over-long selector is truncated by
 * g_strlcpy(), which only ever yields a safe, deterministic key. */
#define SPSPPS_INDEX_MAX 256

/* Immutable snapshot of the cache-key fields, taken once under GST_OBJECT_LOCK
 * by the caller (frame_callback / negotiate). The cache I/O runs off this copy
 * so it never dereferences self->index - a gchar* that set_property(PROP_INDEX)
 * can g_free()/replace on another thread - while the file I/O runs unlocked. */
typedef struct {
    char index[SPSPPS_INDEX_MAX];
    gboolean have_index;
    int is_h265;
    int width;
    int height;
} spspps_key_t;

/* Security: bound a stored parameter-set length to the fixed SPSPPSBUFSZ
 * self->{vps,sps,pps} arrays so a negative/oversized *_length can never make
 * store_spspps()'s fwrite() over-read. Negative floors to 0; oversized caps at
 * SPSPPSBUFSZ. Pure, so unit-testable (test_nal_parse.c store_bounds). */
static inline gsize spspps_clamp_len(gint len) {
    if (len <= 0) {
        return 0;
    }
    return ((gsize) len > (gsize) SPSPPSBUFSZ) ? (gsize) SPSPPSBUFSZ : (gsize) len;
}

void spspps_key_snapshot(GstLibuvcH264Src *self, spspps_key_t *key);
void load_spspps(GstLibuvcH264Src *self, const spspps_key_t *key);
void store_spspps(GstLibuvcH264Src *self, const spspps_key_t *key);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_SPSPPS_CACHE_H */
