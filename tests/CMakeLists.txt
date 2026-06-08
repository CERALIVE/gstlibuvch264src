# Plugin-load smoke test (gst-check). Hardware-independent: it only exercises
# registration/introspection, never opening a UVC device.

add_executable(test_plugin_load test_plugin_load.c)
target_link_libraries(test_plugin_load PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
)

# The test dlopen()s the freshly built plugin via GST_PLUGIN_PATH, so it must be
# built first.
add_dependencies(test_plugin_load gstlibuvch264src)

# Isolate from any system-installed copy of the plugin and from the developer's
# real GStreamer registry, and make the freshly built libuvc resolvable when it
# was vendored at build time (see top-level CMakeLists.txt).
set(_smoke_env
  "GST_PLUGIN_PATH=${GST_PLUGIN_BUILD_DIR}"
  "GST_PLUGIN_SYSTEM_PATH="
  "GST_REGISTRY=${CMAKE_BINARY_DIR}/test-registry.bin"
  "GST_REGISTRY_FORK=no"
  "HOME=${CMAKE_BINARY_DIR}"
)
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _smoke_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()

add_test(NAME plugin_load_smoke COMMAND test_plugin_load)
set_tests_properties(plugin_load_smoke PROPERTIES
  ENVIRONMENT "${_smoke_env}"
)
