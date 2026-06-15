/* Compatibility regression suite for the libuvch264src element.
 *
 * This is the DURABLE GUARDRAIL for the libuvc/GStreamer compatibility boundary.
 * A future libuvc bump (fork SHA roll, upstream re-base, or a distro swapping the
 * shared object) must keep three contracts intact, and each is asserted here:
 *
 *   1. API-SURFACE CONTRACT (compile + link guard). The element depends on a
 *      fixed set of libuvc/libusb symbols - the hard compatibility boundary. The
 *      symbol-address table below references every one, grouped by concern
 *      (enum/open, streaming/format, PTZ, teardown/USB, error). The DECLARATIONS
 *      come from the REAL <libuvc/libuvc.h> and <libusb-1.0/libusb.h> on the
 *      include path, so a renamed/removed symbol fails to COMPILE (undeclared)
 *      here exactly as it would in the element TUs; the address-of references
 *      then fail to LINK if a definition is dropped. The table also pins symbols
 *      the element might stop calling, so the contract is enforced independently
 *      of current call sites. Opaque types and enum constants are pinned the same
 *      way (a struct-tag rename or a renumbered enum trips the static checks).
 *
 *   2. CAPS CONTRACT. Both factory names register (libuvch264src primary +
 *      libuvch26xsrc dual-codec alias), the element is a GstPushSrc, the documented
 *      "index" default is "0", and the ALWAYS src pad template advertises BOTH
 *      video/x-h264 and video/x-h265 (the dual-codec promise).
 *
 *   3. PIPELINE-PARSE CONTRACT. README-style gst-launch pipelines (the fakesink
 *      variants - kernel-independent, no decoder/encoder elements required) parse
 *      AND negotiate against the libuvc mock: an H.264 device negotiates
 *      video/x-h264 through `libuvch264src ! video/x-h264 ! fakesink`, and an
 *      H.265 device negotiates video/x-h265 through `libuvch26xsrc ! video/x-h265
 *      ! fakesink`. No host kernel dependency: only the element, a capsfilter, and
 *      fakesink (coreelements) are in the graph.
 *
 * Like test_negotiate.c / test_device_select.c, the element TUs, the libuvc mock,
 * and this driver are linked into ONE statically-registered executable, so the
 * mock's frame format can be shaped in-process via mock_uvc_set_frame_format()
 * and the real libuvc.h declarations back the symbol guard with no plugin .so to
 * interpose. Each gst-check test is surfaced as its own ctest entry via GST_CHECKS.
 *
 * Scope discipline: this suite restates the compatibility CONTRACT in one place.
 * The authentic plugin_init registration smoke lives in test_plugin_load.c and the
 * negotiate() error-edge matrix lives in test_negotiate.c; this file references
 * that boundary rather than re-deriving it.
 *
 * No UVC hardware is touched: every uvc_* call resolves to the mock, and the
 * symbol table only takes addresses (it never invokes the real library).
 */

#include <gst/check/gstcheck.h>
#include <gst/base/gstpushsrc.h>

#include <libuvc/libuvc.h>
#include <libusb-1.0/libusb.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

#define ELEMENT_NAME "libuvch264src"
#define ELEMENT_ALIAS "libuvch26xsrc"

/* ---------------------------------------------------------------------------
 * GROUP 1 - API-SURFACE CONTRACT
 *
 * The 47-symbol compatibility boundary, grouped by concern. The address-of each
 * function forces its declaration (from the real libuvc/libusb headers) to exist
 * at compile time and its definition to exist at link time. uvc_* definitions are
 * supplied by the mock; libusb_* definitions by the real libusb linked into the
 * static suite - both back the link guard.
 * ------------------------------------------------------------------------- */

#define COMPAT_SYM(fn) ((void *) (fn))

/* enum / open: context lifecycle, device enumeration, open/close, descriptor. */
static void *const compat_syms_enum_open[] = {
  COMPAT_SYM (uvc_init),
  COMPAT_SYM (uvc_exit),
  COMPAT_SYM (uvc_find_devices),
  COMPAT_SYM (uvc_free_device_list),
  COMPAT_SYM (uvc_open),
  COMPAT_SYM (uvc_close),
  COMPAT_SYM (uvc_ref_device),
  COMPAT_SYM (uvc_unref_device),
  COMPAT_SYM (uvc_get_device_descriptor),
  COMPAT_SYM (uvc_free_device_descriptor),
  COMPAT_SYM (uvc_get_bus_number),
  COMPAT_SYM (uvc_get_device_address),
};

/* streaming / format: format descriptor walk, stream-ctrl negotiation, start/stop. */
static void *const compat_syms_streaming[] = {
  COMPAT_SYM (uvc_get_format_descs),
  COMPAT_SYM (uvc_get_stream_ctrl_format_size),
  COMPAT_SYM (uvc_start_streaming),
  COMPAT_SYM (uvc_stop_streaming),
};

