#ifndef GST_LIBUVC_H264_SRC_INTERNAL_H
#define GST_LIBUVC_H264_SRC_INTERNAL_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <libuvc/libuvc.h>
#include "gstlibuvch264src.h"

G_BEGIN_DECLS

/* Debug category is defined (non-static) in gstlibuvch264src.c and referenced
 * by every translation unit of this element. Declared extern here so the split
 * modules share the single category created by GST_DEBUG_CATEGORY_INIT. */
GST_DEBUG_CATEGORY_EXTERN(gst_libuvc_h264_src_debug);
#define GST_CAT_DEFAULT gst_libuvc_h264_src_debug

struct _GstLibuvcH264Src {
  GstPushSrc parent_instance;
  gchar* index;
  uvc_context_t *uvc_ctx;
  uvc_device_t *uvc_dev;
  uvc_device_handle_t *uvc_devh;
  uvc_stream_ctrl_t uvc_ctrl;
  enum uvc_frame_format frame_format;
  gint negotiated_width;
  gint negotiated_height;
  /* The framerate negotiate() resolved, kept so the opt-in reconnect path can
   * re-run uvc_get_stream_ctrl_format_size() with the original geometry. */
  gint negotiated_framerate;
  GAsyncQueue *frame_queue;
  gboolean streaming;
  gint flushing; /* atomic: set by unlock(), checked by create() to bail out */
  /* Sustained-silence disconnect detection + opt-in reconnect (Task 18). libuvc
   * delivers no NULL frame on unplug in callback mode (it goes silent), so
   * create() infers a disconnect after DISCONNECT_TIMEOUT_COUNT consecutive 1s
   * timeouts. Reset in start() and whenever a real frame arrives. */
  gint consecutive_timeouts;
  gboolean reconnect_enabled; /* PROP_RECONNECT: opt-in in-element auto-reconnect */
  /* Interruptible reconnect backoff (Task 7). The backoff between retries parks
   * in g_cond_wait_until() on reconnect_cond; unlock() sets flushing and
   * broadcasts the cond so a state change to NULL/PAUSED tears the element down
   * promptly instead of waiting out the full (up to 16 s) backoff window.
   * Initialised in init(), cleared in finalize(). */
  GMutex reconnect_lock;
  GCond reconnect_cond;
  /* Opt-in USB payload override (Task 12, gated on bmaxpayload-analysis.md).
   * max_payload is the REQUESTED value (PROP_MAX_PAYLOAD); 0 is the sentinel
   * "use the device-negotiated value" that leaves negotiation byte-for-byte
   * unchanged. max_payload_effective is the value actually committed to the
   * device after negotiation - the device-negotiated value on a graceful
   * fallback - and is what a property read-back reports (mirrors how
   * control-socket-path is read back after PAUSED). Both are guarded by
   * GST_OBJECT_LOCK so the set/get-property app thread and the negotiate/
   * reconnect streaming thread form a proper happens-before. */
  guint max_payload;
  guint max_payload_effective;
  GstClock *clock;
  GstClockTime base_time;
  GstClockTime prev_pts;
  /* Nominal frame interval (1/fps in ns) resolved by negotiate() from the
   * fixated caps framerate. Set once and never mutated while streaming (Option B
   * stamps PTS = arrival running-time directly, with no interval estimator); read
   * by the LATENCY query and stamped as GST_BUFFER_DURATION (always the nominal
   * caps interval, never an inter-arrival delta). */
  gint64 frame_interval; // in ns
  /* Monotonic per-session output counter for GST_BUFFER_OFFSET. Reset in
   * start(); only touched on the feeder thread. */
  guint64 frame_offset;
  gboolean had_idr;
  gboolean send_sps_pps;
  gint vps_length;
  gint sps_length;
  gint pps_length;
  unsigned char vps[SPSPPSBUFSZ];
  unsigned char sps[SPSPPSBUFSZ];
  unsigned char pps[SPSPPSBUFSZ];
  
  // Control socket additions. Opt-in (M9): enabled gates it on; path is the bound
  // path (explicit property or per-instance $XDG_RUNTIME_DIR default), heap-owned
  // here and freed in finalize().
  gboolean control_socket_enabled;
  gchar* control_socket_path;
  gint control_socket;
  gpointer control_thread;
  gboolean control_running;
  GMutex control_mutex;

  // PTZ state, filled by ptz_probe_capabilities() at open and guarded by
  // control_mutex. pan and tilt share one UVC control, so a single-axis set must
  // re-send the other axis from *_cur (the last value written).
  gint pan_min, pan_max, pan_cur;
  gint tilt_min, tilt_max, tilt_cur;
  gint zoom_min, zoom_max, zoom_cur;
  gboolean pan_supported, tilt_supported, zoom_supported;
  gboolean ptz_supported;
};

/* Test seam for the reconnect backoff (Task 7). A hook is invoked at the start
 * of every backoff interval with the attempt index and the nominal backoff
 * seconds, and returns the microseconds to actually wait; returning 0 collapses
 * the wall-clock wait so a test never sleeps the full 1+2+4+8+16 s. A NULL hook
 * (production default) waits the full nominal backoff. The hook also lets a test
 * record the interval sequence to assert the exponential 1,2,4,8,16 s schedule. */
typedef gint64 (*GstLibuvcReconnectBackoffHook)(GstLibuvcH264Src *self,
                                                gint attempt, guint backoff_s);
void gst_libuvc_h264_src_set_reconnect_backoff_hook(
    GstLibuvcReconnectBackoffHook hook);

gboolean gst_libuvc_h264_src_reconnect(GstLibuvcH264Src *self);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_INTERNAL_H */
