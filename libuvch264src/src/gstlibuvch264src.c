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
#include "gstlibuvch264src_error.h"
#include "uvc_device.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"
#include "ptz_control.h"
#include "quirks.h"
#include <gst/gst.h>
#include <libuvc/libuvc.h>
#include <libusb-1.0/libusb.h>

GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);

enum {
  PROP_0,
  PROP_INDEX,
  PROP_PAN,
  PROP_TILT,
  PROP_ZOOM,
  PROP_CONTROL_SOCKET,
  PROP_CONTROL_SOCKET_PATH,
  PROP_RECONNECT,
  PROP_MAX_PAYLOAD,
  PROP_TRANSFER_BUFFERS,
  PROP_RESET_SETTLE_MAX_MS,
  PROP_RESET_REARM_FRAMES,
  PROP_AUTO_PORT_RESET,
  PROP_DELIVERABLE_CAPS,
  PROP_LAST
};

/* Sustained-silence disconnect detection. libuvc delivers no NULL frame on
 * unplug in callback mode (Task 4 spike), so create() infers a disconnect after
 * this many consecutive TIMEOUT_DURATION (1 s) pop timeouts with no frame. */
#define DISCONNECT_TIMEOUT_COUNT 5

/* Opt-in in-element reconnect: bounded exponential backoff 1,2,4,8,16 s. */
#define RECONNECT_MAX_RETRIES 5
/* Frames a port-reset recovery must deliver before the one-shot re-arms
 * (PROP_RESET_REARM_FRAMES default). Enough to distinguish a device that
 * genuinely came back from one that emits a frame and immediately re-wedges
 * (which would otherwise reset the port forever). */
#define RESET_REARM_FRAMES_DEFAULT 30
#define RESET_REARM_FRAMES_MIN 1
#define RESET_REARM_FRAMES_MAX 100000
/* Upper BOUND on the whole reset-to-advancing-frames recovery
 * (PROP_RESET_SETTLE_MAX_MS default) - a budget, not a delay. The recovery
 * spends only as long as the device actually needs: it polls re-enumeration,
 * reopens, and requires a real frame, finishing the instant that succeeds. No
 * per-device settle constant is encoded anywhere; a device that comes back in
 * 200 ms recovers in 200 ms, and one that needs seconds still gets them. */
#define RESET_SETTLE_MAX_MS_DEFAULT 8000
#define RESET_SETTLE_MAX_MS_MAX 120000
/* Micro-backoff for the re-enumeration poll: start tight so a fast return is
 * detected almost immediately, then ease off so a long absence costs few
 * syscalls. Generic polling geometry - neither value is derived from any
 * device's measured behavior. */
#define RESET_POLL_INITIAL_MS 25
#define RESET_POLL_MAX_MS 200
#define RECONNECT_BACKOFF_INITIAL_S 1

/* Opt-in USB payload override (Task 12, gated on bmaxpayload-analysis.md §5).
 * MAX_PAYLOAD_DEFAULT is the sentinel: 0 = "use the device-negotiated value",
 * so the unset default leaves negotiation byte-for-byte unchanged. A nonzero
 * request is clamped to [MAX_PAYLOAD_MIN_LEGAL, MAX_PAYLOAD_MAX]: the floor keeps
 * it legal on a USB2 high-speed bulk endpoint (512 B packet), and the ceiling
 * caps the bulk transfer pool (LIBUVC_NUM_TRANSFER_BUFS x payload ~= 100 x
 * payload) well under a constrained Rockchip CMA/DMA budget and usbfs's 16 MB
 * single-buffer limit - a few MB per buffer at most, never tens of MB. */
#define MAX_PAYLOAD_DEFAULT 0u
#define MAX_PAYLOAD_MIN_LEGAL 512u
#define MAX_PAYLOAD_MAX (4u * 1024u * 1024u)

/* Opt-in USB transfer-buffer count override (Task 11, fork A2
 * uvc_set_transfer_buffers). TRANSFER_BUFFERS_DEFAULT is the sentinel: 0 = "leave
 * libuvc's default transfer-buffer count unchanged", so the unset default never
 * touches the fork API. A nonzero request is clamped to the fork's own
 * [TRANSFER_BUFFERS_MIN, TRANSFER_BUFFERS_MAX] band in the apply helper (NOT the
 * param spec, whose range stays the full 0..255 uint8 so an in-range set never
 * trips a GObject range warning). */
#define TRANSFER_BUFFERS_DEFAULT 0u
#define TRANSFER_BUFFERS_MIN 2u
#define TRANSFER_BUFFERS_MAX 100u
#define TRANSFER_BUFFERS_SPEC_MAX 255u

/* The fork exports uvc_set_transfer_buffers(); raw upstream libuvc does not. The
 * build's feature guard (HAVE_UVC_TRANSFER_BUFFERS) is set by meson.build /
 * CMakeLists.txt after probing the selected libuvc. When it is absent the
 * property stays registered but a nonzero value warns and no-ops so the
 * upstream-fallback build stays green. */
#if defined(HAVE_UVC_TRANSFER_BUFFERS) && !defined(LIBUVCH264SRC_NO_TRANSFER_BUFFERS_API)
#define TRANSFER_BUFFERS_API_AVAILABLE 1
#else
#define TRANSFER_BUFFERS_API_AVAILABLE 0
#endif

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
static gboolean gst_libuvc_h264_src_query(GstBaseSrc *basesrc, GstQuery *query);
static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec);
static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec);
static gboolean gst_libuvc_h264_set_clock(GstElement *element, GstClock *clock);
static GstStateChangeReturn gst_libuvc_h264_src_change_state(GstElement *element,
                                                             GstStateChange transition);
static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_unlock(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_unlock_stop(GstBaseSrc *src);
static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf);
static void gst_libuvc_h264_src_finalize(GObject *object);
static gboolean gst_libuvc_h264_src_set_ptz(GstLibuvcH264Src *self,
                                            gint pan, gint tilt, gint zoom);
static GstCaps *gst_libuvc_h264_src_filter_deliverable_caps(GstLibuvcH264Src *self,
                                            GstCaps *advertised,
                                            guint vendor_id, guint product_id);
static gboolean gst_libuvc_h264_src_negotiate_clean_payload(GstLibuvcH264Src *self,
                                            gint width, gint height, gint fps);
static void gst_libuvc_h264_src_apply_max_payload(GstLibuvcH264Src *self,
                                            gint width, gint height, gint fps);
static void gst_libuvc_h264_src_apply_transfer_buffers(GstLibuvcH264Src *self);

/* GAsyncQueue forbids NULL payloads, so create() can never receive a NULL
 * "no more frames" marker. unlock() instead pushes this dedicated address to
 * wake a blocked create(); its value is irrelevant, only its identity matters. */
static const gchar flush_sentinel = 0;
#define FLUSH_SENTINEL ((gpointer) &flush_sentinel)

static GstLibuvcResetPollHook gst_libuvc_reset_poll_hook = NULL;

/* Park for wait_us on reconnect_cond instead of sleeping, so unlock() - which
 * sets flushing and broadcasts the cond - wakes every waiter at once and a
 * NULL/PAUSED transition never blocks out a full wait window. Returns FALSE
 * when a flush asked us to bail. */
static gboolean gst_libuvc_h264_src_interruptible_wait(GstLibuvcH264Src *self,
                                                       gint64 wait_us) {
  if (wait_us > 0) {
    gint64 deadline = g_get_monotonic_time() + wait_us;
    g_mutex_lock(&self->reconnect_lock);
    while (!g_atomic_int_get(&self->flushing)) {
      if (!g_cond_wait_until(&self->reconnect_cond, &self->reconnect_lock,
                             deadline)) {
        break;
      }
    }
    g_mutex_unlock(&self->reconnect_lock);
  }
  return !g_atomic_int_get(&self->flushing);
}