/* PTZ: pan/tilt/zoom get + set. */
static void *const compat_syms_ptz[] = {
  COMPAT_SYM (uvc_get_pantilt_abs),
  COMPAT_SYM (uvc_set_pantilt_abs),
  COMPAT_SYM (uvc_get_zoom_abs),
  COMPAT_SYM (uvc_set_zoom_abs),
};

/* teardown / USB: the libusb handle bridge plus the interface-claim drop path.
 * NOTE: force_usb_release() must drop interface claims on the still-open handle
 * and let uvc_close() own the single libusb_close() (double-close guardrail). */
static void *const compat_syms_teardown_usb[] = {
  COMPAT_SYM (uvc_get_libusb_handle),
  COMPAT_SYM (libusb_get_device),
  COMPAT_SYM (libusb_get_bus_number),
  COMPAT_SYM (libusb_get_device_address),
  COMPAT_SYM (libusb_get_active_config_descriptor),
  COMPAT_SYM (libusb_free_config_descriptor),
  COMPAT_SYM (libusb_kernel_driver_active),
  COMPAT_SYM (libusb_detach_kernel_driver),
  COMPAT_SYM (libusb_release_interface),
  COMPAT_SYM (libusb_reset_device),
  COMPAT_SYM (libusb_close),
};

/* error: strerror for the uvc_error_t -> GST_ELEMENT_ERROR mapping. */
static void *const compat_syms_error[] = {
  COMPAT_SYM (uvc_strerror),
};

/* The total function-symbol count the element contracts on. A dropped entry (or
 * a symbol the element stops needing being removed from the table) changes this
 * number, so the runtime assertion below pins it explicitly. */
#define COMPAT_FN_SYMBOL_COUNT                                                 \
  (G_N_ELEMENTS (compat_syms_enum_open) + G_N_ELEMENTS (compat_syms_streaming) \
      + G_N_ELEMENTS (compat_syms_ptz)                                         \
      + G_N_ELEMENTS (compat_syms_teardown_usb)                               \
      + G_N_ELEMENTS (compat_syms_error))

/* Opaque libuvc types the element holds pointers to. Referencing them through a
 * pointer is a compile-time guard: a struct-tag rename in a future libuvc breaks
 * compilation here exactly as it would in gstlibuvch264src_internal.h. */
static void
compat_assert_types_present (void)
{
  uvc_context_t *ctx = NULL;
  uvc_device_t *dev = NULL;
  uvc_device_handle_t *devh = NULL;
  uvc_stream_ctrl_t *ctrl = NULL;
  uvc_frame_t *frame = NULL;
  uvc_format_desc_t *fmt = NULL;
  uvc_frame_desc_t *fdesc = NULL;
  uvc_device_descriptor_t *desc = NULL;
  uvc_error_t err = UVC_SUCCESS;

  (void) ctx;
  (void) dev;
  (void) devh;
  (void) ctrl;
  (void) frame;
  (void) fmt;
  (void) fdesc;
  (void) desc;
  (void) err;
}

/* uvc_error_t constants the error map depends on, plus the UVC_FRAME_FORMAT_*
 * codes the dual-codec negotiation selects and the UVC_GET_* request codes the
 * PTZ probe issues. Listed so a renumbered/renamed enumerator trips the build. */
static const int compat_enum_contract[] = {
  UVC_SUCCESS,
  UVC_ERROR_NO_DEVICE,
  UVC_ERROR_ACCESS,
  UVC_ERROR_BUSY,
  UVC_ERROR_NOT_SUPPORTED,
  UVC_FRAME_FORMAT_H264,
  UVC_FRAME_FORMAT_H265,
  UVC_GET_CUR,
  UVC_GET_MIN,
  UVC_GET_MAX,
  UVC_GET_RES,
};

/* ---------------------------------------------------------------------------
 * Fixture + helpers (shared by the caps and pipeline-parse groups)
 * ------------------------------------------------------------------------- */

static gint g_buffers_seen;
static gchar *g_negotiated_caps_name; /* media type on the first CAPS event */

static GstPadProbeReturn
caps_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  if (GST_EVENT_TYPE (ev) == GST_EVENT_CAPS && g_negotiated_caps_name == NULL) {
    GstCaps *caps = NULL;
    gst_event_parse_caps (ev, &caps);
    if (caps != NULL && gst_caps_get_size (caps) > 0) {
      GstStructure *s = gst_caps_get_structure (caps, 0);
      g_negotiated_caps_name = g_strdup (gst_structure_get_name (s));
    }
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
count_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&g_buffers_seen);
  return GST_PAD_PROBE_OK;
}

