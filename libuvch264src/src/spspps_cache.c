#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"
#include "spspps_path.h"

#define DIRBUFLEN 4096
static __thread char dir_buf[DIRBUFLEN];

/* Copy the cache-key fields out of the instance under GST_OBJECT_LOCK. This is
 * the writer-vs-reader handshake for self->index: set_property(PROP_INDEX) takes
 * the same lock to g_free()/replace it, so copying it here turns the historical
 * callback-thread use-after-free into a plain value copy. The lock is dropped
 * before any file I/O - it must NEVER be held across fopen/fread/fwrite. */
void spspps_key_snapshot(GstLibuvcH264Src *self, spspps_key_t *key) {
    GST_OBJECT_LOCK(self);
    if (self->index) {
        g_strlcpy(key->index, self->index, sizeof(key->index));
        key->have_index = TRUE;
    } else {
        key->index[0] = '\0';
        key->have_index = FALSE;
    }
    key->is_h265 = (self->frame_format == UVC_FRAME_FORMAT_H265);
    key->width = self->negotiated_width;
    key->height = self->negotiated_height;
    GST_OBJECT_UNLOCK(self);
}

static const char *spspps_home_dir(GstLibuvcH264Src *self) {
    const char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
        GST_WARNING_OBJECT(self, "HOME environment variable not set.");
        home_dir = "";
    }
    return home_dir;
}

/* Build the cache FILE path from the immutable snapshot (no self->index read). */
static char *get_spspps_path(GstLibuvcH264Src *self, const spspps_key_t *key) {
    const char *index = key->have_index ? key->index : NULL;
    int ret = spspps_build_path(dir_buf, DIRBUFLEN, spspps_home_dir(self), index,
                                key->is_h265, key->width, key->height);
    if (ret < 0) {
        GST_ERROR_OBJECT(self, "Error building SPS/PPS path");
        return NULL;
    }

    return dir_buf;
}

static void create_hidden_directory(GstLibuvcH264Src *self) {
    int ret = spspps_build_path(dir_buf, DIRBUFLEN, spspps_home_dir(self), NULL,
                                0, 0, 0);
    if (ret < 0) {
        GST_WARNING_OBJECT(self, "SPS/PPS cache directory path unavailable; skipping creation.");
        return;
    }
    char *hidden_dir = dir_buf;

    struct stat st;
    if (stat(hidden_dir, &st) == -1) {
        if (mkdir(hidden_dir, 0700) != 0)
            GST_ERROR_OBJECT(self, "Error creating directory %s\n", hidden_dir);
        else
            GST_WARNING_OBJECT(self, "Directory %s created successfully.\n", hidden_dir);
    } else if (!S_ISDIR(st.st_mode))
        GST_WARNING_OBJECT(self, "Warning: %s exists but is not a directory.\n", hidden_dir);
}

static FILE *open_spspps_file(GstLibuvcH264Src *self, const spspps_key_t *key, char mode) {
    if (mode == 'w' || mode == 'a') {
        create_hidden_directory(self);
    }

    char m[3];
    snprintf(m, sizeof(m), "%cb", mode);
    char *file_name = get_spspps_path(self, key);
    if (file_name == NULL) {
        GST_WARNING_OBJECT(self, "SPS/PPS cache path unavailable; skipping cache I/O.");
        return NULL;
    }
    FILE *fp = fopen(file_name, m);
    return fp;
}

// Must only be called after the caps have been negotiated and the format is known
void load_spspps(GstLibuvcH264Src *self, const spspps_key_t *key) {
    FILE* fp = open_spspps_file(self, key, 'r');
    if (fp) {
        unsigned char buf[SPSPPSBUFSZ*3];
        gsize read_bytes = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);

        #define MAX_UNITS_LOAD 3
        nal_unit_t units[MAX_UNITS_LOAD];
        gsize c = parse_nal_units(self->frame_format, units, MAX_UNITS_LOAD, buf, read_bytes);

        for (gsize i = 0; i < c; i++) {
            switch (units[i].type) {
                case UNIT_VPS:
                    if (units[i].len == 0 || units[i].len > SPSPPSBUFSZ) {
                        GST_WARNING_OBJECT(self, "Dropping oversized/invalid cached VPS NAL "
                            "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                            units[i].len, SPSPPSBUFSZ);
                        break;
                    }
                    memcpy(self->vps, units[i].ptr, units[i].len);
                    self->vps_length = units[i].len;
                    break;
                case UNIT_SPS:
                    if (units[i].len == 0 || units[i].len > SPSPPSBUFSZ) {
                        GST_WARNING_OBJECT(self, "Dropping oversized/invalid cached SPS NAL "
                            "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                            units[i].len, SPSPPSBUFSZ);
                        break;
                    }
                    memcpy(self->sps, units[i].ptr, units[i].len);
                    self->sps_length = units[i].len;
                    break;
                case UNIT_PPS:
                    if (units[i].len == 0 || units[i].len > SPSPPSBUFSZ) {
                        GST_WARNING_OBJECT(self, "Dropping oversized/invalid cached PPS NAL "
                            "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                            units[i].len, SPSPPSBUFSZ);
                        break;
                    }
                    memcpy(self->pps, units[i].ptr, units[i].len);
                    self->pps_length = units[i].len;
                    break;
                default:
                    // We shouldn't have other types; but ignore them if we do
                    break;
            }
        }
    }
}

void store_spspps(GstLibuvcH264Src *self, const spspps_key_t *key) {
    FILE* fp = open_spspps_file(self, key, 'w');
	if (fp) {
		if (key->is_h265) {
			fwrite(self->vps, 1, spspps_clamp_len(self->vps_length), fp);
		}
		fwrite(self->sps, 1, spspps_clamp_len(self->sps_length), fp);
		fwrite(self->pps, 1, spspps_clamp_len(self->pps_length), fp);
		fclose(fp);
	}
}
