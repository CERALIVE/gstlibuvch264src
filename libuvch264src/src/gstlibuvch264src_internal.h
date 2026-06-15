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
  GAsyncQueue *frame_queue;
  gboolean streaming;
  GstClock *clock;
  int64_t pts_offset_sum;
  int64_t pts_stretch;
  GstClockTime base_time;
  GstClockTime prev_pts;
  gint64 frame_interval; // in ns
  guint64 prev_int_ts;
  gint frame_count;
  gboolean had_idr;
  gboolean send_sps_pps;
  gint vps_length;
  gint sps_length;
  gint pps_length;
  unsigned char vps[SPSPPSBUFSZ];
  unsigned char sps[SPSPPSBUFSZ];
  unsigned char pps[SPSPPSBUFSZ];
  
  // Control socket additions
  gint control_socket;
  gpointer control_thread;
  gboolean control_running;
  GMutex control_mutex;
};

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_INTERNAL_H */
