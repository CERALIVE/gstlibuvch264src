#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "gstlibuvch264src.h"
#include "gstlibuvch264src_internal.h"
#include "uvc_device.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"
#include "ptz_control.h"
#include <gst/gst.h>
#include <libuvc/libuvc.h>

GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);

enum {
  PROP_0,
  PROP_INDEX,
  PROP_LAST
};

#define H264_CAPS "video/x-h264," \
                  "stream-format=(string)byte-stream," \
                  "alignment=(string)au"
#define H265_CAPS "video/x-h265," \
                  "stream-format=(string)byte-stream," \
                  "alignment=(string)au"

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS(H264_CAPS "; " H265_CAPS)
);

G_DEFINE_TYPE_WITH_CODE(GstLibuvcH264Src, gst_libuvc_h264_src, GST_TYPE_PUSH_SRC,
  GST_DEBUG_CATEGORY_INIT(gst_libuvc_h264_src_debug, "libuvch264src", 0, "libuvch264src element"));

static gboolean gst_libuvc_h264_negotiate(GstBaseSrc * basesrc);
static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec);
static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec);
static gboolean gst_libuvc_h264_set_clock(GstElement *element, GstClock *clock);
static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_unlock(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_unlock_stop(GstBaseSrc *src);
static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf);
static void gst_libuvc_h264_src_finalize(GObject *object);

/* GAsyncQueue forbids NULL payloads, so create() can never receive a NULL
 * "no more frames" marker. unlock() instead pushes this dedicated address to
 * wake a blocked create(); its value is irrelevant, only its identity matters. */
static const gchar flush_sentinel = 0;
#define FLUSH_SENTINEL ((gpointer) &flush_sentinel)