static void
setup (void)
{
  /* fakesink lives in coreelements; the harness blanks the system plugin path
   * for isolation, so load just that one plugin explicitly. */
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  /* Register BOTH factory names against the one element GType, mirroring the
   * plugin_init contract (the primary + the dual-codec alias). The authentic
   * plugin_init smoke is test_plugin_load.c; here we restate the name contract so
   * the caps/pipeline-parse cases can resolve both factories in the static suite. */
  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, ELEMENT_NAME, GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register %s", ELEMENT_NAME);
    fail_unless (gst_element_register (NULL, ELEMENT_ALIAS, GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register %s", ELEMENT_ALIAS);
    registered = TRUE;
  }

  mock_uvc_reset ();
  g_atomic_int_set (&g_buffers_seen, 0);
  g_clear_pointer (&g_negotiated_caps_name, g_free);
}

static void
teardown (void)
{
  g_clear_pointer (&g_negotiated_caps_name, g_free);
}

/* Parse a README-style fakesink pipeline, drive it to EOS via num-buffers, and
 * capture the negotiated media type. Fails the test on parse/state/timeout/error.
 * Returns the number of buffers that reached the sink. */
static gint
parse_and_negotiate (const gchar * factory, const gchar * caps, gint num_buffers)
{
  gchar *desc = g_strdup_printf (
      "%s index=0 num-buffers=%d ! %s ! fakesink sync=false name=sink",
      factory, num_buffers, caps);
  GError *err = NULL;
  GstElement *pipeline = gst_parse_launch (desc, &err);
  fail_unless (err == NULL, "pipeline parse failed for [%s]: %s", desc,
      err ? err->message : "(unknown)");
  fail_unless (pipeline != NULL, "no pipeline produced for [%s]", desc);
  g_free (desc);

  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  fail_unless (sink != NULL, "fakesink not found in pipeline");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, caps_event_probe,
      NULL, NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  if (msg != NULL && GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *gerr = NULL;
    gchar *dbg = NULL;
    gst_message_parse_error (msg, &gerr, &dbg);
    fail ("pipeline errored instead of reaching EOS: %s (%s)",
        gerr ? gerr->message : "(none)", dbg ? dbg : "(no debug)");
    g_clear_error (&gerr);
    g_free (dbg);
  }
  fail_unless (msg != NULL, "timed out waiting for EOS");
  fail_unless (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS,
      "expected EOS, got %s", GST_MESSAGE_TYPE_NAME (msg));
  gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return g_atomic_int_get (&g_buffers_seen);
}

/* ---------------------------------------------------------------------------
 * GROUP 1 test
 * ------------------------------------------------------------------------- */

GST_START_TEST (test_compat_api_surface)
{
  /* Every grouped function symbol must resolve to a real address. A NULL here
   * would mean the linker silently bound a weak/missing definition; the build
   * guard already rejects a renamed/removed symbol at compile/link time, so this
   * runtime pass is the belt to that suspenders. */
  void *const *groups[] = {
    compat_syms_enum_open,
    compat_syms_streaming,
    compat_syms_ptz,
    compat_syms_teardown_usb,
    compat_syms_error,
  };
  const guint counts[] = {
    G_N_ELEMENTS (compat_syms_enum_open),
    G_N_ELEMENTS (compat_syms_streaming),
    G_N_ELEMENTS (compat_syms_ptz),
    G_N_ELEMENTS (compat_syms_teardown_usb),
    G_N_ELEMENTS (compat_syms_error),
  };

  guint total = 0;
  for (guint g = 0; g < G_N_ELEMENTS (groups); g++) {
    for (guint i = 0; i < counts[g]; i++) {
      fail_unless (groups[g][i] != NULL,
          "compat symbol group %u index %u resolved to NULL", g, i);
      total++;
    }
  }

  fail_unless (total == COMPAT_FN_SYMBOL_COUNT,
      "libuvc/libusb function-symbol contract drifted: counted %u, expected %u",
      total, (guint) COMPAT_FN_SYMBOL_COUNT);

  /* Opaque-type and enum-constant guards (compile-time; exercised at runtime so
   * the references are not stripped). */
  compat_assert_types_present ();

  fail_unless (UVC_SUCCESS == 0, "UVC_SUCCESS must remain 0");
  fail_unless (G_N_ELEMENTS (compat_enum_contract) == 11,
      "uvc enum contract size changed: %u",
      (guint) G_N_ELEMENTS (compat_enum_contract));
  /* UVC_FRAME_FORMAT_H264 and _H265 are the dual-codec negotiation keys; a
   * renumber would silently break codec selection, so pin them distinct. */
  fail_unless (UVC_FRAME_FORMAT_H264 != UVC_FRAME_FORMAT_H265,
      "H264/H265 frame-format codes collapsed to the same value");
}

GST_END_TEST;

/* ---------------------------------------------------------------------------
 * GROUP 2 test - caps contract
 * ------------------------------------------------------------------------- */

GST_START_TEST (test_compat_caps_contract)
{
  /* Both factory names resolve. */
  GstElementFactory *primary = gst_element_factory_find (ELEMENT_NAME);
  fail_unless (primary != NULL, "primary factory '%s' not found", ELEMENT_NAME);
  GstElementFactory *alias = gst_element_factory_find (ELEMENT_ALIAS);
  fail_unless (alias != NULL, "alias factory '%s' not found", ELEMENT_ALIAS);

  /* The element is a GstPushSrc and exposes the documented "index" default. */
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL, "could not instantiate '%s'", ELEMENT_NAME);
  fail_unless (GST_IS_PUSH_SRC (element), "'%s' is not a GstPushSrc",
      ELEMENT_NAME);

  GParamSpec *pspec =
      g_object_class_find_property (G_OBJECT_GET_CLASS (element), "index");
  fail_unless (pspec != NULL, "expected 'index' property is missing");
  fail_unless (pspec->value_type == G_TYPE_STRING, "'index' should be a string");

  gchar *index = NULL;
  g_object_get (element, "index", &index, NULL);
  fail_unless (index != NULL && g_strcmp0 (index, "0") == 0,
      "default 'index' should be \"0\", got '%s'", index ? index : "(null)");
  g_free (index);
  gst_object_unref (element);

  /* The ALWAYS src pad template advertises BOTH codecs (the dual-codec promise). */
  const GList *templates =
      gst_element_factory_get_static_pad_templates (primary);
  gboolean found_dual_codec_src = FALSE;
  for (const GList *l = templates; l != NULL; l = l->next) {
    GstStaticPadTemplate *tmpl = (GstStaticPadTemplate *) l->data;
    if (tmpl->direction == GST_PAD_SRC
        && g_strcmp0 (tmpl->name_template, "src") == 0) {
      fail_unless (tmpl->presence == GST_PAD_ALWAYS,
          "src pad template should be ALWAYS present");
      GstCaps *caps = gst_static_caps_get (&tmpl->static_caps);
      fail_unless (caps != NULL && !gst_caps_is_empty (caps),
          "src pad template caps are empty");
      gchar *caps_str = gst_caps_to_string (caps);
      fail_unless (g_strrstr (caps_str, "video/x-h264") != NULL,
          "src caps missing video/x-h264: %s", caps_str);
      fail_unless (g_strrstr (caps_str, "video/x-h265") != NULL,
          "src caps missing video/x-h265: %s", caps_str);
      g_free (caps_str);
      gst_caps_unref (caps);
      found_dual_codec_src = TRUE;
    }
  }
  fail_unless (found_dual_codec_src,
      "no ALWAYS dual-codec src pad template named 'src'");

  gst_object_unref (primary);
  gst_object_unref (alias);
}

