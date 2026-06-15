# Hardware-independent ctest suites for the libuvch264src element.
#
#   plugin_load_smoke   registration/introspection only, against the REAL plugin.
#   mock_smoke          full READY->PLAYING streaming path, against a MOCK-backed
#                       copy of the plugin (tests/mock_libuvc.c stands in for
#                       libuvc). Optional -fsanitize=address / -fsanitize=thread
#                       variants are built when ENABLE_SANITIZERS=ON.

find_package(Threads REQUIRED)

# Directory holding GStreamer's own plugins (coreelements -> fakesink). Used to
# load just that one plugin explicitly in the streaming test, instead of scanning
# the whole system plugin path.
pkg_get_variable(GST_PLUGINS_DIR gstreamer-1.0 pluginsdir)

# -----------------------------------------------------------------------------
# Plugin-load smoke test (gst-check). Hardware-independent: it only exercises
# registration/introspection, never opening a UVC device.
# -----------------------------------------------------------------------------
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

# -----------------------------------------------------------------------------
# Mock streaming harness.
#
# A second, mock-backed copy of the plugin links the element's own translation
# units against tests/mock_libuvc.c instead of the real libuvc. Because the mock
# provides every uvc_* symbol the element references, no real libuvc is linked.
# The smoke test drives this plugin to PLAYING; the mock feeder thread delivers
# crafted H.264 access units and we assert the element pops them.
# -----------------------------------------------------------------------------
option(ENABLE_SANITIZERS
  "Also build -fsanitize=thread / -fsanitize=address variants of the mock harness"
  OFF)

# The element sources, compiled verbatim into the mock-backed module.
set(_element_srcs
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/uvc_device.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/ptz_control.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/spspps_cache.c
)

# add_mock_harness_variant(<variant> <sanitizer>)
#   <variant>   "" for the plain build, else a suffix ("asan", "tsan")
#   <sanitizer> "" or a -fsanitize=<name> value ("address", "thread")
function(add_mock_harness_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(plugin "gstlibuvch264src_mock${suffix}")
  set(testexe "test_mock_smoke${suffix}")
  set(ctestname "mock_smoke${suffix}")
  set(plugdir "${CMAKE_BINARY_DIR}/gstreamer-1.0-mock${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  # --- mock-backed plugin module ---
  add_library(${plugin} MODULE ${_element_srcs} mock_libuvc.c)
  set_target_properties(${plugin} PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "gstlibuvch264src"
    LIBRARY_OUTPUT_DIRECTORY ${plugdir}
  )
  target_include_directories(${plugin} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${plugin} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::LIBUSB
    Threads::Threads
  )
  if(san_opts)
    target_compile_options(${plugin} PRIVATE ${san_opts})
    target_link_options(${plugin} PRIVATE ${san_opts})
  endif()

  # --- smoke test executable (loads the mock-backed plugin) ---
  add_executable(${testexe} test_mock_smoke.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  # Each variant gets its own plugin dir, registry, and HOME so they never
  # clobber each other (the smoke test writes the element's SPS/PPS cache under
  # HOME). The mock-backed plugin links no real libuvc, so no LD_LIBRARY_PATH.
  # Full isolation: the system plugin path is blanked so the registry never
  # scans unrelated third-party plugins (which trip the sanitizers). Only the
  # mock-backed plugin (GST_PLUGIN_PATH) plus an explicitly loaded coreelements
  # plugin (GST_COREELEMENTS_PLUGIN, for fakesink) are visible to the test.
  set(_home "${CMAKE_BINARY_DIR}/mock-home${suffix}")
  file(MAKE_DIRECTORY ${_home})
  set(_env
    "GST_PLUGIN_PATH=${plugdir}"
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/mock-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(sanitizer STREQUAL "address")
    # Leak detection is intentionally deferred: GStreamer/GLib's one-time global
    # allocations are noisy. ASan still catches buffer overflow, use-after-free,
    # and double-free - the high-value coverage the oversized-SPS path targets.
    list(APPEND _env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  elseif(sanitizer STREQUAL "thread")
    # ignore_noninstrumented_modules makes TSan treat calls into uninstrumented
    # libraries (all of GStreamer/GLib here) atomically, so their internal mutex
    # bookkeeping is not misreported; the suppressions file is a documented
    # backstop. Races inside the instrumented plugin + mock are still reported.
    list(APPEND _env
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
  endif()

  add_test(NAME ${ctestname} COMMAND ${testexe})
  set_tests_properties(${ctestname} PROPERTIES
    ENVIRONMENT "${_env}"
    # The element's control thread binds a fixed /tmp/libuvc_control socket, so
    # the mock variants must not run concurrently even under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_mock_harness_variant("" "")
if(ENABLE_SANITIZERS)
  add_mock_harness_variant("asan" "address")
  add_mock_harness_variant("tsan" "thread")
endif()