static void gst_libuvc_h264_src_class_init(GstLibuvcH264SrcClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

  base_src_class->negotiate = GST_DEBUG_FUNCPTR(gst_libuvc_h264_negotiate);
  gobject_class->set_property = gst_libuvc_h264_src_set_property;
  gobject_class->get_property = gst_libuvc_h264_src_get_property;

  g_object_class_install_property(gobject_class, PROP_INDEX,
    g_param_spec_string("index", "Index", "Device location, e.g., '0'",
                        DEFAULT_DEVICE_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata(element_class,
    "UVC H.264 / H.265 Video Source", "Source/Video",
    "Captures H.264 or H.265 video from a UVC device", "Name");

  gst_element_class_add_pad_template(element_class,
    gst_static_pad_template_get(&src_template));

  element_class->set_clock = gst_libuvc_h264_set_clock;
  base_src_class->start = gst_libuvc_h264_src_start;
  base_src_class->stop = gst_libuvc_h264_src_stop;
  base_src_class->unlock = gst_libuvc_h264_src_unlock;
  base_src_class->unlock_stop = gst_libuvc_h264_src_unlock_stop;
  push_src_class->create = gst_libuvc_h264_src_create;
  gobject_class->finalize = gst_libuvc_h264_src_finalize;
}

static void gst_libuvc_h264_src_init(GstLibuvcH264Src *self) {
  self->index = g_strdup(DEFAULT_DEVICE_INDEX);
  self->uvc_ctx = NULL;
  self->uvc_dev = NULL;
  self->uvc_devh = NULL;
  self->clock = NULL;
  self->frame_queue = g_async_queue_new();
  self->streaming = FALSE;
  self->flushing = 0;
  self->base_time = G_MAXUINT64;
  self->prev_pts = G_MAXUINT64;
  
  // Control socket initialization
  self->control_socket = -1;
  self->control_thread = NULL;
  self->control_running = FALSE;
  g_mutex_init(&self->control_mutex);

  gchar sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x34, 0xAC, 0x4D, 0x00, 0xF0, 0x04, 0x4F, 0xCB, 0x35, 0x01, 0x01, 0x01, 0x40, 0x00, 0x00, 0xFA, 0x00, 0x00, 0x3A, 0x98, 0x03, 0xC7, 0x0C, 0xA8 };
  self->sps_length = sizeof(sps);
  memcpy(self->sps, sps, self->sps_length);

  gchar pps[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };
  self->pps_length = sizeof(pps);
  memcpy(self->pps, pps, self->pps_length);

  gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
  gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}

static gboolean gst_libuvc_h264_negotiate(GstBaseSrc * basesrc) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(basesrc);

    GstCaps *thiscaps = gst_pad_query_caps(GST_BASE_SRC_PAD(basesrc), NULL);
    GST_INFO_OBJECT(basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);

    GstCaps *peercaps = gst_pad_peer_query_caps(GST_BASE_SRC_PAD(basesrc), NULL);
    GST_INFO_OBJECT(basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);

    GstCaps *caps = NULL;
    if (peercaps) {
        caps = gst_caps_intersect(peercaps, thiscaps);
        gst_caps_unref(thiscaps);
        gst_caps_unref(peercaps);
    } else {
        caps = thiscaps;
    }

    GST_INFO_OBJECT(basesrc, "caps intersection: %" GST_PTR_FORMAT, caps);

    gint width = -1, height = -1, framerate = -1;
    GstCaps *best_caps = NULL;

    // Enumerate supported H264 / H265 resolutions and framerates
    // And select the highest compatible resolution, at the highest supported framerate
    for (const uvc_format_desc_t *format_desc = uvc_get_format_descs(self->uvc_devh);
         format_desc; format_desc = format_desc->next)
    {
        gboolean is_h264 = (memcmp(format_desc->fourccFormat, "H264", 4) == 0);
        gboolean is_h265 = (memcmp(format_desc->fourccFormat, "H265", 4) == 0);

        if (!is_h264 && !is_h265) continue;

        GstCaps *tmp_caps = gst_caps_from_string(is_h264? H264_CAPS : H265_CAPS);
        GstStructure *tmp_structure = gst_caps_get_structure(tmp_caps, 0);

        for (const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
             frame_desc; frame_desc = frame_desc->next)
        {
            gint resolution = frame_desc->wWidth * frame_desc->wHeight;

            gst_structure_set(tmp_structure,
                              "width", G_TYPE_INT, frame_desc->wWidth,
                              "height", G_TYPE_INT, frame_desc->wHeight,
                              NULL);

            gint fps = -1;
            if (frame_desc->intervals) {
                GValue framerates = G_VALUE_INIT;
                g_value_init(&framerates, GST_TYPE_LIST);

                for (const uint32_t *interval = frame_desc->intervals; *interval; interval++) {
                    gint _fps = 1e7 / *interval;
                    if (_fps > fps) {
                        fps = _fps;
                    }

                    GValue fps = G_VALUE_INIT;
                    g_value_init(&fps, GST_TYPE_FRACTION);
                    gst_value_set_fraction(&fps, (gint)_fps, 1);
                    gst_value_list_append_value(&framerates, &fps);
                }

                gst_structure_set_value(tmp_structure, "framerate", &framerates);
            } else {
                gint fps_min = 1e7 / frame_desc->dwMaxFrameInterval;
                gint fps = 1e7 / frame_desc->dwMinFrameInterval;
                gst_structure_set(tmp_structure, "framerate", GST_TYPE_FRACTION_RANGE, fps_min, 1, fps, 1, NULL);
            }

            if (gst_caps_can_intersect(caps, tmp_caps)) {
                if (resolution > (width * height)
                    || (resolution == (width * height) && fps > framerate)) {
                    width = frame_desc->wWidth;
                    height = frame_desc->wHeight;
                    self->frame_format = is_h264 ? UVC_FRAME_FORMAT_H264 : UVC_FRAME_FORMAT_H265;

                    if (best_caps) {
                        gst_caps_unref(best_caps);
                    }
                    best_caps = gst_caps_intersect(caps, tmp_caps);
                    GstStructure *s = gst_caps_get_structure(best_caps, 0);
                    gst_structure_fixate_field_nearest_fraction(s, "framerate", fps, 1);

                    gint fr_num, fr_den;
                    gst_structure_get_fraction(s, "framerate", &fr_num, &fr_den);
                    framerate = fr_num / fr_den;
                }
            }

        } // for frame_desc

        gst_caps_unref(tmp_caps);
    } // for format_desc

    if (width < 0 || height < 0 || framerate < 0 || !best_caps) {
        GST_ERROR_OBJECT(self, "Unable to negotiate common caps\n");
        return FALSE;
    }

    int res = uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                              self->frame_format, width, height, framerate);
    if (res < 0) {
        GST_ERROR_OBJECT(self, "Unable to get stream control: %s", uvc_strerror(res));
        return FALSE;
    }

    self->frame_interval = (1000L * 1000L * 1000L) / framerate;

    /* Persist the negotiated resolution so the SPS/PPS cache key (L5) reflects
     * the active format; load_spspps/store_spspps read these. */
    self->negotiated_width = width;
    self->negotiated_height = height;

    gst_base_src_set_caps(basesrc, best_caps);

    GST_INFO_OBJECT(basesrc, "Negotiated caps: %" GST_PTR_FORMAT, best_caps);

    load_spspps(self);

    return TRUE;
}

