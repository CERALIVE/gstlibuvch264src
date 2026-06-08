/* Hardware-independent plugin-load smoke test for the libuvch264src element.
 *
 * This deliberately exercises ONLY the registration / introspection surface:
 *   - the plugin module loads and registers in the GStreamer registry,
 *   - both element factories (primary + alias) exist,
 *   - the element instantiates as a GstPushSrc,
 *   - the documented "index" property is present with its default,
 *   - the ALWAYS "src" pad template advertises H.264 caps.
 *
 * No UVC device is opened: gst_element_factory_make() only runs class/instance
 * init; the camera is only touched on the READY->PAUSED start() transition,
 * which this test never triggers. Safe to run on CI without capture hardware.
 */

#include <gst/check/gstcheck.h>
#include <gst/base/gstpushsrc.h>

#define PLUGIN_NAME "libuvch264src"
#define ELEMENT_NAME "libuvch264src"
#define ELEMENT_ALIAS "libuvch26xsrc"

GST_START_TEST (test_plugin_is_registered)
{
  GstPlugin *plugin =
      gst_registry_find_plugin (gst_registry_get (), PLUGIN_NAME);
  fail_unless (plugin != NULL,
      "GStreamer plugin '%s' is not registered - is GST_PLUGIN_PATH pointing "
      "at the freshly built module directory?", PLUGIN_NAME);
  gst_object_unref (plugin);
}

GST_END_TEST;

GST_START_TEST (test_element_factories_exist)
{
  GstElementFactory *factory = gst_element_factory_find (ELEMENT_NAME);
  fail_unless (factory != NULL, "element factory '%s' not found", ELEMENT_NAME);
  gst_object_unref (factory);

  GstElementFactory *alias = gst_element_factory_find (ELEMENT_ALIAS);
  fail_unless (alias != NULL, "alias factory '%s' not found", ELEMENT_ALIAS);
  gst_object_unref (alias);
}

GST_END_TEST;

GST_START_TEST (test_element_creates_and_is_pushsrc)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL, "could not instantiate '%s'", ELEMENT_NAME);
  fail_unless (GST_IS_PUSH_SRC (element), "'%s' is not a GstPushSrc",
      ELEMENT_NAME);
  gst_object_unref (element);
}

GST_END_TEST;

GST_START_TEST (test_element_has_index_property)
{
  GstElement *element = gst_element_factory_make (ELEMENT_NAME, NULL);
  fail_unless (element != NULL);

  GParamSpec *pspec =
      g_object_class_find_property (G_OBJECT_GET_CLASS (element), "index");
  fail_unless (pspec != NULL, "expected 'index' property is missing");
  fail_unless (pspec->value_type == G_TYPE_STRING,
      "'index' property should be a string");

  gchar *index = NULL;
  g_object_get (element, "index", &index, NULL);
  fail_unless (index != NULL && g_strcmp0 (index, "0") == 0,
      "default 'index' should be \"0\", got '%s'", index ? index : "(null)");
  g_free (index);

  gst_object_unref (element);
}

GST_END_TEST;

GST_START_TEST (test_src_pad_template)
{
  GstElementFactory *factory = gst_element_factory_find (ELEMENT_NAME);
  fail_unless (factory != NULL);

  const GList *templates =
      gst_element_factory_get_static_pad_templates (factory);
  gboolean found_src = FALSE;

  for (const GList *l = templates; l != NULL; l = l->next) {
    GstStaticPadTemplate *tmpl = (GstStaticPadTemplate *) l->data;

    if (tmpl->direction == GST_PAD_SRC
        && g_strcmp0 (tmpl->name_template, "src") == 0) {
      found_src = TRUE;
      fail_unless (tmpl->presence == GST_PAD_ALWAYS,
          "src pad template should be ALWAYS present");

      GstCaps *caps = gst_static_caps_get (&tmpl->static_caps);
      fail_unless (caps != NULL && !gst_caps_is_empty (caps),
          "src pad template caps are empty");

      gchar *caps_str = gst_caps_to_string (caps);
      fail_unless (g_strrstr (caps_str, "video/x-h264") != NULL,
          "src caps missing video/x-h264: %s", caps_str);
      g_free (caps_str);
      gst_caps_unref (caps);
    }
  }

  fail_unless (found_src, "no ALWAYS src pad template named 'src'");
  gst_object_unref (factory);
}

GST_END_TEST;

static Suite *
plugin_load_suite (void)
{
  Suite *s = suite_create ("libuvch264src-plugin-load");
  TCase *tc = tcase_create ("smoke");

  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_plugin_is_registered);
  tcase_add_test (tc, test_element_factories_exist);
  tcase_add_test (tc, test_element_creates_and_is_pushsrc);
  tcase_add_test (tc, test_element_has_index_property);
  tcase_add_test (tc, test_src_pad_template);

  return s;
}

GST_CHECK_MAIN (plugin_load);
