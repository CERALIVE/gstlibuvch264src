/* Concurrency test for the SPS/PPS cache index race (Task 3).
 *
 * store_spspps() runs on the libuvc callback thread and reads self->index (a
 * gchar*); set_property(PROP_INDEX) g_free()s and replaces it on the application
 * thread. Before the fix that was an unsynchronised read of a pointer the other
 * thread could free - a use-after-free. The fix snapshots the cache key under
 * GST_OBJECT_LOCK (spspps_key_snapshot) and takes the same object lock in
 * set_property/get_property.
 *
 * This statically links the element TUs + libuvc mock into ONE binary (like
 * test_pts_thread_safety) so both sides of the race run instrumented. One thread
 * snapshots the key and writes the cache in a tight loop while the main thread
 * hammers g_object_set(self,"index",...). Teeth:
 *   - ASAN variant: drop the lock and g_free() races the snapshot's read -> the
 *     use-after-free aborts the run. With the lock it is clean.
 *   - behavioural: the run completes with no crash, the cache writer actually
 *     ran, and the index is a valid string afterwards.
 *   - TSAN variant: clean under tsan_cache.suppressions (GST_OBJECT_LOCK is a
 *     GMutex blind spot; the scoped suppression covers only the locked index
 *     functions, so any OTHER new race still reports).
 */

#include <gst/check/gstcheck.h>
#include <string.h>

/* gstcheck.h defines GST_CAT_DEFAULT (check_debug); drop it so the element's
 * internal header installs its own category without a redefinition warning. */
#undef GST_CAT_DEFAULT
#include "gstlibuvch264src_internal.h"
#include "spspps_cache.h"

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

#define RACE_ITERATIONS 10000

static const char *const g_selectors[] = {
  "0",
  "serial:CAM-A",
  "serial:CAM-BBBBBBBBBBBBBBBBBBBB",
  "1234:5678",
  "bus:1:5",
  "serial:CAM-C",
};

typedef struct
{
  GstLibuvcH264Src *self;
  gint stop;                    /* atomic */
  gint stores;                  /* atomic: cache writes completed */
} race_ctx_t;

static gpointer
cache_writer_thread (gpointer data)
{
  race_ctx_t *ctx = data;
  while (!g_atomic_int_get (&ctx->stop)) {
    spspps_key_t key;
    spspps_key_snapshot (ctx->self, &key);
    store_spspps (ctx->self, &key);
    g_atomic_int_inc (&ctx->stores);
  }
  return NULL;
}

GST_START_TEST (test_index_cache_race)
{
  register_element ();

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->negotiated_width = 1920;
  self->negotiated_height = 1080;
  self->sps_length = 8;
  self->pps_length = 8;
  memset (self->sps, 0xA5, self->sps_length);
  memset (self->pps, 0x5A, self->pps_length);

  race_ctx_t ctx = { self, 0, 0 };
  GThread *writer = g_thread_new ("cache-writer", cache_writer_thread, &ctx);

  for (int i = 0; i < RACE_ITERATIONS; i++) {
    g_object_set (self, "index",
        g_selectors[i % G_N_ELEMENTS (g_selectors)], NULL);
  }

  g_atomic_int_set (&ctx.stop, 1);
  g_thread_join (writer);

  fail_unless (g_atomic_int_get (&ctx.stores) > 0,
      "cache writer never ran; the race was not exercised");

  gchar *final_index = NULL;
  g_object_get (self, "index", &final_index, NULL);
  fail_unless (final_index != NULL && *final_index != '\0',
      "index is empty/NULL after concurrent set");
  g_free (final_index);

  gst_object_unref (self);
}

GST_END_TEST;

static Suite *
cache_race_suite (void)
{
  Suite *s = suite_create ("libuvch264src-cache-race");
  TCase *tc = tcase_create ("index_cache_race");
  tcase_set_timeout (tc, 60);
  tcase_add_test (tc, test_index_cache_race);
  suite_add_tcase (s, tc);
  return s;
}

GST_CHECK_MAIN (cache_race);