static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

  switch (prop_id) {
    case PROP_INDEX:
      g_free(self->index);
      self->index = g_value_dup_string(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

  switch (prop_id) {
    case PROP_INDEX:
      g_value_set_string(value, self->index);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_libuvc_h264_set_clock(GstElement *element, GstClock *clock) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(element);

  GST_OBJECT_LOCK(self);

  if (self->clock) {
    gst_object_unref(self->clock);
    self->clock = NULL;
  }

  if (clock) {
    self->clock = gst_object_ref(clock);
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;
    self->pts_offset_sum = 0;
    self->pts_stretch = 0;
  }

  GST_OBJECT_UNLOCK(self);

  return GST_ELEMENT_CLASS(gst_libuvc_h264_src_parent_class)->set_clock(element, clock);
}

static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  GST_DEBUG_OBJECT(self, "Starting libuvc source");

  // Check if we need to cleanup a previous session
  if (self->uvc_ctx != NULL || self->uvc_devh != NULL) {
    GST_WARNING_OBJECT(self, "Previous session not fully cleaned up, forcing cleanup");
    gst_libuvc_h264_src_stop(src);
    usleep(1000000); // Wait 1 second for USB to settle
  }

  // Resolve the device index up-front, before touching libuvc. The `index`
  // property stays a string so it can grow richer selectors later (vid:pid /
  // serial), but today a bare, non-negative integer is an ordinal into the
  // enumerated device list. Reject anything else loudly instead of silently
  // selecting device 0 the way atoi() would have.
  errno = 0;
  char *index_end = NULL;
  long device_ordinal = strtol(self->index ? self->index : "", &index_end, 10);
  if (self->index == NULL || index_end == self->index || *index_end != '\0' ||
      errno != 0 || device_ordinal < 0 || device_ordinal > INT_MAX) {
    GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
        ("Invalid device index \"%s\"", self->index ? self->index : "(null)"),
        ("index must be a non-negative integer ordinal"));
    return FALSE;
  }

  // Initialize libuvc context
  res = uvc_init(&self->uvc_ctx, NULL);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Failed to initialize libuvc: %s", uvc_strerror(res));
    return FALSE;
  }
  
  uvc_device_t **dev_list;
  res = uvc_find_devices(self->uvc_ctx, &dev_list, 0, 0, NULL);
  if (res < 0) {
    GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
        ("No UVC devices found"),
        ("uvc_find_devices failed: %s", uvc_strerror(res)));
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

  for (int i = 0; dev_list[i] != NULL; ++i) {
    if ((long)i == device_ordinal) {
      self->uvc_dev = dev_list[i];
      break;
    }
  }

  if (!self->uvc_dev) {
    GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
        ("No UVC device at index %ld", device_ordinal),
        ("ordinal %ld matched none of the enumerated UVC devices", device_ordinal));
    uvc_free_device_list(dev_list, 1);
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

  // The selected device aliases an entry in dev_list, and uvc_free_device_list()
  // unrefs every entry; take our own reference first so it survives the free.
  uvc_ref_device(self->uvc_dev);
  uvc_free_device_list(dev_list, 1);

  res = uvc_open(self->uvc_dev, &self->uvc_devh);
  if (res < 0) {
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE,
        ("Unable to open UVC device at index %ld", device_ordinal),
        ("uvc_open failed: %s", uvc_strerror(res)));
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

  // Start control socket thread
  self->control_running = TRUE;
  self->control_thread = g_thread_new("uvc-control",
                                     gst_libuvc_h264_src_control_thread,
                                     self);

  GST_DEBUG_OBJECT(self, "Libuvc source started successfully");
  return TRUE;
}

// FIXED: Proper cleanup with libusb handle release
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Stopping libuvc source");

  // Stop control thread
  if (self->control_running) {
    GST_DEBUG_OBJECT(self, "Stopping control thread");
    self->control_running = FALSE;
    
    // Wake up control thread
    int wakeup_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (wakeup_fd >= 0) {
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      strcpy(addr.sun_path, "/tmp/libuvc_control");
      fcntl(wakeup_fd, F_SETFL, O_NONBLOCK);
      connect(wakeup_fd, (struct sockaddr*)&addr, sizeof(addr));
      close(wakeup_fd);
    }
    
    if (self->control_thread) {
      g_thread_join(self->control_thread);
      self->control_thread = NULL;
    }
  }

  // Close control socket
  if (self->control_socket >= 0) {
    close(self->control_socket);
    self->control_socket = -1;
    unlink("/tmp/libuvc_control");
  }

  // CRITICAL FIX: Stop streaming and force USB release
  if (self->streaming && self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Stopping UVC streaming");
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
    usleep(100000); // 100ms for streaming to stop
  }

  // Clear frame queue
  if (self->frame_queue) {
    GstBuffer *buffer;
    while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
      gst_buffer_unref(buffer);
    }
  }

  // FIXED: Release USB device BEFORE uvc_close
  if (self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Force releasing USB device");
    gst_libuvc_h264_src_force_usb_release(self);
    
    // Now call uvc_close (it may fail but that's OK since we already released)
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
  }

  // Unreference UVC device
  if (self->uvc_dev) {
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
  }

  // Exit UVC context
  if (self->uvc_ctx) {
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
  }

  // control_mutex is NOT cleared here: stop() runs on every restart and is even
  // re-entered from start()'s cleanup path, so clearing it would leave the
  // control thread locking a destroyed mutex. It is cleared once in finalize().

  GST_DEBUG_OBJECT(self, "Libuvc source fully stopped");
  return TRUE;
}