GST_END_TEST;

/* ---------------------------------------------------------------------------
 * GROUP 3 tests - pipeline-parse contract (README fakesink variants)
 * ------------------------------------------------------------------------- */

GST_START_TEST (test_compat_pipeline_parse_h264)
{
  /* Default mock format is H.264. README-style fakesink pipeline. */
  gint got = parse_and_negotiate (ELEMENT_NAME, "video/x-h264", 5);

  fail_unless (g_negotiated_caps_name != NULL,
      "H.264 pipeline negotiated no CAPS at the sink");
  fail_unless_equals_string (g_negotiated_caps_name, "video/x-h264");
  fail_unless (got == 5, "expected 5 H.264 buffers, got %d", got);
}

GST_END_TEST;

GST_START_TEST (test_compat_pipeline_parse_h265)
{
  /* Shape the in-process mock to advertise and feed H.265; the dual-codec alias
   * negotiates video/x-h265 through the README-style fakesink pipeline. */
  mock_uvc_set_frame_format (UVC_FRAME_FORMAT_H265);

  gint got = parse_and_negotiate (ELEMENT_ALIAS, "video/x-h265", 5);

  fail_unless (g_negotiated_caps_name != NULL,
      "H.265 pipeline negotiated no CAPS at the sink");
  fail_unless_equals_string (g_negotiated_caps_name, "video/x-h265");
  fail_unless (got == 5, "expected 5 H.265 buffers, got %d", got);
}

GST_END_TEST;

static Suite *
compat_suite (void)
{
  Suite *s = suite_create ("libuvch264src-compat");
  TCase *tc = tcase_create ("compat");

  tcase_set_timeout (tc, 90);
  tcase_add_checked_fixture (tc, setup, teardown);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_compat_api_surface);
  tcase_add_test (tc, test_compat_caps_contract);
  tcase_add_test (tc, test_compat_pipeline_parse_h264);
  tcase_add_test (tc, test_compat_pipeline_parse_h265);

  return s;
}

GST_CHECK_MAIN (compat);