static void gst_libuvc_h264_src_class_init(GstLibuvcH264SrcClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

  base_src_class->negotiate = GST_DEBUG_FUNCPTR(gst_libuvc_h264_negotiate);
  base_src_class->query = GST_DEBUG_FUNCPTR(gst_libuvc_h264_src_query);
  gobject_class->set_property = gst_libuvc_h264_src_set_property;
  gobject_class->get_property = gst_libuvc_h264_src_get_property;

  g_object_class_install_property(gobject_class, PROP_INDEX,
    g_param_spec_string("index", "Index",
                        "Device selector: ordinal \"0\", \"vid:pid\" (hex, e.g. "
                        "\"1234:5678\"), \"serial:<sn>\", or \"bus:<bus>:<addr>\"",
                        DEFAULT_DEVICE_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Native PTZ properties. Param-spec bounds cover the UVC arcsecond / focal
   * domain; the real per-device range is enforced at set time, and a set on an
   * axis the device does not report is silently ignored (capability-gated). */
  g_object_class_install_property(gobject_class, PROP_PAN,
    g_param_spec_int("pan", "Pan", "Absolute pan position in UVC arcseconds",
                     -648000, 648000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_TILT,
    g_param_spec_int("tilt", "Tilt", "Absolute tilt position in UVC arcseconds",
                     -648000, 648000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_ZOOM,
    g_param_spec_int("zoom", "Zoom", "Absolute zoom as a UVC focal length",
                     0, 65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Opt-in PTZ control socket (M9). Default OFF: the legacy world-accessible
   * /tmp/libuvc_control is gone, so nothing binds unless asked. */
  g_object_class_install_property(gobject_class, PROP_CONTROL_SOCKET,
    g_param_spec_boolean("control-socket", "Control socket",
                         "Enable the Unix-domain PTZ control socket",
                         FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_CONTROL_SOCKET_PATH,
    g_param_spec_string("control-socket-path", "Control socket path",
                        "Explicit control socket path; empty selects a "
                        "per-instance path under $XDG_RUNTIME_DIR",
                        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Opt-in in-element auto-reconnect (Task 18). Default OFF: a mid-stream
   * disconnect always posts a RESOURCE/READ error; only with this enabled does
   * the element first attempt a bounded-backoff teardown/reopen before erroring. */
  g_object_class_install_property(gobject_class, PROP_RECONNECT,
    g_param_spec_boolean("reconnect", "Reconnect",
                         "Attempt bounded in-element auto-reconnect when the "
                         "device disconnects mid-stream",
                         FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Opt-in USB payload override (Task 12, gated on bmaxpayload-analysis.md).
   * Default 0 is the "leave the device-negotiated value unchanged" sentinel, so
   * the negotiation is byte-for-byte identical to before unless set. A nonzero
   * value is clamped to the conservative band and applied via probe/commit with
   * read-back; a device that refuses it falls back to the device-negotiated
   * value. Read-back reports the effective committed value. */
  g_object_class_install_property(gobject_class, PROP_MAX_PAYLOAD,
    g_param_spec_uint("max-payload", "Max payload transfer size",
                      "USB payload transfer size hint in bytes "
                      "(dwMaxPayloadTransferSize). 0 = use the device-negotiated "
                      "value (default; negotiation unchanged). A nonzero value is "
                      "clamped to [512, 4194304], applied via probe/commit with "
                      "read-back, and falls back to the device-negotiated value "
                      "if the device refuses it. Read-back reports the effective "
                      "committed value.",
                      0, MAX_PAYLOAD_MAX, MAX_PAYLOAD_DEFAULT,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Opt-in USB transfer-buffer count override (Task 11, fork A2). Default 0 is
   * the "leave libuvc's default count unchanged" sentinel. The param-spec range
   * is the full 0..255 uint8 domain; the conservative [2, 100] clamp is applied
   * at streaming-start time in the apply helper, NOT here, so a value inside the
   * spec never trips a GObject range warning (which gst-check would turn into a
   * longjmp). A read-back reports the effective committed value. */
  g_object_class_install_property(gobject_class, PROP_TRANSFER_BUFFERS,
    g_param_spec_uint("transfer-buffers", "USB transfer buffer count",
                      "Number of USB transfer buffers libuvc submits per stream. "
                      "0 = use the library default (default; unchanged). A "
                      "nonzero value is clamped to [2, 100] and applied via the "
                      "fork's uvc_set_transfer_buffers() right before streaming "
                      "starts; a read-back reports the effective value. Requires "
                      "the CeraLive libuvc fork; ignored (with a warning) on "
                      "upstream libuvc.",
                      0, TRANSFER_BUFFERS_SPEC_MAX, TRANSFER_BUFFERS_DEFAULT,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Bounds of the wedged-device recovery (Task 14). The recovery is
   * readiness-driven, not timed: these cap how long it may keep trying and how
   * much delivered video it demands as proof, so no device's settle time is
   * baked into the element. */
  g_object_class_install_property(gobject_class, PROP_RESET_SETTLE_MAX_MS,
    g_param_spec_uint("reset-settle-max-ms", "Reset settle budget",
                      "Budget in milliseconds for the element's OWN readiness "
                      "loop after a port reset: re-enumeration polling, the "
                      "reopen retries and the wait for the first real frame. It "
                      "is a budget, not a delay - the recovery returns as soon "
                      "as frames are actually flowing, and stops starting new "
                      "attempts once it is spent. It does NOT bound the "
                      "underlying libuvc/libusb teardown: uvc_stop_streaming() "
                      "and uvc_close() are synchronous and expose no "
                      "interruption seam, and on a device that is still "
                      "re-enumerating they can occasionally push the total well "
                      "past this value (measured ~22 s against an 8 s budget). "
                      "Size it for the fast path; do not treat it as a "
                      "worst-case guarantee.",
                      0, RESET_SETTLE_MAX_MS_MAX, RESET_SETTLE_MAX_MS_DEFAULT,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_RESET_REARM_FRAMES,
    g_param_spec_uint("reset-rearm-frames", "Reset re-arm frames",
                      "Frames the device must deliver after a recovery before "
                      "the one-shot port reset re-arms for a LATER wedge. "
                      "Re-arming on the first frame back would let a device "
                      "that emits one frame and immediately re-wedges reset "
                      "the port forever.",
                      RESET_REARM_FRAMES_MIN, RESET_REARM_FRAMES_MAX,
                       RESET_REARM_FRAMES_DEFAULT,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Safety opt-out for the otherwise always-on wedge recovery. Default TRUE
   * preserves the shipped behavior; FALSE falls through to the normal
   * disconnect/error path without issuing a USB port reset. */
  g_object_class_install_property(gobject_class, PROP_AUTO_PORT_RESET,
    g_param_spec_boolean("auto-port-reset", "Automatic USB port reset",
                         "Issue one USB port reset when a present device goes "
                         "silent; disable to use normal disconnect handling",
                         TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* The mode ladder negotiate() actually selects from, i.e. what this element
   * will ACCEPT - not what the device advertises. NULL until a device has been
   * negotiated. Read it to publish honest options to an operator instead of the
   * raw descriptor ladder, which for a quirked camera contains modes
   * negotiation is guaranteed to refuse. */
  g_object_class_install_property(gobject_class, PROP_DELIVERABLE_CAPS,
    g_param_spec_boxed("deliverable-caps", "Deliverable caps",
                       "Post-quirk mode ladder negotiation selects from; "
                       "NULL before a device is negotiated",
                       GST_TYPE_CAPS,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* The same filter, without a device.
   *
   * A consumer enumerating cameras already holds the advertised ladder (from
   * v4l2) and the vid:pid, and must NOT open the camera to learn which of those
   * modes are real - opening one through libuvc detaches uvcvideo and destroys
   * /dev/videoN. This applies uvc_quirks_filter_caps() to a caller-supplied
   * ladder, so the enumeration answer and the negotiation answer come from one
   * implementation. Pure caps arithmetic: no device I/O. */
  g_signal_new_class_handler("filter-deliverable-caps", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
    G_CALLBACK(gst_libuvc_h264_src_filter_deliverable_caps), NULL, NULL, NULL,
    GST_TYPE_CAPS, 3, GST_TYPE_CAPS, G_TYPE_UINT, G_TYPE_UINT);

  /* Action signal driving all three axes in one emission; each axis is applied
   * only when the device supports it (gated in ptz_control.c). */
  g_signal_new_class_handler("set-ptz", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
    G_CALLBACK(gst_libuvc_h264_src_set_ptz), NULL, NULL, NULL,
    G_TYPE_BOOLEAN, 3, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);

  gst_element_class_set_static_metadata(element_class,
    "UVC H.264 / H.265 Video Source", "Source/Video",
    "Captures H.264 or H.265 video from a UVC device", "Name");

  gst_element_class_add_pad_template(element_class,
    gst_static_pad_template_get(&src_template));

  element_class->set_clock = gst_libuvc_h264_set_clock;
  element_class->change_state = gst_libuvc_h264_src_change_state;
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
  self->deliverable_caps = NULL;
  self->clock = NULL;
  self->frame_queue = g_async_queue_new();
  self->streaming = FALSE;
  self->flushing = 0;
  self->consecutive_timeouts = 0;
  self->reset_recovery_used = FALSE;
  self->frames_since_reset = 0;
  self->reconnect_enabled = FALSE;
  self->max_payload = MAX_PAYLOAD_DEFAULT;
  self->max_payload_effective = 0;
  self->transfer_buffers = TRANSFER_BUFFERS_DEFAULT;
  self->transfer_buffers_effective = 0;
  self->reset_settle_max_ms = RESET_SETTLE_MAX_MS_DEFAULT;
  self->reset_rearm_frames = RESET_REARM_FRAMES_DEFAULT;
  self->auto_port_reset = TRUE;
  self->frame_offset = 0;
  self->base_time = G_MAXUINT64;
  self->prev_pts = G_MAXUINT64;
  
  // Control socket initialization
  self->control_socket_enabled = FALSE;
  self->control_socket_path = NULL;
  self->control_socket = -1;
  self->control_thread = NULL;
  self->control_running = FALSE;
  g_mutex_init(&self->control_mutex);
  g_mutex_init(&self->reconnect_lock);
  g_cond_init(&self->reconnect_cond);

  gchar sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x34, 0xAC, 0x4D, 0x00, 0xF0, 0x04, 0x4F, 0xCB, 0x35, 0x01, 0x01, 0x01, 0x40, 0x00, 0x00, 0xFA, 0x00, 0x00, 0x3A, 0x98, 0x03, 0xC7, 0x0C, 0xA8 };
  self->sps_length = sizeof(sps);
  memcpy(self->sps, sps, self->sps_length);

  gchar pps[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };
  self->pps_length = sizeof(pps);
  memcpy(self->pps, pps, self->pps_length);

  gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
  gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}

/* Re-run a CLEAN device negotiation (NO payload override) so self->uvc_ctrl
 * holds the device-negotiated dwMaxPayloadTransferSize. This is the graceful
 * fallback target whenever an override is refused or wedges stream start
 * (bmaxpayload-analysis.md §5.4): the stream comes up on the device value rather
 * than failing. Records the device value as the effective committed payload for
 * property read-back. Returns FALSE only if the clean re-negotiation itself
 * fails (a genuine device error, not a payload mismatch). */
static gboolean gst_libuvc_h264_src_negotiate_clean_payload(GstLibuvcH264Src *self,
                                            gint width, gint height, gint fps) {
  int res = uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                            self->frame_format, width, height, fps);
  if (res < 0) {
    GST_WARNING_OBJECT(self, "max-payload fallback: clean re-negotiation failed: %s",
                       uvc_strerror(res));
    return FALSE;
  }
  GST_OBJECT_LOCK(self);
  self->max_payload_effective = self->uvc_ctrl.dwMaxPayloadTransferSize;
  GST_OBJECT_UNLOCK(self);
  return TRUE;
}

/* Apply the opt-in max-payload override to the just-negotiated stream control
 * (bmaxpayload-analysis.md §5). Called right after uvc_get_stream_ctrl_format_size
 * in both negotiate() and the reconnect re-arm. Unset (0) is the sentinel: it
 * leaves the device-negotiated value byte-for-byte unchanged (ZERO extra device
 * writes), only recording it for read-back. A nonzero request is clamped to the
 * conservative band, written into the control block, and RE-PROBED so the device
 * write-back (GET_CUR) is observed - never trusting the read-back-free SET_CUR
 * inside uvc_stream_start (the silent-divergence trap). On any re-probe failure
 * or read-back mismatch it reverts to the device-negotiated value (graceful
 * fallback); the stream never fails because of the hint. */
static void gst_libuvc_h264_src_apply_max_payload(GstLibuvcH264Src *self,
                                            gint width, gint height, gint fps) {
  GST_OBJECT_LOCK(self);
  guint requested = self->max_payload;
  GST_OBJECT_UNLOCK(self);

  if (requested == 0) {
    GST_OBJECT_LOCK(self);
    self->max_payload_effective = self->uvc_ctrl.dwMaxPayloadTransferSize;
    GST_OBJECT_UNLOCK(self);
    return;
  }

  guint clamped = requested;
  if (clamped < MAX_PAYLOAD_MIN_LEGAL)
    clamped = MAX_PAYLOAD_MIN_LEGAL;
  if (clamped > MAX_PAYLOAD_MAX)
    clamped = MAX_PAYLOAD_MAX;
  if (clamped != requested)
    GST_WARNING_OBJECT(self, "max-payload %u out of range [%u, %u]; clamped to %u",
                       requested, MAX_PAYLOAD_MIN_LEGAL, MAX_PAYLOAD_MAX, clamped);

  guint32 device_default = self->uvc_ctrl.dwMaxPayloadTransferSize;

  self->uvc_ctrl.dwMaxPayloadTransferSize = clamped;
  uvc_error_t res = uvc_probe_stream_ctrl(self->uvc_devh, &self->uvc_ctrl);
  guint32 committed = self->uvc_ctrl.dwMaxPayloadTransferSize;

  if (res < 0 || committed != clamped) {
    GST_WARNING_OBJECT(self,
        "max-payload %u not honored by device (re-probe %s, device committed %u); "
        "falling back to device-negotiated payload %u",
        clamped, res < 0 ? uvc_strerror(res) : "read-back mismatch",
        committed, device_default);
    gst_libuvc_h264_src_negotiate_clean_payload(self, width, height, fps);
    return;
  }

  GST_INFO_OBJECT(self,
      "max-payload applied: requested %u, device committed %u (was %u)",
      clamped, committed, device_default);
  GST_OBJECT_LOCK(self);
  self->max_payload_effective = committed;
  GST_OBJECT_UNLOCK(self);
}

/* Apply the opt-in transfer-buffers override to the open device handle, called
 * right before uvc_start_streaming() in both the initial start and the reconnect
 * re-arm (the fork rejects the setter mid-stream, so it must precede start).
 * Unset (0) is the sentinel: it never calls the fork API, leaving libuvc's
 * default transfer-buffer count byte-for-byte unchanged (ZERO extra device
 * writes). A nonzero request is clamped to [TRANSFER_BUFFERS_MIN,
 * TRANSFER_BUFFERS_MAX] and pushed via uvc_set_transfer_buffers(); the committed
 * (clamped) value is recorded for read-back. A device that refuses it keeps
 * libuvc's default (graceful; the stream never fails because of the hint). When
 * the fork symbol is absent a nonzero request emits ONE warning and no-ops. */
static void gst_libuvc_h264_src_apply_transfer_buffers(GstLibuvcH264Src *self) {
  GST_OBJECT_LOCK(self);
  guint requested = self->transfer_buffers;
  GST_OBJECT_UNLOCK(self);

  if (requested == 0)
    return;

#if TRANSFER_BUFFERS_API_AVAILABLE
  guint clamped = requested;
  if (clamped < TRANSFER_BUFFERS_MIN)
    clamped = TRANSFER_BUFFERS_MIN;
  if (clamped > TRANSFER_BUFFERS_MAX)
    clamped = TRANSFER_BUFFERS_MAX;
  if (clamped != requested)
    GST_WARNING_OBJECT(self, "transfer-buffers %u out of range [%u, %u]; "
                       "clamped to %u", requested, TRANSFER_BUFFERS_MIN,
                       TRANSFER_BUFFERS_MAX, clamped);

  uvc_error_t res = uvc_set_transfer_buffers(self->uvc_devh, (uint8_t) clamped);
  if (res < 0) {
    GST_WARNING_OBJECT(self,
        "transfer-buffers %u not applied by libuvc (%s); using the library "
        "default count", clamped, uvc_strerror(res));
    return;
  }

  GST_INFO_OBJECT(self, "transfer-buffers applied: requested %u, applied %u",
                  requested, clamped);
  GST_OBJECT_LOCK(self);
  self->transfer_buffers_effective = clamped;
  GST_OBJECT_UNLOCK(self);
#else
  GST_WARNING_OBJECT(self,
      "transfer-buffers %u requested but this libuvc lacks "
      "uvc_set_transfer_buffers (built without the CeraLive fork API); ignoring",
      requested);
#endif
}

/* Log a full inventory of every format/frame descriptor the device advertises.
 * Called from negotiate() right before the no-H264/H265 bus error so field
 * triage (GST_DEBUG=libuvch264src:3) can see WHAT the camera actually offered -
 * fourcc/guid plus each frame's resolution and interval range - when it exposes
 * no codec the element can stream. */
static void gst_libuvc_h264_src_log_format_inventory(GstLibuvcH264Src *self) {
    for (const uvc_format_desc_t *format_desc = uvc_get_format_descs(self->uvc_devh);
         format_desc; format_desc = format_desc->next)
    {
        const guint8 *g = format_desc->guidFormat;
        GST_WARNING_OBJECT(self,
            "  format fourcc '%.4s' guid "
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            format_desc->fourccFormat,
            g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);

        for (const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
             frame_desc; frame_desc = frame_desc->next)
        {
            GST_WARNING_OBJECT(self,
                "    %ux%u frame interval [%u..%u] (100ns units)",
                frame_desc->wWidth, frame_desc->wHeight,
                frame_desc->dwMinFrameInterval, frame_desc->dwMaxFrameInterval);
        }
    }
}

/* The highest discrete rate a mode offers, or -1 when it carries a continuous
 * RANGE. The -1 is deliberate and load-bearing: the preference below compares it,
 * and fixating "nearest" to -1/1 snaps a range to its LOWEST rate - the behavior
 * continuous-frame-interval devices have always had here. */
static gint gst_libuvc_h264_src_top_rate(const GstStructure *structure) {
    const GValue *rates = gst_structure_get_value(structure, "framerate");
    if (rates == NULL || !GST_VALUE_HOLDS_LIST(rates)) {
        return -1;
    }

    gint top = -1;
    for (guint i = 0; i < gst_value_list_get_size(rates); i++) {
        const GValue *rate = gst_value_list_get_value(rates, i);
        gint num = gst_value_get_fraction_numerator(rate);
        gint den = gst_value_get_fraction_denominator(rate);

        if (den > 0 && num / den > top) {
            top = num / den;
        }
    }
    return top;
}

static void gst_libuvc_h264_src_publish_ladder(GstLibuvcH264Src *self,
                                               GstCaps *ladder) {
    GST_OBJECT_LOCK(self);
    gst_caps_replace(&self->deliverable_caps, ladder);
    GST_OBJECT_UNLOCK(self);
    gst_caps_unref(ladder);
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
    gboolean result = FALSE;
    gboolean found_codec_format = FALSE;

    /* Every mode that survives the quirk filter, accumulated as it is selected
     * from and published on "deliverable-caps". This is the list an enumerating
     * consumer needs and could not see before. */
    GstCaps *ladder = gst_caps_new_empty();

    // vid:pid quirk seam (A14), resolved BEFORE the selection loop because a quirk
    // can rule advertised-but-undeliverable modes OUT of the selection, not just
    // change how the winning mode is probed. A device with no row gets zeroed
    // limits, which impose nothing.
    uvc_quirk_limits_t quirk_limits = {0};
    uvc_device_descriptor_t *quirk_desc = NULL;
    if (uvc_get_device_descriptor(self->uvc_dev, &quirk_desc) == UVC_SUCCESS
        && quirk_desc != NULL) {
        uvc_quirks_limits(quirk_desc->idVendor, quirk_desc->idProduct, &quirk_limits);
        uvc_free_device_descriptor(quirk_desc);
    }

    // Enumerate supported H264 / H265 resolutions and framerates
    // And select the highest compatible resolution, at the highest supported framerate
    for (const uvc_format_desc_t *format_desc = uvc_get_format_descs(self->uvc_devh);
         format_desc; format_desc = format_desc->next)
    {
        gboolean is_h264 = (memcmp(format_desc->fourccFormat, "H264", 4) == 0);
        gboolean is_h265 = (memcmp(format_desc->fourccFormat, "H265", 4) == 0);

        if (!is_h264 && !is_h265) continue;
        found_codec_format = TRUE;

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

            if (frame_desc->intervals) {
                GValue framerates = G_VALUE_INIT;
                g_value_init(&framerates, GST_TYPE_LIST);

                for (const uint32_t *interval = frame_desc->intervals; *interval; interval++) {
                    GValue fps = G_VALUE_INIT;
                    g_value_init(&fps, GST_TYPE_FRACTION);
                    gst_value_set_fraction(&fps, (gint)(1e7 / *interval), 1);
                    gst_value_list_append_value(&framerates, &fps);
                    g_value_unset(&fps);
                }

                // gst_structure_set_value() copies the list, so the local GValue
                // owns a GST_TYPE_LIST that must be released or it leaks per call.
                gst_structure_set_value(tmp_structure, "framerate", &framerates);
                g_value_unset(&framerates);
            } else {
                // A device that reports a zero frame interval would divide by
                // zero here (SIGFPE); skip such a degenerate descriptor instead.
                if (frame_desc->dwMinFrameInterval == 0 ||
                    frame_desc->dwMaxFrameInterval == 0) {
                    GST_WARNING_OBJECT(self,
                        "Skipping %ux%u: device reported a zero frame interval",
                        frame_desc->wWidth, frame_desc->wHeight);
                    continue;
                }
                gint fps_min = 1e7 / frame_desc->dwMaxFrameInterval;
                gint fps_max = 1e7 / frame_desc->dwMinFrameInterval;

                gst_structure_set(tmp_structure, "framerate", GST_TYPE_FRACTION_RANGE, fps_min, 1, fps_max, 1, NULL);
            }

            // A quirked device advertises rates it cannot deliver. The exclusion
            // lives in ONE function, which the "deliverable-caps" property and the
            // "filter-deliverable-caps" signal publish too, so the modes offered to
            // an operator are by construction the modes this loop can accept.
            GstCaps *deliverable = uvc_quirks_filter_caps(&quirk_limits, tmp_caps);
            if (gst_caps_is_empty(deliverable)) {
                gst_caps_unref(deliverable);
                continue;
            }
            gst_caps_append(ladder, gst_caps_copy(deliverable));

            gint fps = gst_libuvc_h264_src_top_rate(
                gst_caps_get_structure(deliverable, 0));

            if (gst_caps_can_intersect(caps, deliverable)) {
                if (resolution > (width * height)
                    || (resolution == (width * height) && fps > framerate)) {
                    width = frame_desc->wWidth;
                    height = frame_desc->wHeight;
                    self->frame_format = is_h264 ? UVC_FRAME_FORMAT_H264 : UVC_FRAME_FORMAT_H265;

                    if (best_caps) {
                        gst_caps_unref(best_caps);
                    }
                    best_caps = gst_caps_intersect(caps, deliverable);
                    GstStructure *s = gst_caps_get_structure(best_caps, 0);
                    gst_structure_fixate_field_nearest_fraction(s, "framerate", fps, 1);

                    gint fr_num, fr_den;
                    gst_structure_get_fraction(s, "framerate", &fr_num, &fr_den);
                    framerate = fr_num / fr_den;
                }
            }
            gst_caps_unref(deliverable);

        } // for frame_desc

        gst_caps_unref(tmp_caps);
    } // for format_desc

    if (!found_codec_format) {
        // The device exposes no H264/H265 format descriptor at all, so there is
        // nothing to stream. Log a full inventory of what it DID advertise first
        // (field triage), then post a bus ERROR (not just a debug log) so
        // downstream consumers (cerastream/CeraUI) can react, instead of falling
        // through with uninitialized width/height/framerate.
        GST_WARNING_OBJECT(self,
            "device exposes no H264/H265 format; advertised formats follow:");
        gst_libuvc_h264_src_log_format_inventory(self);
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), UVC_ERROR_NOT_SUPPORTED,
            "negotiating caps: device exposes no H264/H265 format");
        goto out;
    }

    // framerate <= 0 (not just < 0): a device whose fastest interval rounds down
    // to 0 fps would otherwise divide by zero at the frame_interval computation.
    if (width < 0 || height < 0 || framerate <= 0 || !best_caps) {
        GST_ERROR_OBJECT(self, "Unable to negotiate common caps");
        goto out;
    }

    // Reuses the flags resolved above the selection loop: a device with no row has
    // none set and the probe count stays at 1. QUIRK_DOUBLE_PROBE (libuvc #242)
    // issues the format-size probe twice, discarding the first result.
    if (quirk_limits.flags & QUIRK_DOUBLE_PROBE) {
        // Some devices return a stale/rejected stream control on the first
        // probe; run it once and discard the result before the real probe.
        uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                        self->frame_format, width, height, framerate);
    }

    int res = uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                              self->frame_format, width, height, framerate);
    if (res < 0) {
        GST_ERROR_OBJECT(self, "Unable to get stream control: %s", uvc_strerror(res));
        goto out;
    }

    // Opt-in max-payload override (Task 12). Unset leaves negotiation unchanged;
    // a set value is probe/committed with read-back and falls back gracefully.
    gst_libuvc_h264_src_apply_max_payload(self, width, height, framerate);

    GST_OBJECT_LOCK(self);
    self->frame_interval = (1000L * 1000L * 1000L) / framerate;
    GST_OBJECT_UNLOCK(self);

    /* Persist the negotiated resolution so the SPS/PPS cache key (L5) reflects
     * the active format; load_spspps/store_spspps read these. The framerate is
     * also kept so the opt-in reconnect path can re-run
     * uvc_get_stream_ctrl_format_size() with the original geometry. */
    self->negotiated_width = width;
    self->negotiated_height = height;
    self->negotiated_framerate = framerate;

    gst_base_src_set_caps(basesrc, best_caps);

    GST_INFO_OBJECT(basesrc, "Negotiated caps: %" GST_PTR_FORMAT, best_caps);

    spspps_key_t cache_key;
    spspps_key_snapshot(self, &cache_key);
    load_spspps(self, &cache_key);

    result = TRUE;

out:
    // Single cleanup path: the working caps and the chosen caps are owned locals.
    // gst_base_src_set_caps() takes its own reference, so best_caps must be freed
    // here on success too, and both must be freed on every error path.
    // The ladder is published on EVERY path, including the failures: a device
    // that could not negotiate is exactly when a consumer needs to know which
    // modes were on offer.
    gst_libuvc_h264_src_publish_ladder(self, ladder);
    if (caps)
        gst_caps_unref(caps);
    if (best_caps)
        gst_caps_unref(best_caps);
    return result;
}

static gboolean gst_libuvc_h264_src_query(GstBaseSrc *basesrc, GstQuery *query) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(basesrc);

  if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
    /* A live source delivers a frame only once it has been fully captured, so
       the minimum latency is one frame interval; report it explicitly rather
       than leaving downstream sinks with the GstBaseSrc default of zero. max ==
       min: the element does not buffer ahead. frame_interval is shared with the
       frame_callback PTS estimator, which mutates it under the object lock, so
       read it the same way; until negotiate() sets it, defer to the base class. */
    GstClockTime latency;
    GST_OBJECT_LOCK(self);
    latency = self->frame_interval > 0
              ? (GstClockTime) self->frame_interval : GST_CLOCK_TIME_NONE;
    GST_OBJECT_UNLOCK(self);

    if (GST_CLOCK_TIME_IS_VALID(latency)) {
      gst_query_set_latency(query, TRUE, latency, latency);
      return TRUE;
    }
  }

  return GST_BASE_SRC_CLASS(gst_libuvc_h264_src_parent_class)->query(basesrc, query);
}

static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

  switch (prop_id) {
    case PROP_INDEX:
      /* self->index is read on the libuvc callback thread (the SPS/PPS cache key
       * snapshot). Mutate it under the object lock so that free/replace and the
       * snapshot's read form a proper happens-before instead of a use-after-free. */
      GST_OBJECT_LOCK(self);
      g_free(self->index);
      self->index = g_value_dup_string(value);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_PAN:
      gst_libuvc_h264_src_ptz_set_pan(self, g_value_get_int(value));
      break;
    case PROP_TILT:
      gst_libuvc_h264_src_ptz_set_tilt(self, g_value_get_int(value));
      break;
    case PROP_ZOOM:
      gst_libuvc_h264_src_ptz_set_zoom(self, g_value_get_int(value));
      break;
    case PROP_CONTROL_SOCKET:
      self->control_socket_enabled = g_value_get_boolean(value);
      break;
    case PROP_CONTROL_SOCKET_PATH: {
      const gchar *path = g_value_get_string(value);
      g_free(self->control_socket_path);
      self->control_socket_path = (path && *path) ? g_strdup(path) : NULL;
      break;
    }
    case PROP_RECONNECT:
      self->reconnect_enabled = g_value_get_boolean(value);
      break;
    case PROP_MAX_PAYLOAD:
      /* Read on the negotiate/reconnect streaming thread by the apply helper;
       * mutate under the object lock so the write and that read form a proper
       * happens-before. Stored verbatim (the conservative clamp is applied at
       * negotiation time, where the device-default is known for the fallback). */
      GST_OBJECT_LOCK(self);
      self->max_payload = g_value_get_uint(value);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_TRANSFER_BUFFERS:
      /* Stored verbatim (the [2,100] clamp is applied at streaming-start time in
       * the apply helper). Read on the negotiate/reconnect streaming thread, so
       * mutate under the object lock for a proper happens-before, mirroring
       * max-payload. */
      GST_OBJECT_LOCK(self);
      self->transfer_buffers = g_value_get_uint(value);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_RESET_SETTLE_MAX_MS:
      /* Read on the streaming thread by the recovery policy; mutate under the
       * object lock so the write and that read form a happens-before. */
      GST_OBJECT_LOCK(self);
      self->reset_settle_max_ms = g_value_get_uint(value);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_RESET_REARM_FRAMES:
      GST_OBJECT_LOCK(self);
      self->reset_rearm_frames = g_value_get_uint(value);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_AUTO_PORT_RESET:
      GST_OBJECT_LOCK(self);
      self->auto_port_reset = g_value_get_boolean(value);
      GST_OBJECT_UNLOCK(self);
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
      GST_OBJECT_LOCK(self);
      g_value_set_string(value, self->index);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_PAN:
      g_value_set_int(value, self->pan_cur);
      break;
    case PROP_TILT:
      g_value_set_int(value, self->tilt_cur);
      break;
    case PROP_ZOOM:
      g_value_set_int(value, self->zoom_cur);
      break;
    case PROP_CONTROL_SOCKET:
      g_value_set_boolean(value, self->control_socket_enabled);
      break;
    case PROP_CONTROL_SOCKET_PATH:
      g_value_set_string(value, self->control_socket_path);
      break;
    case PROP_RECONNECT:
      g_value_set_boolean(value, self->reconnect_enabled);
      break;
    case PROP_MAX_PAYLOAD:
      GST_OBJECT_LOCK(self);
      g_value_set_uint(value, self->max_payload_effective > 0
                              ? self->max_payload_effective
                              : self->max_payload);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_TRANSFER_BUFFERS:
      GST_OBJECT_LOCK(self);
      g_value_set_uint(value, self->transfer_buffers_effective > 0
                              ? self->transfer_buffers_effective
                              : self->transfer_buffers);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_RESET_SETTLE_MAX_MS:
      GST_OBJECT_LOCK(self);
      g_value_set_uint(value, self->reset_settle_max_ms);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_RESET_REARM_FRAMES:
      GST_OBJECT_LOCK(self);
      g_value_set_uint(value, self->reset_rearm_frames);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_AUTO_PORT_RESET:
      GST_OBJECT_LOCK(self);
      g_value_set_boolean(value, self->auto_port_reset);
      GST_OBJECT_UNLOCK(self);
      break;
    case PROP_DELIVERABLE_CAPS:
      GST_OBJECT_LOCK(self);
      gst_value_set_caps(value, self->deliverable_caps);
      GST_OBJECT_UNLOCK(self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* "filter-deliverable-caps" action handler. Stateless: it reads the quirk table
 * for the caller's vid:pid and runs the caller's ladder through the SAME filter
 * negotiate() uses. `self` is only the emission target - no device is touched,
 * so this is safe to call per enumeration on an element that was never started. */
static GstCaps *gst_libuvc_h264_src_filter_deliverable_caps(GstLibuvcH264Src *self,
                                                            GstCaps *advertised,
                                                            guint vendor_id,
                                                            guint product_id) {
    g_return_val_if_fail(advertised != NULL, NULL);

    uvc_quirk_limits_t limits = {0};
    uvc_quirks_limits((guint16)vendor_id, (guint16)product_id, &limits);

    GstCaps *deliverable = uvc_quirks_filter_caps(&limits, advertised);
    GST_DEBUG_OBJECT(self,
        "filter-deliverable-caps %04x:%04x: %" GST_PTR_FORMAT
        " -> %" GST_PTR_FORMAT, vendor_id, product_id, advertised, deliverable);
    return deliverable;
}

/* "set-ptz" action handler: apply pan, tilt and zoom in one call. Each axis is
 * driven only when the device reports it; returns TRUE only if at least one
 * supported axis was driven and every attempted set succeeded. */
static gboolean gst_libuvc_h264_src_set_ptz(GstLibuvcH264Src *self,
                                            gint pan, gint tilt, gint zoom) {
  gboolean any = FALSE;
  gboolean ok = TRUE;

  if (self->pan_supported) {
    any = TRUE;
    ok = gst_libuvc_h264_src_ptz_set_pan(self, pan) && ok;
  }
  if (self->tilt_supported) {
    any = TRUE;
    ok = gst_libuvc_h264_src_ptz_set_tilt(self, tilt) && ok;
  }
  if (self->zoom_supported) {
    any = TRUE;
    ok = gst_libuvc_h264_src_ptz_set_zoom(self, zoom) && ok;
  }

  return any && ok;
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
    /* Rebaseline: re-latch base_time and prev_pts on the next frame at the new
       clock's running-time instead of clamping against a stale PTS. */
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;
  }

  GST_OBJECT_UNLOCK(self);

  return GST_ELEMENT_CLASS(gst_libuvc_h264_src_parent_class)->set_clock(element, clock);
}

/* On PAUSED->PLAYING the pipeline (re)assigns the element's base_time without
 * start() running (e.g. a pause/resume cycle that never passes through NULL), so
 * the cached self->base_time and running PTS would otherwise be stale. Reset both
 * latch sentinels here so the next frame re-latches the new running-time baseline
 * (base_time) and is not clamped against the old PTS (prev_pts) by the
 * frame_callback() monotonicity guard. Mirrors the set_clock() rebaseline. */
static GstStateChangeReturn
gst_libuvc_h264_src_change_state(GstElement *element, GstStateChange transition) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING) {
    GST_OBJECT_LOCK(self);
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;
    GST_OBJECT_UNLOCK(self);
  }

  return GST_ELEMENT_CLASS(gst_libuvc_h264_src_parent_class)->change_state(element, transition);
}

/* The `index` property selects ONE device from the libuvc enumeration. It stays
 * a string (cerastream passes a bare ordinal via `i.to_string()`), but now also
 * accepts richer, hardware-stable selectors. Parsed ONCE at start():
 *   "N"            ordinal into the enumerated list (UNCHANGED, the default)
 *   "vid:pid"      hex vendor:product id, e.g. "1234:5678"
 *   "serial:<sn>"  exact USB serial-number string
 *   "bus:<b>:<a>"  decimal USB bus number and device address */
typedef enum {
  UVC_SEL_ORDINAL,
  UVC_SEL_VID_PID,
  UVC_SEL_SERIAL,
  UVC_SEL_BUS_ADDR,
} GstLibuvcSelectorType;

typedef struct {
  GstLibuvcSelectorType type;
  long ordinal;
  guint16 vid, pid;
  const gchar *serial;   /* borrows the index string, not owned */
  guint8 bus, addr;
} GstLibuvcDeviceSelector;

/* Parse one integer token that MUST consume the whole string (no trailing junk,
 * no overflow, within [min,max]). base 10 or 16. Mirrors the Task-6 strtol
 * validation so the ordinal path is byte-for-byte as strict as before. */
static gboolean
gst_libuvc_h264_src_parse_uint(const gchar *s, int base, long min, long max,
                               long *out) {
  if (s == NULL || *s == '\0')
    return FALSE;
  errno = 0;
  char *end = NULL;
  long v = strtol(s, &end, base);
  if (end == s || *end != '\0' || errno != 0 || v < min || v > max)
    return FALSE;
  *out = v;
  return TRUE;
}

/* Parse the `index` property into a selector. Returns FALSE with a human-readable
 * reason in *errmsg on a malformed selector (caller maps it to RESOURCE/SETTINGS).
 * A bare, non-negative decimal is the ordinal — anything that is not one of the
 * three prefixed forms falls back to the ordinal parse and so still fails loudly
 * (the old atoi()-silently-selects-0 trap stays closed). */
static gboolean
gst_libuvc_h264_src_parse_selector(const gchar *index,
                                   GstLibuvcDeviceSelector *sel,
                                   const gchar **errmsg) {
  if (index == NULL) {
    *errmsg = "index is NULL";
    return FALSE;
  }

  if (g_str_has_prefix(index, "serial:")) {
    const gchar *sn = index + strlen("serial:");
    if (*sn == '\0') {
      *errmsg = "serial selector requires a non-empty serial number";
      return FALSE;
    }
    sel->type = UVC_SEL_SERIAL;
    sel->serial = sn;
    return TRUE;
  }

  if (g_str_has_prefix(index, "bus:")) {
    const gchar *rest = index + strlen("bus:");
    const gchar *colon = strchr(rest, ':');
    if (colon == NULL) {
      *errmsg = "bus selector requires \"bus:<bus>:<addr>\"";
      return FALSE;
    }
    gchar *bus_str = g_strndup(rest, (gsize)(colon - rest));
    long bus_v = 0, addr_v = 0;
    gboolean ok = gst_libuvc_h264_src_parse_uint(bus_str, 10, 0, 255, &bus_v) &&
                  gst_libuvc_h264_src_parse_uint(colon + 1, 10, 0, 255, &addr_v);
    g_free(bus_str);
    if (!ok) {
      *errmsg = "bus selector requires \"bus:<bus>:<addr>\" (decimal 0..255 each)";
      return FALSE;
    }
    sel->type = UVC_SEL_BUS_ADDR;
    sel->bus = (guint8)bus_v;
    sel->addr = (guint8)addr_v;
    return TRUE;
  }

  /* A colon with no recognised prefix is the hex vid:pid form. */
  const gchar *colon = strchr(index, ':');
  if (colon != NULL) {
    gchar *vid_str = g_strndup(index, (gsize)(colon - index));
    long vid_v = 0, pid_v = 0;
    gboolean ok = gst_libuvc_h264_src_parse_uint(vid_str, 16, 0, 0xFFFF, &vid_v) &&
                  gst_libuvc_h264_src_parse_uint(colon + 1, 16, 0, 0xFFFF, &pid_v);
    g_free(vid_str);
    if (!ok) {
      *errmsg = "vid:pid selector requires hex \"<vid>:<pid>\" (0000..ffff each)";
      return FALSE;
    }
    sel->type = UVC_SEL_VID_PID;
    sel->vid = (guint16)vid_v;
    sel->pid = (guint16)pid_v;
    return TRUE;
  }

  long ord = 0;
  if (!gst_libuvc_h264_src_parse_uint(index, 10, 0, INT_MAX, &ord)) {
    *errmsg = "index must be a non-negative integer ordinal, \"vid:pid\", "
              "\"serial:<sn>\", or \"bus:<bus>:<addr>\"";
    return FALSE;
  }
  sel->type = UVC_SEL_ORDINAL;
  sel->ordinal = ord;
  return TRUE;
}

/* Test one enumerated device against the parsed selector. `ordinal` is the
 * device's position in the libuvc list. vid:pid and serial reads go through the
 * libuvc descriptor (freed before returning); bus/addr read the cached topology.
 * A device whose descriptor cannot be read simply does not match. */
static gboolean
gst_libuvc_h264_src_selector_matches(const GstLibuvcDeviceSelector *sel,
                                     uvc_device_t *dev, int ordinal) {
  switch (sel->type) {
    case UVC_SEL_ORDINAL:
      return (long)ordinal == sel->ordinal;
    case UVC_SEL_VID_PID: {
      uvc_device_descriptor_t *desc = NULL;
      if (uvc_get_device_descriptor(dev, &desc) != UVC_SUCCESS || desc == NULL)
        return FALSE;
      gboolean ok = (desc->idVendor == sel->vid && desc->idProduct == sel->pid);
      uvc_free_device_descriptor(desc);
      return ok;
    }
    case UVC_SEL_SERIAL: {
      uvc_device_descriptor_t *desc = NULL;
      if (uvc_get_device_descriptor(dev, &desc) != UVC_SUCCESS || desc == NULL)
        return FALSE;
      gboolean ok = (desc->serialNumber != NULL &&
                     g_strcmp0(desc->serialNumber, sel->serial) == 0);
      uvc_free_device_descriptor(desc);
      return ok;
    }
    case UVC_SEL_BUS_ADDR:
      return (uvc_get_bus_number(dev) == sel->bus &&
              uvc_get_device_address(dev) == sel->addr);
  }
  return FALSE;
}

/* Resolve `selector` against a fresh enumeration, returning a REFERENCED device.
 *
 * With deadline == 0 this is the single-shot lookup start() and the reconnect
 * ladder have always done. With a nonzero deadline it keeps re-enumerating on a
 * micro-backoff until the device appears or the deadline passes: libuvc exposes
 * no "the device has finished re-enumerating" API, so a successful
 * uvc_find_devices() match IS that readiness signal after a port reset. Polling
 * for it is what lets the recovery cost only as long as the device actually
 * takes, instead of a fixed settle sized for the slowest device anyone measured.
 *
 * out_enum_failed distinguishes "no devices at all" from "none matched", so the
 * caller can keep posting the two different errors it always posted. */
static gboolean gst_libuvc_h264_src_resolve_device(
    GstLibuvcH264Src *self, const GstLibuvcDeviceSelector *selector,
    gint64 deadline, uvc_device_t **out_dev, long *out_ordinal,
    gboolean *out_enum_failed) {
  guint interval_ms = RESET_POLL_INITIAL_MS;
  gint attempt = 0;

  for (;;) {
    uvc_device_t **dev_list = NULL;
    gboolean enum_ok = (uvc_find_devices(self->uvc_ctx, &dev_list, 0, 0, NULL)
                        >= 0 && dev_list != NULL);
    if (enum_ok) {
      for (int i = 0; dev_list[i] != NULL; i++) {
        if (gst_libuvc_h264_src_selector_matches(selector, dev_list[i], i)) {
          /* The list free unrefs every entry, so take our own ref first. */
          uvc_ref_device(dev_list[i]);
          *out_dev = dev_list[i];
          if (out_ordinal != NULL) {
            *out_ordinal = i;
          }
          uvc_free_device_list(dev_list, 1);
          if (out_enum_failed != NULL) {
            *out_enum_failed = FALSE;
          }
          return TRUE;
        }
      }
      uvc_free_device_list(dev_list, 1);
    }
    if (out_enum_failed != NULL) {
      *out_enum_failed = !enum_ok;
    }

    if (deadline == 0 || g_get_monotonic_time() >= deadline) {
      return FALSE;
    }

    gint64 wait_us = (gint64) interval_ms * G_TIME_SPAN_MILLISECOND;
    if (gst_libuvc_reset_poll_hook != NULL) {
      wait_us = gst_libuvc_reset_poll_hook(self, attempt, interval_ms);
    }
    attempt++;
    interval_ms = MIN(interval_ms * 2, (guint) RESET_POLL_MAX_MS);
    if (!gst_libuvc_h264_src_interruptible_wait(self, wait_us)) {
      return FALSE;
    }
  }
}

static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  GST_DEBUG_OBJECT(self, "Starting libuvc source");

  // Check if we need to cleanup a previous session
  gboolean forced_cleanup = FALSE;
  if (self->uvc_ctx != NULL || self->uvc_devh != NULL) {
    GST_WARNING_OBJECT(self, "Previous session not fully cleaned up, forcing cleanup");
    gst_libuvc_h264_src_stop(src);
    forced_cleanup = TRUE;
  }

  // Reset per-session frame state so a restart never forwards stale non-IDR
  // frames (or a stale PTS baseline) before a fresh IDR re-establishes the
  // stream. had_idr/send_sps_pps gate NAL forwarding in frame_callback(), and
  // prev_pts/base_time use G_MAXUINT64 as the "latch on first frame" sentinel
  // that frame_callback() and create() test for.
  self->had_idr = FALSE;
  self->send_sps_pps = FALSE;
  self->frame_offset = 0;
  self->prev_pts = G_MAXUINT64;
  self->base_time = G_MAXUINT64;
  self->consecutive_timeouts = 0;
  self->reset_recovery_used = FALSE;
  self->frames_since_reset = 0;

  // Resolve the device selector up-front, before touching libuvc, so a
  // malformed index fails loudly here instead of silently selecting device 0.
  GstLibuvcDeviceSelector selector = {0};
  const gchar *parse_err = NULL;
  if (!gst_libuvc_h264_src_parse_selector(self->index, &selector, &parse_err)) {
    GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
        ("Invalid device index \"%s\"", self->index ? self->index : "(null)"),
        ("%s", parse_err));
    return FALSE;
  }
  long device_ordinal = -1;

  // Initialize libuvc context
  res = uvc_init(&self->uvc_ctx, NULL);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Failed to initialize libuvc: %s", uvc_strerror(res));
    return FALSE;
  }
  
  // A restart after a forced cleanup can race the bus: the previous session's
  // handle has only just closed, and the device may not be enumerable again for
  // a moment. Poll for it within the recovery budget rather than sleeping a
  // fixed second and hoping it was long enough. A NORMAL start stays single-shot,
  // so a genuinely absent device still fails immediately, exactly as before.
  gint64 resolve_deadline = 0;
  if (forced_cleanup) {
    guint budget_ms;
    GST_OBJECT_LOCK(self);
    budget_ms = self->reset_settle_max_ms;
    GST_OBJECT_UNLOCK(self);
    resolve_deadline = g_get_monotonic_time()
        + (gint64) budget_ms * G_TIME_SPAN_MILLISECOND;
  }

  gboolean enum_failed = FALSE;
  if (!gst_libuvc_h264_src_resolve_device(self, &selector, resolve_deadline,
                                          &self->uvc_dev, &device_ordinal,
                                          &enum_failed)) {
    if (enum_failed) {
      GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
          ("No UVC devices found"),
          ("uvc_find_devices found no enumerable UVC device"));
    } else {
      GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
          ("No UVC device matching \"%s\"", self->index ? self->index : "(null)"),
          ("selector matched none of the enumerated UVC devices"));
    }
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

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

  gst_libuvc_h264_src_v4l2_probe(GST_ELEMENT(self), (int)device_ordinal);

  // Probe PTZ ranges so only axes the device actually exposes are driven (M6).
  gst_libuvc_h264_src_ptz_probe_capabilities(self);

  // Opt-in control socket (M9): bind here BEFORE the thread so the listening fd
  // exists before any accept(); a bind failure is non-fatal to the media path.
  if (self->control_socket_enabled) {
    if (gst_libuvc_h264_src_control_socket_bind(self)) {
      /* Atomic so the control thread sees TRUE before it ever runs (g_thread_new
       * is the publish edge) and observes the stop() FALSE without a data race. */
      g_atomic_int_set(&self->control_running, TRUE);
      self->control_thread = g_thread_new("uvc-control",
                                          gst_libuvc_h264_src_control_thread,
                                          self);
    } else {
      GST_WARNING_OBJECT(self, "Control socket enabled but bind failed; "
                         "continuing without it");
    }
  }

  GST_DEBUG_OBJECT(self, "Libuvc source started successfully");
  return TRUE;
}

// Teardown: stop streaming, drain the frame queue, then let uvc_close() own the
// single libusb_close() (libuvc's native release path). Runs on every restart
// and is re-entered from start()'s cleanup path, so it must stay idempotent.
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Stopping libuvc source");

  // Stop control thread
  if (g_atomic_int_get(&self->control_running)) {
    GST_DEBUG_OBJECT(self, "Stopping control thread");
    g_atomic_int_set(&self->control_running, FALSE);

    // Nudge the thread out of its select() at once by self-connecting to the
    // bound path; the 1s select timeout is the fallback if this misses.
    if (self->control_socket_path != NULL) {
      int wakeup_fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (wakeup_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        g_strlcpy(addr.sun_path, self->control_socket_path, sizeof(addr.sun_path));
        fcntl(wakeup_fd, F_SETFL, O_NONBLOCK);
        connect(wakeup_fd, (struct sockaddr*)&addr, sizeof(addr));
        close(wakeup_fd);
      }
    }

    if (self->control_thread) {
      g_thread_join(self->control_thread);
      self->control_thread = NULL;
    }
  }

  // Close the listening fd and unlink the per-instance socket path.
  gst_libuvc_h264_src_control_socket_unbind(self);

  // Stop streaming. uvc_stop_streaming() is synchronous: it clears running,
  // broadcasts cb_cond, and pthread_join()s the callback thread before it
  // returns (Task 4 spike, reconnect-spike.md §2/§3), so no post-stop settle
  // delay is needed - the old usleep(100ms) here was unnecessary.
  if (self->streaming && self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Stopping UVC streaming");
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
  }

  // Clear frame queue
  if (self->frame_queue) {
    GstBuffer *buffer;
    while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
      gst_buffer_unref(buffer);
    }
  }

  // Let uvc_close() own the single libusb_close(). libuvc's native teardown
  // (uvc_close -> uvc_release_if -> libusb_release_interface, then exactly one
  // libusb_close) releases the interfaces and closes the handle once. Do NOT
  // call a bare libusb_close()/interface release before this: the Task 4 spike
  // (reconnect-spike.md §3) proved that double-closes the libusb handle (heap
  // corruption). This mirrors the reconnect path's native teardown.
  if (self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Closing UVC device handle");
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

  // Wake a reconnect backoff parked in g_cond_wait_until() (Task 7). The flag is
  // set before the broadcast and re-checked by the waiter under reconnect_lock,
  // so the broadcast is serialised by the lock and no wakeup is lost.
  g_mutex_lock(&self->reconnect_lock);
  g_cond_broadcast(&self->reconnect_cond);
  g_mutex_unlock(&self->reconnect_lock);

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

static GstLibuvcReconnectBackoffHook gst_libuvc_reconnect_backoff_hook = NULL;

void gst_libuvc_h264_src_set_reconnect_backoff_hook(
    GstLibuvcReconnectBackoffHook hook) {
  gst_libuvc_reconnect_backoff_hook = hook;
}

static GstLibuvcResetDeviceHook gst_libuvc_reset_device_hook = NULL;

void gst_libuvc_h264_src_set_reset_device_hook(GstLibuvcResetDeviceHook hook) {
  gst_libuvc_reset_device_hook = hook;
}

void gst_libuvc_h264_src_set_reset_poll_hook(GstLibuvcResetPollHook hook) {
  gst_libuvc_reset_poll_hook = hook;
}

/* Clear a WEDGED device: still enumerated, still answering every control
 * transfer (descriptors, probe/commit, PTZ), yet delivering nothing on the
 * streaming endpoint. Measured on a DJI Osmo Pocket 3 (2ca3:0023) after the
 * holding process died without uvc_close(): the interface's alternate setting is
 * never wound back, so the device stays armed for a host that is gone and
 * refuses to stream for the next one. Reproduced identically through libuvc AND
 * through the kernel uvcvideo driver, which is what proves it is device state
 * rather than anything in this element.
 *
 * A close/reopen does NOT clear it (measured). A USB port reset does, every
 * time. libusb_reset_device() issues exactly that, so the recovery needs no
 * sysfs write and no privilege this process does not already have.
 *
 * LIBUSB_ERROR_NOT_FOUND is a SUCCESS for our purpose: libusb returns it when
 * the reset re-enumerated the device, which is precisely the outcome we want.
 * It only means the caller must reopen from a fresh enumeration -- which is what
 * gst_libuvc_h264_src_reconnect() does. Any other status means the port reset
 * itself did not happen, so the device really is unreachable. */
gboolean gst_libuvc_h264_src_reset_silent_device(GstLibuvcH264Src *self) {
  gint rc;

  GST_INFO_OBJECT(self, "About to issue USB port reset for silent-but-present device");

  if (gst_libuvc_reset_device_hook != NULL) {
    rc = gst_libuvc_reset_device_hook(self);
  } else {
    if (self->uvc_devh == NULL) {
      return FALSE;
    }
    libusb_device_handle *usb_devh = uvc_get_libusb_handle(self->uvc_devh);
    if (usb_devh == NULL) {
      GST_DEBUG_OBJECT(self, "No libusb handle; cannot port-reset a silent device");
      return FALSE;
    }
    rc = libusb_reset_device(usb_devh);
  }

  if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
    GST_WARNING_OBJECT(self, "USB port reset failed (%d); device is unreachable", rc);
    return FALSE;
  }

  GST_INFO_OBJECT(self, "USB port reset issued on a silent-but-present device%s",
                  rc == LIBUSB_ERROR_NOT_FOUND ? " (re-enumerated)" : "");
  return TRUE;
}

/* Opt-in in-element reconnect (Task 18), gated on the Task 4 spike verdict.
 *
 * Runs on the streaming thread from inside create() after a sustained-silence
 * disconnect is detected, so it never races stop() (GstBaseSrc serialises them;
 * unlock() only sets the flushing flag). Tears the dead handle down with the
 * spike's verified NATIVE sequence and re-resolves the `index` selector against
 * a fresh enumeration (bus/address can change across a replug), then reopens and
 * restarts streaming with bounded exponential backoff. Returns TRUE once
 * streaming has resumed, FALSE if every retry was exhausted or a concurrent
 * unlock() asked us to bail.
 *
 * CRITICAL: tear the dead handle down with the native sequence ONLY
 * (uvc_stop_streaming()->uvc_close()->uvc_unref_device()); uvc_close() owns the
 * single libusb_close(). Never reintroduce a bare libusb_close()/interface
 * release before uvc_close() — the Task 4 spike proved that double-closes the
 * libusb handle. */
/* One reopen: resolve the selector, open, re-run the negotiated geometry,
 * re-arm the stream state and restart streaming. Shared verbatim by the
 * reconnect ladder (resolve_deadline 0, one shot per backoff interval) and by
 * the wedge recovery (resolve_deadline = its budget, so the resolve step polls
 * for re-enumeration). Returns TRUE only once uvc_start_streaming() succeeded -
 * which is NOT by itself proof the device is delivering; see
 * gst_libuvc_h264_src_await_first_frame().
 *
 * `deadline` (0 = none) bounds the WHOLE call, not just the resolve: libuvc's
 * open and format negotiation are synchronous USB control transfers with their
 * own timeouts, and on a device that is still re-enumerating they can each block
 * for seconds. Checking between every step caps the overrun at one such call
 * instead of letting a full reopen sequence run minutes past its budget. */
static gboolean gst_libuvc_h264_src_reopen_once(
    GstLibuvcH264Src *self, const GstLibuvcDeviceSelector *selector,
    gint64 deadline) {
  uvc_device_t *selected = NULL;
  if (!gst_libuvc_h264_src_resolve_device(self, selector, deadline,
                                          &selected, NULL, NULL)) {
    return FALSE;
  }

  if (deadline != 0 && g_get_monotonic_time() >= deadline) {
    uvc_unref_device(selected);
    return FALSE;
  }

  if (uvc_open(selected, &self->uvc_devh) < 0) {
    self->uvc_devh = NULL;
    uvc_unref_device(selected);
    return FALSE;
  }
  self->uvc_dev = selected;

  if ((deadline != 0 && g_get_monotonic_time() >= deadline)
      || uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
          self->frame_format, self->negotiated_width, self->negotiated_height,
          self->negotiated_framerate) < 0) {
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
    return FALSE;
  }

  // Re-apply the opt-in max-payload override idempotently (Task 12). A post-replug
  // link that came back as USB2 or a device that now refuses the value falls back
  // to the device-negotiated payload here rather than failing the reopen.
  gst_libuvc_h264_src_apply_max_payload(self, self->negotiated_width,
                                        self->negotiated_height,
                                        self->negotiated_framerate);

  // Re-arm the stream state BEFORE the feeder spawns so frame_callback sees the
  // reset (pthread_create in uvc_start_streaming is the happens-before edge):
  // re-latch the PTS baseline and re-engage the IDR gate after the gap.
  self->had_idr = FALSE;
  self->send_sps_pps = FALSE;
  self->base_time = G_MAXUINT64;
  self->prev_pts = G_MAXUINT64;

  // The reopened handle starts at the library default, so the opt-in
  // transfer-buffers count must be re-armed here like max-payload (Task 11).
  gst_libuvc_h264_src_apply_transfer_buffers(self);

  if ((deadline != 0 && g_get_monotonic_time() >= deadline)
      || uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl, frame_callback,
                             self, 0) < 0) {
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
    return FALSE;
  }

  self->streaming = TRUE;
  return TRUE;
}

/* Tear a live handle down with the spike's verified NATIVE sequence:
 * uvc_stop_streaming() -> uvc_close() -> uvc_unref_device(), where uvc_close()
 * owns the single libusb_close(). Never put force_usb_release() before
 * uvc_close() - the Task 4 spike proved that double-closes the libusb handle. */
static void gst_libuvc_h264_src_teardown_handle(GstLibuvcH264Src *self) {
  if (self->uvc_devh) {
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
  }
  if (self->uvc_dev) {
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
  }
}

/* Drop everything queued before a reopen so a resumed stream never forwards a
 * frame captured before the break (offset/PTS would be inconsistent). */
static void gst_libuvc_h264_src_drain_queue(GstLibuvcH264Src *self) {
  gpointer stale;
  while ((stale = g_async_queue_try_pop(self->frame_queue)) != NULL) {
    if (stale != FLUSH_SENTINEL) {
      gst_buffer_unref(stale);
    }
  }
}

static gboolean gst_libuvc_h264_src_reconnect_bounded(GstLibuvcH264Src *self,
                                                      gint max_retries) {
  GstLibuvcDeviceSelector selector = {0};
  const gchar *parse_err = NULL;
  if (!gst_libuvc_h264_src_parse_selector(self->index, &selector, &parse_err)) {
    GST_ERROR_OBJECT(self, "Reconnect: invalid index \"%s\": %s",
                     self->index ? self->index : "(null)", parse_err);
    return FALSE;
  }

  gst_libuvc_h264_src_teardown_handle(self);
  gst_libuvc_h264_src_drain_queue(self);

  guint backoff_s = RECONNECT_BACKOFF_INITIAL_S;
  for (int attempt = 0; attempt < max_retries; attempt++) {
    gint64 wait_us = (gint64) backoff_s * G_USEC_PER_SEC;
    if (gst_libuvc_reconnect_backoff_hook != NULL) {
      wait_us = gst_libuvc_reconnect_backoff_hook(self, attempt, backoff_s);
    }

    // Interruptible backoff, so unlock() wakes us at once on a NULL/PAUSED
    // transition instead of blocking for the full (up to 16 s) backoff window.
    if (!gst_libuvc_h264_src_interruptible_wait(self, wait_us)) {
      return FALSE;
    }

    GST_DEBUG_OBJECT(self, "Reconnect attempt %d/%d (after %u s backoff)",
                     attempt + 1, max_retries, backoff_s);
    backoff_s *= 2;

    if (!gst_libuvc_h264_src_reopen_once(self, &selector, 0)) {
      continue;
    }

    GST_INFO_OBJECT(self, "Reconnect succeeded on attempt %d", attempt + 1);
    return TRUE;
  }

  GST_WARNING_OBJECT(self, "Reconnect exhausted after %d attempts",
                     max_retries);
  return FALSE;
}

gboolean gst_libuvc_h264_src_reconnect(GstLibuvcH264Src *self) {
  return gst_libuvc_h264_src_reconnect_bounded(self, RECONNECT_MAX_RETRIES);
}

/* EMPIRICAL readiness. libuvc offers no way to ask whether a streaming endpoint
 * is live, and a successful uvc_start_streaming() provably lies about it: a
 * reopen issued too soon after a port reset returns OK from both uvc_open() and
 * uvc_start_streaming() and then delivers nothing, which is indistinguishable
 * from the wedge the reset was meant to clear. A DELIVERED FRAME is the only
 * trustworthy signal, so wait for one - and hand it back to create() via
 * push_front rather than dropping it, so proving recovery costs no video. */
static gboolean gst_libuvc_h264_src_await_first_frame(GstLibuvcH264Src *self,
                                                      gint64 wait_us) {
  gint64 deadline = g_get_monotonic_time() + wait_us;

  for (;;) {
    gint64 remaining = deadline - g_get_monotonic_time();
    if (remaining <= 0) {
      return FALSE;
    }

    gpointer item = g_async_queue_timeout_pop(self->frame_queue, remaining);
    if (item == NULL) {
      return FALSE;
    }
    if (g_atomic_int_get(&self->flushing)) {
      if (item != FLUSH_SENTINEL) {
        gst_buffer_unref(item);
      }
      return FALSE;
    }
    if (item == FLUSH_SENTINEL) {
      continue;
    }

    g_async_queue_push_front(self->frame_queue, item);
    return TRUE;
  }
}

/* Recover a WEDGED device: still enumerated, still answering every control
 * transfer, delivering nothing. ONE USB port reset per silence episode (the
 * one-shot the caller arms), then a READINESS-DRIVEN return to streaming.
 *
 * The reset is the only part with a fixed shape. Everything after it is
 * measured against the device rather than against a clock: poll until it
 * re-enumerates, reopen, restart, and then require an ACTUAL frame. If the
 * reopen came too early - clean start, no video - tear it down and try again
 * while the budget lasts. The recovery therefore ends the moment frames are
 * really flowing, which on a fast device is far sooner than any fixed settle
 * would have allowed, and on a slow one is still correct.
 *
 * `reset-settle-max-ms` bounds THIS loop - the polling, the reopen retries and
 * the frame wait - so an unrecoverable device costs a bounded number of attempts
 * and then fails through to the usual disconnect error. It does NOT bound the
 * teardown between attempts: uvc_stop_streaming() joins the callback thread and
 * uvc_close() talks to a device that may have just been reset away, both
 * synchronously and with no way to interrupt them. MEASURED: 21880 ms and
 * 21893 ms total against an 8000 ms budget, on two independent runs. Making that
 * real needs a non-blocking teardown, which is a separate change.
 *
 * The port reset itself is never repeated here: retries are reopens only, so
 * the one-shot guarantee the caller relies on is preserved exactly. */
static gboolean gst_libuvc_h264_src_recover_wedged_device(
    GstLibuvcH264Src *self) {
  GstLibuvcDeviceSelector selector = {0};
  const gchar *parse_err = NULL;
  if (!gst_libuvc_h264_src_parse_selector(self->index, &selector, &parse_err)) {
    GST_ERROR_OBJECT(self, "Wedge recovery: invalid index \"%s\": %s",
                     self->index ? self->index : "(null)", parse_err);
    return FALSE;
  }

  if (!gst_libuvc_h264_src_reset_silent_device(self)) {
    return FALSE;
  }

  guint budget_ms;
  GST_OBJECT_LOCK(self);
  budget_ms = self->reset_settle_max_ms;
  GST_OBJECT_UNLOCK(self);

  gint64 started = g_get_monotonic_time();
  gint64 deadline = started + (gint64) budget_ms * G_TIME_SPAN_MILLISECOND;

  // The reset invalidated the open handle; drop it before re-enumerating.
  gst_libuvc_h264_src_teardown_handle(self);
  gst_libuvc_h264_src_drain_queue(self);

  // ...and the libuvc context with it. MEASURED on hardware: after a real port
  // reset the device re-enumerates, and the context that was open ACROSS that
  // reset can no longer open it - a freshly started process was streaming again
  // 14.4 s in while this element, still holding its original context, could not
  // reopen the device at all inside a 30 s budget. Re-initialising is what makes
  // the recovery equivalent to the fresh process that demonstrably works. If the
  // re-init fails there is nothing left to enumerate with, so give up here.
  uvc_exit(self->uvc_ctx);
  self->uvc_ctx = NULL;
  if (uvc_init(&self->uvc_ctx, NULL) < 0) {
    self->uvc_ctx = NULL;
    GST_WARNING_OBJECT(self, "Wedge recovery: could not re-init libuvc");
    return FALSE;
  }

  gint attempt = 0;
  guint retry_ms = RESET_POLL_INITIAL_MS;
  while (!g_atomic_int_get(&self->flushing)
         && g_get_monotonic_time() < deadline) {
    if (!gst_libuvc_h264_src_reopen_once(self, &selector, deadline)) {
      // MEASURED on hardware: the first reopen after a real port reset routinely
      // fails - the device is listed again but still refuses uvc_open() or the
      // format negotiation while it finishes coming back. Inside the budget that
      // is a device in transit, NOT a verdict, so back off and try again. Ending
      // the recovery here would make the budget meaningless: it once gave up
      // after 6.9 s of a 30 s budget on a single failed reopen.
      GST_DEBUG_OBJECT(self,
          "Wedge recovery: device not reopenable yet; retrying in %u ms",
          retry_ms);
      if (!gst_libuvc_h264_src_interruptible_wait(self,
              (gint64) retry_ms * G_TIME_SPAN_MILLISECOND)) {
        break;
      }
      retry_ms = MIN(retry_ms * 2, (guint) RESET_POLL_MAX_MS);
      continue;
    }
    attempt++;

    gint64 remaining = deadline - g_get_monotonic_time();
    gint64 proof_us = MIN(remaining, (gint64) TIMEOUT_DURATION);
    if (proof_us > 0
        && gst_libuvc_h264_src_await_first_frame(self, proof_us)) {
      GST_INFO_OBJECT(self,
          "Wedge recovery: frames advancing again %" G_GINT64_FORMAT
          " ms after the port reset (reopen attempt %d)",
          (g_get_monotonic_time() - started) / G_TIME_SPAN_MILLISECOND,
          attempt);
      return TRUE;
    }

    // Started clean and delivered nothing, so the device is not back yet. Tear
    // the handle down and try again rather than trusting the successful start.
    GST_DEBUG_OBJECT(self,
        "Wedge recovery: reopen attempt %d started but delivered no frame",
        attempt);
    gst_libuvc_h264_src_teardown_handle(self);
    gst_libuvc_h264_src_drain_queue(self);
  }

  GST_WARNING_OBJECT(self,
      "Wedge recovery gave up after %" G_GINT64_FORMAT " ms (budget %u ms, "
      "%d reopen attempt(s)); the device never delivered a frame. An overrun "
      "past the budget is one synchronous libuvc call that was already in "
      "flight when the budget expired",
      (g_get_monotonic_time() - started) / G_TIME_SPAN_MILLISECOND, budget_ms,
      attempt);
  return FALSE;
}

static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  if (!self->streaming) {
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;

    self->streaming = TRUE;

    // Apply the opt-in transfer-buffers override right before streaming starts
    // (the fork rejects it mid-stream); unset leaves libuvc's default count
    // unchanged (Task 11).
    gst_libuvc_h264_src_apply_transfer_buffers(self);

    // Start streaming
    res = uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl, frame_callback, self, 0);
    if (res < 0) {
      // Task 12 graceful fallback: a start failure attributable to a max-payload
      // override (e.g. LIBUSB_ERROR_NO_MEM exhausting a constrained DMA pool, or
      // UVC_ERROR_INVALID_MODE on a slower link) must not fail the media path.
      // Revert to the device-negotiated payload via a clean re-negotiation and
      // retry the start once before surfacing an error.
      guint requested;
      GST_OBJECT_LOCK(self);
      requested = self->max_payload;
      GST_OBJECT_UNLOCK(self);
      if (requested > 0 &&
          gst_libuvc_h264_src_negotiate_clean_payload(self,
              self->negotiated_width, self->negotiated_height,
              self->negotiated_framerate)) {
        GST_WARNING_OBJECT(self,
            "stream start failed with max-payload override (%s); retrying with "
            "device-negotiated payload", uvc_strerror(res));
        res = uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl,
                                  frame_callback, self, 0);
      }
      if (res < 0) {
        self->streaming = FALSE;
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res,
            "starting UVC stream");
        return GST_FLOW_ERROR;
      }
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

    if (item == NULL) {
      // A real pop timeout. libuvc delivers no NULL frame on unplug in callback
      // mode (it just goes silent, per the Task 4 spike), so sustained silence
      // is how a disconnect is detected. Count consecutive timeouts; a single
      // gap is tolerated, but DISCONNECT_TIMEOUT_COUNT in a row means the device
      // is gone.
      if (++self->consecutive_timeouts < DISCONNECT_TIMEOUT_COUNT) {
        continue;
      }

      GST_WARNING_OBJECT(self, "Device silent for %d s, assuming disconnect",
                         DISCONNECT_TIMEOUT_COUNT);

      // Opt-in reconnect: try to resume before erroring. Default off, so a
      // disconnect always surfaces as a RESOURCE/READ error downstream.
      if (self->reconnect_enabled) {
        if (gst_libuvc_h264_src_reconnect(self)) {
          self->consecutive_timeouts = 0;
          continue;
        }
      } else if (!self->reset_recovery_used) {
        gboolean auto_port_reset;
        GST_OBJECT_LOCK(self);
        auto_port_reset = self->auto_port_reset;
        GST_OBJECT_UNLOCK(self);
        if (!auto_port_reset) {
          GST_INFO_OBJECT(self,
                          "Automatic USB port reset disabled; using normal "
                          "disconnect handling");
        } else {
        // Sustained silence has TWO indistinguishable causes from here: the
        // device was unplugged, or it is still present but WEDGED (see
        // gst_libuvc_h264_src_reset_silent_device). With reconnect off we still
        // owe the wedged case ONE port reset plus a single reopen, because
        // "the device was removed" is factually wrong for a device that is still
        // on the bus — and a reset is the only thing that recovers it. A
        // genuinely absent device fails the reset, so this costs an unplugged
        // device nothing and cannot mask a real disconnect. Deliberately ONE
        // NOT the opt-in path's retry ladder: `reconnect` stays the property
        // that buys 1/2/4/8/16 s. The recovery below is bounded by TIME
        // (`reset-settle-max-ms`) and ends on delivered frames, not attempts.
        self->reset_recovery_used = TRUE;
        self->frames_since_reset = 0;
        if (gst_libuvc_h264_src_recover_wedged_device(self)) {
          GST_INFO_OBJECT(self, "Recovered a wedged device via USB port reset");
          self->consecutive_timeouts = 0;
          continue;
        }
        }
      }

      // A flush raced in during the reconnect backoff: honour it over the error.
      if (g_atomic_int_get(&self->flushing)) {
        return GST_FLOW_FLUSHING;
      }

      gst_libuvc_h264_src_post_disconnect_error(GST_ELEMENT(self));
      return GST_FLOW_ERROR;
    }

    if (item == FLUSH_SENTINEL) {
      // A stale sentinel from a finished flush: not silence, so reset the
      // disconnect counter and keep waiting.
      self->consecutive_timeouts = 0;
      continue;
    }

    // A real frame arrived: silence is broken. Re-arm the one-shot reset only
    // once the device has PROVEN it recovered, so a LATER wedge is recoverable
    // (without this the recovery would fire at most once per session) but a
    // device that emits one frame and re-wedges cannot reset the port forever.
    self->consecutive_timeouts = 0;
    if (self->reset_recovery_used) {
      guint rearm_frames;
      GST_OBJECT_LOCK(self);
      rearm_frames = self->reset_rearm_frames;
      GST_OBJECT_UNLOCK(self);
      if (++self->frames_since_reset >= rearm_frames) {
        self->reset_recovery_used = FALSE;
      }
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

    gst_caps_replace(&self->deliverable_caps, NULL);

    // stop() above already unlinked the socket; free the owned path string.
    if (self->control_socket_path) {
        g_free(self->control_socket_path);
        self->control_socket_path = NULL;
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
    g_cond_clear(&self->reconnect_cond);
    g_mutex_clear(&self->reconnect_lock);

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