// Interrupt a create() that is blocked waiting for a frame (e.g. on disconnect
// or shutdown), so state changes and teardown never deadlock on a silent source.
static gboolean gst_libuvc_h264_src_unlock(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Unlock: interrupting create()");

  g_atomic_int_set(&self->flushing, 1);
  g_async_queue_push(self->frame_queue, FLUSH_SENTINEL);

  return TRUE;
}

static gboolean gst_libuvc_h264_src_unlock_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Unlock stop: resuming create()");

  g_atomic_int_set(&self->flushing, 0);

  // Drop sentinels (and any frames buffered during the flush) so the next
  // create() resumes from a clean queue.
  gpointer item;
  while ((item = g_async_queue_try_pop(self->frame_queue)) != NULL) {
    if (item != FLUSH_SENTINEL) {
      gst_buffer_unref(item);
    }
  }

  return TRUE;
}

static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  if (!self->streaming) {
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;
    self->pts_offset_sum = 0;
    self->pts_stretch = 0;

    self->streaming = TRUE;

    // Start streaming
    res = uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl, frame_callback, self, 0);
    if (res < 0) {
      self->streaming = FALSE;
      GST_ERROR_OBJECT(self, "Unable to start streaming: %s", uvc_strerror(res));
      return GST_FLOW_ERROR;
    }
  }

  // Bounded wait so unlock() can interrupt a stalled capture: the timeout is a
  // backstop for a silent source, while unlock()'s sentinel wakes us at once.
  while (TRUE) {
    gpointer item = g_async_queue_timeout_pop(self->frame_queue, TIMEOUT_DURATION);

    if (g_atomic_int_get(&self->flushing)) {
      if (item != NULL && item != FLUSH_SENTINEL) {
        gst_buffer_unref(item);
      }
      return GST_FLOW_FLUSHING;
    }

    if (item == NULL || item == FLUSH_SENTINEL) {
      // Plain timeout, or a stale sentinel from a finished flush: keep waiting
      // rather than ending the stream on a transient gap.
      continue;
    }

    *buf = item;
    return GST_FLOW_OK;
  }
}

static void gst_libuvc_h264_src_finalize(GObject *object) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

    GST_DEBUG_OBJECT(self, "Finalizing libuvc source");

    // Force cleanup
    gst_libuvc_h264_src_stop(GST_BASE_SRC(self));

    if (self->index) {
        g_free(self->index);
        self->index = NULL;
    }

    if (self->frame_queue) {
        GstBuffer *buffer;
        while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
            gst_buffer_unref(buffer);
        }
        
        g_async_queue_unref(self->frame_queue);
        self->frame_queue = NULL;
    }

    // Sole clear point for control_mutex (paired with g_mutex_init in init): the
    // control thread was already joined by stop() above, so this is race-free.
    g_mutex_clear(&self->control_mutex);

    GST_DEBUG_OBJECT(self, "Libuvc source finalized");

    G_OBJECT_CLASS(gst_libuvc_h264_src_parent_class)->finalize(object);
}

static gboolean plugin_init(GstPlugin *plugin) {
    // Also register under the libuvch26xsrc alias since it now supports both H264 and H265
    if (!gst_element_register(plugin, "libuvch26xsrc", GST_RANK_NONE, GST_TYPE_LIBUVC_H264_SRC))
      return FALSE;
    return gst_element_register(plugin, "libuvch264src", GST_RANK_NONE, GST_TYPE_LIBUVC_H264_SRC);
}

#define PACKAGE "libuvch264src"
#define VERSION "1.0"
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    libuvch264src,
    "UVC H264 / H265 Source Plugin",
    plugin_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "https://gstreamer.freedesktop.org/"
)
