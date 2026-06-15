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
# SPS/PPS cache path-builder unit tests. Pure C against spspps_path.h (no GST,
# no device, no filesystem) - they assert M8 traversal blocking, M7 NULL-path
# safety, and the L5 codec+resolution cache key. One executable, two suites.
# -----------------------------------------------------------------------------
add_executable(test_cache test_cache.c)
target_include_directories(test_cache PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
)

add_test(NAME cache_path_safety COMMAND test_cache path)
add_test(NAME cache_resolution_key COMMAND test_cache key)

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
# gstlibuvch264src_error.c is included because ptz_control.c posts bus errors via
# gst_libuvc_h264_src_post_error(); every mock-linked executable needs the symbol.
set(_element_srcs
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/uvc_device.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/ptz_control.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/spspps_cache.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src_error.c
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

  # --- lifecycle regression tests (unlock/unlock_stop + control_mutex) ---
  # Reuses the same mock-backed plugin and isolated env as the smoke test; a
  # per-test GST_CHECKS selects one case so each gets its own mock config.
  set(lifecycleexe "test_lifecycle${suffix}")
  add_executable(${lifecycleexe} test_lifecycle.c)
  target_link_libraries(${lifecycleexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${lifecycleexe} ${plugin})
  if(san_opts)
    target_compile_options(${lifecycleexe} PRIVATE ${san_opts})
    target_link_options(${lifecycleexe} PRIVATE ${san_opts})
  endif()

  # DISCONNECT mode (one frame, then silence) makes create() block, so the test
  # exercises unlock() interrupting it on teardown.
  add_test(NAME unlock_shutdown${suffix} COMMAND ${lifecycleexe})
  set_tests_properties(unlock_shutdown${suffix} PROPERTIES
    ENVIRONMENT "${_env};GST_CHECKS=test_unlock_shutdown;MOCK_UVC_FRAME_MODE=disconnect"
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )

  add_test(NAME mutex_restart${suffix} COMMAND ${lifecycleexe})
  set_tests_properties(mutex_restart${suffix} PROPERTIES
    ENVIRONMENT "${_env};GST_CHECKS=test_mutex_restart"
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_mock_harness_variant("" "")
if(ENABLE_SANITIZERS)
  add_mock_harness_variant("asan" "address")
  add_mock_harness_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# Device-selection unit tests (Task 6).
#
# The element TUs, the libuvc mock, and the driver are linked into ONE
# executable with the element type registered statically (no plugin .so). That
# keeps the mock's state in the test process, so the device-list leak counter
# (mock_uvc_device_lists_outstanding) is directly observable and device counts
# can be set programmatically. Each gst-check test is surfaced as its own ctest
# entry via GST_CHECKS so the suite can be filtered test-by-test.
# -----------------------------------------------------------------------------
add_executable(test_device_select
  test_device_select.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_device_select PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_device_select PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_device_select PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_device_select PRIVATE -fsanitize=address)
endif()

# "<ctest name>:<gst-check test fn>" - one ctest entry each, selected by GST_CHECKS.
set(_device_select_cases
  "device_zero:test_device_zero"
  "index_validate:test_index_validate"
  "device_restart_leak:test_device_restart_leak"
)
foreach(_case ${_device_select_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_ds_home "${CMAKE_BINARY_DIR}/device-select-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_ds_home})

  set(_ds_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/device-select-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_ds_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _ds_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_device_select)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_ds_env}"
    # start() spawns the control thread which binds the fixed /tmp/libuvc_control
    # socket, so these must not run concurrently with each other or the mock
    # streaming variants.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# Native PTZ property/signal tests (Task 12).
#
# Same single-executable, statically-registered shape as test_device_select so
# the mock's last-written PTZ values are observable in-process
# (mock_uvc_get_last_pantilt/zoom). Each gst-check test is its own ctest entry
# selected via GST_CHECKS.
# -----------------------------------------------------------------------------
add_executable(test_ptz
  test_ptz.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_ptz PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_ptz PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_ptz PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_ptz PRIVATE -fsanitize=address)
endif()

set(_ptz_cases
  "ptz_properties:test_ptz_properties"
  "ptz_capability_gate:test_ptz_capability_gate"
)
foreach(_case ${_ptz_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_ptz_home "${CMAKE_BINARY_DIR}/ptz-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_ptz_home})

  set(_ptz_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/ptz-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_ptz_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _ptz_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_ptz)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_ptz_env}"
    # start() binds the fixed /tmp/libuvc_control socket via the control thread,
    # so the PTZ cases must not run concurrently with each other or the other
    # start()-driven suites under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# Error-mapping unit test (gst-check). Compiles the standalone error TU directly
# and asserts each uvc_error_t -> GST_ELEMENT_ERROR domain/code on the bus.
# No plugin and no UVC device are involved; only the helper + libuvc's error
# enum/strerror are linked.
# -----------------------------------------------------------------------------
add_executable(test_error_map
  test_error_map.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src_error.c
)
target_include_directories(test_error_map PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_error_map PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  ${LIBUVC_LINK_TARGET}
)

# Isolate the GStreamer registry/HOME, and make a vendored libuvc resolvable at
# run time when it was built via FetchContent (see top-level CMakeLists.txt).
set(_errmap_env
  "GST_REGISTRY=${CMAKE_BINARY_DIR}/errmap-registry.bin"
  "GST_REGISTRY_FORK=no"
  "HOME=${CMAKE_BINARY_DIR}"
)
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _errmap_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()

add_test(NAME error_map COMMAND test_error_map)
set_tests_properties(error_map PROPERTIES ENVIRONMENT "${_errmap_env}")

# -----------------------------------------------------------------------------
# SPS/PPS/VPS bounds-clamp regression (C1, heap overflow). Reuses the mock-backed
# plugin built by add_mock_harness_variant, but runs the feeder in oversized-SPS
# mode so an unclamped NAL copy would smash the element's fixed 1024 B SPS
# buffer. The ASAN variant is the teeth: an overflow aborts the run; the clamp
# keeps it green.
# -----------------------------------------------------------------------------
function(add_sps_bounds_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(plugin "gstlibuvch264src_mock${suffix}")   # built by add_mock_harness_variant
  set(testexe "test_sps_bounds${suffix}")
  set(ctestname "sps_bounds${suffix}")
  set(plugdir "${CMAKE_BINARY_DIR}/gstreamer-1.0-mock${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe} test_sps_bounds.c)
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

  set(_home "${CMAKE_BINARY_DIR}/bounds-home${suffix}")
  file(MAKE_DIRECTORY ${_home})
  set(_env
    "GST_PLUGIN_PATH=${plugdir}"
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/bounds-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
    "MOCK_UVC_FRAME_MODE=oversized_sps"
  )
  if(sanitizer STREQUAL "address")
    # always-malloc so each GObject instance is a plain malloc with an ASan
    # redzone right after it; otherwise the SPS overflow would land inside the
    # instance (intra-object) where ASan cannot see it. detect_leaks=0 keeps
    # GLib's one-time global allocations from drowning the real finding.
    list(APPEND _env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1"
      "G_SLICE=always-malloc")
  endif()

  add_test(NAME ${ctestname} COMMAND ${testexe})
  set_tests_properties(${ctestname} PROPERTIES
    ENVIRONMENT "${_env}"
    # Shares the element's fixed /tmp/libuvc_control socket with the other mock
    # variants, so it must not run concurrently with them under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_sps_bounds_variant("" "")
if(ENABLE_SANITIZERS)
  add_sps_bounds_variant("asan" "address")
endif()
