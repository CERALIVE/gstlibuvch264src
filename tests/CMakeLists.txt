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
# Shared test helpers.
# -----------------------------------------------------------------------------

# apply_sanitizer_flags(<target> <sanitizer>)
#   <sanitizer>  "" (no-op), "address", or "thread".
#   Adds the matching -fsanitize compile + link flags to <target>. The compile
#   side also enables frame pointers and debug info for readable sanitizer
#   traces; passing the same set at link is a harmless no-op for the extra
#   compile-only flags and keeps every call site to a single line. Collapses the
#   ~20 hand-rolled san_opts / ENABLE_SANITIZERS blocks that used to be inlined.
function(apply_sanitizer_flags target sanitizer)
  if(sanitizer STREQUAL "")
    return()
  endif()
  set(_san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  target_compile_options(${target} PRIVATE ${_san_opts})
  target_link_options(${target} PRIVATE ${_san_opts})
endfunction()

# setup_gst_test_environment(<prefix> <sanitizer>)
#   Assembles the common GStreamer test-isolation environment shared by the
#   statically-registered single-executable suites and the mock-plugin suites,
#   and returns it as a CMake list in the parent-scope variable GST_TEST_ENV.
#
#   <prefix>     unique stem for this test instance; also names the per-instance
#                GST_REGISTRY file and $HOME dir (created here) so suites never
#                clobber each other under `ctest -j`.
#   <sanitizer>  "", "address", or "thread". Appends the STANDARD
#                ASAN_OPTIONS / TSAN_OPTIONS (the latter using tsan.suppressions).
#                Suites with bespoke sanitizer env - the PTS race suite's
#                tsan_pts.suppressions, the negotiate LSAN leak case, and the
#                sps-bounds G_SLICE knob - pass "" here and append their own.
#
#   Callers append GST_PLUGIN_PATH (mock-plugin suites), GST_CHECKS, and any
#   test-specific vars to GST_TEST_ENV after the call. Collapses the ~15
#   hand-rolled six-line env blocks that used to be inlined.
function(setup_gst_test_environment prefix sanitizer)
  set(_home "${CMAKE_BINARY_DIR}/${prefix}-home")
  file(MAKE_DIRECTORY "${_home}")
  set(_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/${prefix}-registry.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(sanitizer STREQUAL "address")
    list(APPEND _env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  elseif(sanitizer STREQUAL "thread")
    list(APPEND _env
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
  endif()
  set(GST_TEST_ENV "${_env}" PARENT_SCOPE)
endfunction()

# add_gstcheck_multicase_test(<exe> <prefix> <sanitizer> <cases> [options])
#   Registers one ctest entry per case for a gst-check suite that fans a single
#   test executable out across several gst-check test functions via GST_CHECKS.
#   Collapses the eight near-identical multi-case `foreach` registration blocks
#   (device_select, ptz, socket, pts_safety, pts_monotonic, live_source,
#   functional, reconnect - plus framerate_mismatch) that all shared this shape:
#     setup_gst_test_environment(<prefix>-<name> <sanitizer>) -> GST_TEST_ENV
#     env = [<prepend>] GST_TEST_ENV "GST_CHECKS=<fn>" [<per-case extra>] [<append>]
#     add_test(<name> <exe>); RESOURCE_LOCK uvc_control_socket; TIMEOUT 120
#
#   <exe>        the test executable every case runs.
#   <prefix>     stem for the per-case isolation prefix ("<prefix>-<ctestname>"),
#                fed to setup_gst_test_environment for a unique registry/HOME.
#   <sanitizer>  "", "address", or "thread"; forwarded to
#                setup_gst_test_environment for the STANDARD ASAN/TSAN env. Pass
#                "" and use APPEND_ENV for suites with bespoke sanitizer options
#                (e.g. the PTS race's tsan_pts.suppressions).
#   <cases>      a CMake list of "<ctest name>:<gst-check fn>" or
#                "<ctest name>:<gst-check fn>:<per-case extra env>" entries. The
#                optional third field (which may be empty) is appended verbatim.
#
#   Options:
#     NO_RESOURCE_LOCK   omit the uvc_control_socket RESOURCE_LOCK (suites whose
#                        cases bind a per-instance path and can run concurrently).
#     PREPEND_ENV <e>... env entries placed BEFORE GST_TEST_ENV (e.g. a mock
#                        plugin's GST_PLUGIN_PATH).
#     APPEND_ENV  <e>... env entries placed AFTER the per-case vars (e.g. a
#                        bespoke TSAN_OPTIONS for the PTS race suite).
function(add_gstcheck_multicase_test exe prefix sanitizer cases)
  cmake_parse_arguments(_mc "NO_RESOURCE_LOCK" "" "PREPEND_ENV;APPEND_ENV" ${ARGN})
  foreach(_case ${cases})
    string(REPLACE ":" ";" _parts "${_case}")
    list(GET _parts 0 _ctestname)
    list(GET _parts 1 _testfn)
    list(LENGTH _parts _nparts)
    set(_extra "")
    if(_nparts GREATER 2)
      list(GET _parts 2 _extra)
    endif()

    setup_gst_test_environment("${prefix}-${_ctestname}" "${sanitizer}")
    set(_env ${_mc_PREPEND_ENV} ${GST_TEST_ENV})
    list(APPEND _env "GST_CHECKS=${_testfn}")
    if(NOT _extra STREQUAL "")
      list(APPEND _env "${_extra}")
    endif()
    if(_mc_APPEND_ENV)
      list(APPEND _env ${_mc_APPEND_ENV})
    endif()

    add_test(NAME ${_ctestname} COMMAND ${exe})
    # ENVIRONMENT must be passed as a single quoted argument: the env value is a
    # CMake list whose internal ";" would otherwise flatten into stray
    # set_tests_properties key/value args. Branch instead of building a props
    # variable so "${_env}" stays one argument.
    if(_mc_NO_RESOURCE_LOCK)
      set_tests_properties(${_ctestname} PROPERTIES
        ENVIRONMENT "${_env}"
        TIMEOUT 120)
    else()
      # start()-driven suites share the fixed /tmp/libuvc_control socket, so they
      # must not run concurrently under `ctest -j`.
      set_tests_properties(${_ctestname} PROPERTIES
        ENVIRONMENT "${_env}"
        RESOURCE_LOCK uvc_control_socket
        TIMEOUT 120)
    endif()
  endforeach()
endfunction()

# add_static_gstcheck_exe(<name> [EXTRA_SRCS <src>...])
#   Builds a statically-registered single-executable gst-check suite: the named
#   test (<name>.c) plus the element TUs and the libuvc mock compiled into ONE
#   binary (no plugin .so), so the mock's in-process state is directly
#   observable. Wires the standard element include dirs, the GST/libusb/Threads
#   links, and STATIC_SUITE_SANITIZER instrumentation. Collapses the seven
#   identical add_executable/target_include_directories/target_link_libraries/
#   apply_sanitizer_flags blocks (device_select, ptz, socket, pts_monotonic,
#   live_source, reconnect, framerate_mismatch). Suites with a different shape
#   (extra error TU, libusb mock, or a per-variant sanitizer) keep their own
#   bespoke definition.
#
#   EXTRA_SRCS  additional sources to compile into the binary beyond the
#               <name>.c + element TUs + mock_libuvc.c default set.
function(add_static_gstcheck_exe name)
  cmake_parse_arguments(_se "" "" "EXTRA_SRCS" ${ARGN})
  add_executable(${name} ${name}.c ${_element_srcs} mock_libuvc.c ${_se_EXTRA_SRCS})
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${name} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  apply_sanitizer_flags(${name} "${STATIC_SUITE_SANITIZER}")
endfunction()

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
add_test(NAME cache_selector_key COMMAND test_cache selector)

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

# The statically-registered single-executable suites build exactly one
# instrumented binary; under ENABLE_SANITIZERS that binary is always the
# AddressSanitizer build. The mock-plugin and PTS/TSan suites pass their own
# per-variant sanitizer instead. Centralised here so the per-suite call sites
# reduce to apply_sanitizer_flags(... "${STATIC_SUITE_SANITIZER}").
set(STATIC_SUITE_SANITIZER "")
if(ENABLE_SANITIZERS)
  set(STATIC_SUITE_SANITIZER "address")
endif()

# The element sources, compiled verbatim into the mock-backed module.
# gstlibuvch264src_error.c is included because ptz_control.c posts bus errors via
# gst_libuvc_h264_src_post_error(); every mock-linked executable needs the symbol.
#
# Single source of truth: read the .c list from libuvch264src/src/sources.txt,
# shared verbatim with the top-level CMakeLists.txt and the Meson production build
# (libuvch264src/src/meson.build). sources.txt holds bare filenames; prefix each
# with the src dir to form the absolute paths the test targets expect.
file(STRINGS "${CMAKE_SOURCE_DIR}/libuvch264src/src/sources.txt" _element_src_names)
set(_element_srcs "")
foreach(_src ${_element_src_names})
  list(APPEND _element_srcs "${CMAKE_SOURCE_DIR}/libuvch264src/src/${_src}")
endforeach()

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
  apply_sanitizer_flags(${plugin} "${sanitizer}")

  # --- smoke test executable (loads the mock-backed plugin) ---
  add_executable(${testexe} test_mock_smoke.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # Each variant gets its own plugin dir, registry, and HOME so they never
  # clobber each other (the smoke test writes the element's SPS/PPS cache under
  # HOME). The mock-backed plugin links no real libuvc, so no LD_LIBRARY_PATH.
  # Full isolation: the system plugin path is blanked so the registry never
  # scans unrelated third-party plugins (which trip the sanitizers). Only the
  # mock-backed plugin (GST_PLUGIN_PATH) plus an explicitly loaded coreelements
  # plugin (GST_COREELEMENTS_PLUGIN, for fakesink) are visible to the test. The
  # standard ASAN_OPTIONS (detect_leaks=0) / TSAN_OPTIONS (tsan.suppressions,
  # ignore_noninstrumented_modules) come from setup_gst_test_environment.
  setup_gst_test_environment("mock${suffix}" "${sanitizer}")
  set(_env "GST_PLUGIN_PATH=${plugdir}" ${GST_TEST_ENV})

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
  apply_sanitizer_flags(${lifecycleexe} "${sanitizer}")

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
add_static_gstcheck_exe(test_device_select)

# "<ctest name>:<gst-check test fn>" - one ctest entry each, selected by GST_CHECKS.
set(_device_select_cases
  "device_zero:test_device_zero"
  "index_validate:test_index_validate"
  "device_restart_leak:test_device_restart_leak"
  "device_selector:test_device_selector"
)
# start() spawns the control thread which binds the fixed /tmp/libuvc_control
# socket, so these must not run concurrently (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_device_select "device-select"
  "${STATIC_SUITE_SANITIZER}" "${_device_select_cases}")

# -----------------------------------------------------------------------------
# Native PTZ property/signal tests (Task 12).
#
# Same single-executable, statically-registered shape as test_device_select so
# the mock's last-written PTZ values are observable in-process
# (mock_uvc_get_last_pantilt/zoom). Each gst-check test is its own ctest entry
# selected via GST_CHECKS.
# -----------------------------------------------------------------------------
add_static_gstcheck_exe(test_ptz)

set(_ptz_cases
  "ptz_properties:test_ptz_properties"
  "ptz_capability_gate:test_ptz_capability_gate"
)
# start() binds the fixed /tmp/libuvc_control socket via the control thread, so
# the PTZ cases must not run concurrently (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_ptz "ptz"
  "${STATIC_SUITE_SANITIZER}" "${_ptz_cases}")

# -----------------------------------------------------------------------------
# Opt-in hardened control-socket tests (Task 16 + Task 4 harden-v3).
#
# Same single-executable, statically-registered shape as test_ptz so start()/
# stop() run the REAL socket bind/unbind against the filesystem (only libuvc is
# mocked). No RESOURCE_LOCK is needed: each case binds a per-instance path under
# its own private $XDG_RUNTIME_DIR (created in the test), so nothing collides on
# the old fixed /tmp/libuvc_control path - which is gone now that the socket is
# opt-in and per-instance.
#
# Built in plain, ASAN, and TSAN variants. The TSAN variant is the gate for the
# control-socket lifecycle race (close_during_select): the element TUs and the
# mock link into one -fsanitize=thread binary so TSan observes the control thread
# against stop()/unbind(). It uses the STANDARD tsan.suppressions (third-party
# blind spots only) - NOT tsan_pts.suppressions - and that file has had its
# control_thread/process_control_command baselines removed now that this task
# locks the path, so a regression reappears as a reported race.
# -----------------------------------------------------------------------------
function(add_socket_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_socket${suffix}")

  add_executable(${testexe} test_socket.c ${_element_srcs} mock_libuvc.c)
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # "<ctest name>:<gst-check test fn>" - one ctest entry each via GST_CHECKS.
  set(_socket_cases
    "socket_default_off${suffix}:test_socket_default_off"
    "socket_hardened${suffix}:test_socket_hardened"
    "socket_command_framing${suffix}:test_socket_command_framing"
    "socket_command_oversized${suffix}:test_socket_command_oversized"
    "socket_close_during_select${suffix}:test_socket_close_during_select"
  )
  # No RESOURCE_LOCK: each case binds a per-instance path under its own private
  # $XDG_RUNTIME_DIR, so nothing collides and they can run concurrently.
  add_gstcheck_multicase_test(${testexe} "socket${suffix}" "${sanitizer}"
    "${_socket_cases}" NO_RESOURCE_LOCK)
endfunction()

add_socket_variant("" "")
if(ENABLE_SANITIZERS)
  add_socket_variant("asan" "address")
  add_socket_variant("tsan" "thread")
endif()

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
# CVE-2026-1991 reproduction harness (Task 9).
#
# A standalone C program (NOT a gst-check suite) that links the REAL libuvc and
# calls the exported uvc_scan_streaming() with a hand-built, malformed
# libusb_config_descriptor. On a libuvc WITHOUT the CVE-2026-1991 guard each case
# SIGSEGVs (out-of-range interface index / NULL altsetting dereference); on a
# patched libuvc each case returns UVC_ERROR_INVALID_DEVICE and the program exits
# 0. Two argv-selected cases ("oob", "null"), one ctest entry each.
#
# This gate exercises the *vendored libuvc*, so it only passes once the consumed
# libuvc carries the fix (CeraLive fork harden/2026.6: uvc_scan_streaming guard +
# e001f04 backport). Until the build pins that SHA it reproduces the crash by
# design. Built with the static-suite sanitizer (ASan when ENABLE_SANITIZERS=ON)
# for a clean fault backtrace; the SIGSEGV is deterministic with or without ASan.
# -----------------------------------------------------------------------------
add_executable(test_cve_2026_1991 test_cve_2026_1991.c)
target_include_directories(test_cve_2026_1991 PRIVATE
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
)
target_link_libraries(test_cve_2026_1991 PRIVATE
  PkgConfig::LIBUSB
  ${LIBUVC_LINK_TARGET}
)
apply_sanitizer_flags(test_cve_2026_1991 "${STATIC_SUITE_SANITIZER}")

# Make a vendored libuvc resolvable at run time (built via FetchContent/script).
set(_cve_env "")
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _cve_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()
if(STATIC_SUITE_SANITIZER STREQUAL "address")
  list(APPEND _cve_env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
endif()

add_test(NAME cve_2026_1991_oob COMMAND test_cve_2026_1991 oob)
add_test(NAME cve_2026_1991_null COMMAND test_cve_2026_1991 null)
set_tests_properties(cve_2026_1991_oob cve_2026_1991_null PROPERTIES
  ENVIRONMENT "${_cve_env}"
  TIMEOUT 30
)

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

  add_executable(${testexe} test_sps_bounds.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # Standard ASAN_OPTIONS (detect_leaks=0) comes from the helper; this suite adds
  # G_SLICE=always-malloc so each GObject instance is a plain malloc with an ASan
  # redzone right after it - otherwise the oversized NAL copy would land inside
  # the instance (intra-object) where ASan cannot see it.
  setup_gst_test_environment("bounds${suffix}" "${sanitizer}")
  set(_base_env "GST_PLUGIN_PATH=${plugdir}" ${GST_TEST_ENV})
  if(sanitizer STREQUAL "address")
    list(APPEND _base_env "G_SLICE=always-malloc")
  endif()

  # "<ctest name>:<gst-check test fn>:<feeder mode>:<codec>" - one ctest entry
  # each via GST_CHECKS. The SPS case overflows the H.264 SPS buffer (default
  # codec); the VPS case overflows the H.265-only VPS buffer, so it selects H265.
  set(_bounds_cases
    "${ctestname}:test_oversized_sps_is_clamped_no_overflow:oversized_sps:"
    "vps_bounds${suffix}:test_oversized_vps_is_clamped_no_overflow:oversized_vps:MOCK_UVC_FRAME_FORMAT=H265"
  )
  foreach(_case ${_bounds_cases})
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _bctestname)
    list(GET _parts 1 _btestfn)
    list(GET _parts 2 _bmode)
    list(GET _parts 3 _bcodec)

    set(_env ${_base_env})
    list(APPEND _env "MOCK_UVC_FRAME_MODE=${_bmode}" "GST_CHECKS=${_btestfn}")
    if(NOT _bcodec STREQUAL "")
      list(APPEND _env "${_bcodec}")
    endif()

    add_test(NAME ${_bctestname} COMMAND ${testexe})
    set_tests_properties(${_bctestname} PROPERTIES
      ENVIRONMENT "${_env}"
      # Shares the element's fixed /tmp/libuvc_control socket with the other mock
      # variants, so it must not run concurrently with them under `ctest -j`.
      RESOURCE_LOCK uvc_control_socket
      TIMEOUT 120
    )
  endforeach()
endfunction()

add_sps_bounds_variant("" "")
if(ENABLE_SANITIZERS)
  add_sps_bounds_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# Clock + PTS critical-section tests (Task 10).
#
# Statically links the element TUs, the libuvc mock, and the driver into one
# executable (like test_device_select) so the racing set_clock() vmethod and the
# mock feeder's frame_callback() run in the SAME instrumented binary - what TSan
# needs to observe both sides of the clock/PTS race. The error helper TU is
# included because ptz_control.c references it. The -fsanitize=thread variant is
# the gate; it uses a dedicated suppressions file that does NOT mask
# frame_callback()/set_clock(), so a lock regression reappears as a reported
# race instead of being silently baselined.
# -----------------------------------------------------------------------------
function(add_pts_safety_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_pts_thread_safety${suffix}")

  add_executable(${testexe}
    test_pts_thread_safety.c
    ${_element_srcs}
    ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src_error.c
    mock_libuvc.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # "<ctest name>:<gst-check test fn>" - one ctest entry each via GST_CHECKS.
  set(_pts_cases
    "pts_thread_safety${suffix}:test_pts_clock_race"
    "frame_throughput${suffix}:test_frame_throughput"
  )
  # This suite uses a DEDICATED TSan suppressions file (tsan_pts.suppressions)
  # that does NOT mask frame_callback()/set_clock(), so the helper's standard
  # thread branch is bypassed (sanitizer "") and the bespoke TSAN_OPTIONS is
  # passed via APPEND_ENV instead.
  set(_pts_extra "")
  if(sanitizer STREQUAL "thread")
    set(_pts_extra APPEND_ENV
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan_pts.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
  endif()
  # start() binds the fixed /tmp/libuvc_control socket, so these must not run
  # concurrently (RESOURCE_LOCK, applied by helper).
  add_gstcheck_multicase_test(${testexe} "pts" "" "${_pts_cases}" ${_pts_extra})
endfunction()

add_pts_safety_variant("" "")
if(ENABLE_SANITIZERS)
  add_pts_safety_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# USB teardown tests (Task 11, H1 + L8).
#
# Links the element TUs, the libuvc mock, AND a dedicated libusb mock into one
# executable - and deliberately does NOT link the real libusb, so mock_libusb.c
# is the sole provider of the libusb-1.0 symbols force_usb_release() calls.
# MOCK_LIBUSB_TEARDOWN switches the libuvc mock to hand back a real (mock) libusb
# handle and to model uvc_close() closing it, so the old double-close (the handle
# closed in force_usb_release() and again in uvc_close()) is observable. The ASAN
# variant aborts on that double-free; the fixed teardown stays clean and the test
# also asserts one close per open and that the interface count came from the
# active config descriptor, not a hardcoded 0..7 range.
# -----------------------------------------------------------------------------
function(add_usb_teardown_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_usb_teardown${suffix}")

  add_executable(${testexe}
    test_usb_teardown.c
    ${_element_srcs}
    mock_libuvc.c
    mock_libusb.c
  )
  target_compile_definitions(${testexe} PRIVATE MOCK_LIBUSB_TEARDOWN)
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
  )
  # No PkgConfig::LIBUSB: mock_libusb.c supplies every libusb symbol, and linking
  # the real library too would multiply-define them.
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    Threads::Threads
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  setup_gst_test_environment("usb-teardown${suffix}" "${sanitizer}")
  set(_env ${GST_TEST_ENV})

  add_test(NAME usb_teardown${suffix} COMMAND ${testexe})
  set_tests_properties(usb_teardown${suffix} PROPERTIES
    ENVIRONMENT "${_env}"
    # start() binds the fixed /tmp/libuvc_control socket via the control thread,
    # so this must not run concurrently with the other start()-driven suites.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_usb_teardown_variant("" "")
if(ENABLE_SANITIZERS)
  add_usb_teardown_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# negotiate() tests (Task 13).
#
# Statically registers the element (like test_device_select) so the mock's
# format descriptor can be shaped in-process via mock_uvc_set_format_mode().
#   negotiate_edge_*   a zero-format device posts a RESOURCE bus error, and a
#                      0 fps / zero device-interval descriptor fails gracefully
#                      instead of taking SIGFPE on the framerate divisions (H4).
#   negotiate_leak     -fsanitize=address build only: N renegotiations leak no
#                      GstCaps (M3). LeakSanitizer's recoverable check is the
#                      gate; an __lsan_disable() warm-up window plus a
#                      suppressions file keep GStreamer's one-time init noise out
#                      of the result while the per-renegotiation caps leak trips.
# -----------------------------------------------------------------------------
function(add_negotiate_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_negotiate${suffix}")

  add_executable(${testexe}
    test_negotiate.c
    ${_element_srcs}
    mock_libuvc.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # "<ctest name>:<gst-check test fn>" - one ctest entry each via GST_CHECKS. The
  # leak case is only meaningful (and only registered) for the asan variant.
  set(_neg_cases
    "negotiate_edge_zero_format${suffix}:test_negotiate_zero_format"
    "negotiate_edge_framerate${suffix}:test_negotiate_framerate_zero"
    "negotiate_edge_zero_interval${suffix}:test_negotiate_zero_interval"
    "negotiate_extreme_fps_low${suffix}:test_negotiate_extreme_fps_low"
    "negotiate_extreme_fps_high${suffix}:test_negotiate_extreme_fps_high"
    "negotiate_extreme_res_low${suffix}:test_negotiate_extreme_res_low"
    "negotiate_extreme_res_high${suffix}:test_negotiate_extreme_res_high"
    "negotiate_smaller_max_payload${suffix}:test_negotiate_smaller_max_payload"
    "negotiate_max_payload_default${suffix}:test_negotiate_max_payload_default_unchanged"
    "negotiate_max_payload_reject${suffix}:test_negotiate_max_payload_rejected_falls_back"
    "negotiate_max_payload_accept${suffix}:test_negotiate_max_payload_accepted_applied"
  )
  if(NOT sanitizer STREQUAL "")
    list(APPEND _neg_cases "negotiate_leak:test_negotiate_leak")
  endif()

  foreach(_case ${_neg_cases})
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _ctestname)
    list(GET _parts 1 _testfn)

    # Helper called with "" so the standard ASAN branch is bypassed: the leak
    # case needs detect_leaks=1 + LSAN suppressions, the edge cases need the
    # standard detect_leaks=0 - both are applied case-by-case below.
    setup_gst_test_environment("negotiate-${_ctestname}" "")
    set(_neg_env ${GST_TEST_ENV})
    list(APPEND _neg_env "GST_CHECKS=${_testfn}")
    if(NOT sanitizer STREQUAL "")
      if(_ctestname STREQUAL "negotiate_leak")
        # detect_leaks must be ON for __lsan_do_recoverable_leak_check(), but the
        # at-exit check is disabled so only the in-test recoverable check (scoped
        # by __lsan_disable() and filtered by the suppressions file) is the gate.
        list(APPEND _neg_env
          "ASAN_OPTIONS=detect_leaks=1:leak_check_at_exit=0:abort_on_error=1:halt_on_error=1"
          "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_negotiate.suppressions:print_suppressions=0")
      else()
        list(APPEND _neg_env
          "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
      endif()
    endif()

    add_test(NAME ${_ctestname} COMMAND ${testexe})
    set_tests_properties(${_ctestname} PROPERTIES
      ENVIRONMENT "${_neg_env}"
      # start() binds the fixed /tmp/libuvc_control socket via the control thread,
      # so these must not run concurrently with each other or the other
      # start()-driven suites under `ctest -j`.
      RESOURCE_LOCK uvc_control_socket
      TIMEOUT 120
    )
  endforeach()
endfunction()

add_negotiate_variant("" "")
if(ENABLE_SANITIZERS)
  add_negotiate_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# PTS clamp + restart-state tests (Task 14).
#
# Same single-executable, statically-registered shape as test_device_select so
# the element's frame_callback() PTS arithmetic and the start() restart-state
# reset run in the test process with the mock feeder driving frame_callback().
#
#   pts_monotonic  The mock feeds access units far faster than the negotiated
#                  frame interval, so the running PTS outruns the wall clock and
#                  the resync offset goes strongly negative; assert every buffer
#                  PTS is strictly increasing and every DURATION is sane (M4).
#   restart_idr    Lead each stream with non-IDR slices before the first IDR,
#                  stream once to latch had_idr, restart, and assert the first
#                  buffer after the restart is never a forwarded non-IDR (M5).
# -----------------------------------------------------------------------------
add_static_gstcheck_exe(test_pts_monotonic)

# "<ctest name>:<gst-check test fn>" - one ctest entry each, selected by GST_CHECKS.
set(_pts_monotonic_cases
  "pts_monotonic:test_pts_monotonic"
  "restart_idr:test_restart_idr"
)
# start() binds the fixed /tmp/libuvc_control socket via the control thread, so
# these must not run concurrently (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_pts_monotonic "pts-monotonic"
  "${STATIC_SUITE_SANITIZER}" "${_pts_monotonic_cases}")

# -----------------------------------------------------------------------------
# V4L2 capability probe tests (Task 23).
#
# Compiles uvc_device.c directly into the test executable (no plugin .so, no
# mock) and calls gst_libuvc_h264_src_v4l2_probe() with indices guaranteed
# absent on any CI host. Verifies the probe is non-fatal and does not crash
# regardless of whether a real /dev/videoN node exists.
# -----------------------------------------------------------------------------
add_executable(test_v4l2_probe
  test_v4l2_probe.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/uvc_device.c
)
target_include_directories(test_v4l2_probe PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_v4l2_probe PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::LIBUSB
  ${LIBUVC_LINK_TARGET}
)

set(_probe_env
  "GST_PLUGIN_SYSTEM_PATH="
  "GST_REGISTRY=${CMAKE_BINARY_DIR}/v4l2probe-registry.bin"
  "GST_REGISTRY_FORK=no"
  "HOME=${CMAKE_BINARY_DIR}"
)
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _probe_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()

add_test(NAME v4l2_probe COMMAND test_v4l2_probe probe)
set_tests_properties(v4l2_probe PROPERTIES
  ENVIRONMENT "${_probe_env}"
  TIMEOUT 30
)

add_test(NAME v4l2_probe_nonfatal COMMAND test_v4l2_probe nonfatal)
set_tests_properties(v4l2_probe_nonfatal PROPERTIES
  ENVIRONMENT "${_probe_env}"
  TIMEOUT 30
)

# -----------------------------------------------------------------------------
# Live-source polish tests (Task 19).
#
# Same single-executable, statically-registered shape as test_pts_monotonic so
# the test can drive a real PLAYING pipeline against the mock feeder AND reach
# into the instance struct / call frame_callback() directly.
#   live_source_polish      LATENCY query reports live + min == one frame
#                           interval, and buffer OFFSETs are a strict +1 sequence.
#   spspps_write_on_change  The SPS/PPS cache is written on a changed parameter
#                           set, not rewritten for an identical one (L10).
# -----------------------------------------------------------------------------
add_static_gstcheck_exe(test_live_source)

# "<ctest name>:<gst-check test fn>:<extra env>" - one ctest entry each via
# GST_CHECKS. cache_h265_vps_write_on_change is named with the "cache" stem so
# `ctest -R cache` covers it alongside the path-builder suites; it exports
# MOCK_UVC_FRAME_FORMAT=H265 to declare the codec the case exercises.
set(_live_source_cases
  "live_source_polish:test_live_source_polish:"
  "live_source_latency_pre_negotiate:test_latency_pre_negotiate:"
  "spspps_write_on_change:test_spspps_write_on_change:"
  "cache_h265_vps_write_on_change:test_h265_vps_write_on_change:MOCK_UVC_FRAME_FORMAT=H265"
)
# start() may spawn the control thread, so keep these off the shared socket
# resource like the other start()-driven suites (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_live_source "live-source"
  "${STATIC_SUITE_SANITIZER}" "${_live_source_cases}")

# -----------------------------------------------------------------------------
# NAL parser unit tests (Task 15). Pure C against the real parser in
# frame_pipeline.c: no UVC device, no GStreamer runtime, no mock feeder. The TU
# is linked whole, so frame_callback()'s only element-internal references (the
# debug category and store_spspps) are satisfied by stubs in test_nal_parse.c;
# the parser functions under test touch neither. Three argv-selected suites; the
# ASAN variant is the teeth for the bounds suite (over-read on truncated /
# oversized / merged NAL buffers).
# -----------------------------------------------------------------------------
function(add_nal_parse_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_nal_parse${suffix}")

  add_executable(${testexe}
    test_nal_parse.c
    ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # Pure-C parser test: no GStreamer runtime, registry, or device, so it needs
  # only the bare ASAN_OPTIONS (not the full setup_gst_test_environment block).
  set(_nal_env "")
  if(sanitizer STREQUAL "address")
    set(_nal_env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  foreach(_suite multislice startcode bounds
                 multislice_h265 startcode_h265 bounds_h265
                 store_bounds fuzz_seed)
    add_test(NAME nal_parse_${_suite}${suffix} COMMAND ${testexe} ${_suite})
    if(_nal_env)
      set_tests_properties(nal_parse_${_suite}${suffix} PROPERTIES ENVIRONMENT "${_nal_env}")
    endif()
  endforeach()
endfunction()

add_nal_parse_variant("" "")
if(ENABLE_SANITIZERS)
  add_nal_parse_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# libFuzzer NAL-parser harness (Task 11 harden). Opt-in: -DENABLE_FUZZERS=ON.
#
# libFuzzer needs -fsanitize=fuzzer, which conflicts with the suite's
# -fsanitize=thread and is clang-only, so the fuzz target is built by an explicit
# clang invocation (a custom command) independent of the project compiler (which
# may be gcc) rather than a normal add_executable. It links fuzz_nal.c + the real
# frame_pipeline.c with -fsanitize=fuzzer,address into <build>/tests/fuzz_nal.
#
# The fixed-iteration (20000-run) ctest smoke replays the seed corpus and proves
# nothing crashes; it runs against a build-local COPY of the corpus so the fuzzer
# never writes new finds back into the source tree. It is NOT in the default
# ctest path - the target and the test exist only when ENABLE_FUZZERS=ON.
# -----------------------------------------------------------------------------
option(ENABLE_FUZZERS "Build the libFuzzer NAL-parser harness (clang only)" OFF)
if(ENABLE_FUZZERS)
  find_program(CLANG_FOR_FUZZ NAMES clang clang-20 clang-19 clang-18 clang-17)
  if(NOT CLANG_FOR_FUZZ)
    message(FATAL_ERROR
      "ENABLE_FUZZERS=ON requires clang (libFuzzer); none found in PATH")
  endif()

  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags gstreamer-1.0 gstreamer-base-1.0
    OUTPUT_VARIABLE _fuzz_pkg_cflags OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --libs gstreamer-1.0 gstreamer-base-1.0
    OUTPUT_VARIABLE _fuzz_pkg_libs OUTPUT_STRIP_TRAILING_WHITESPACE)
  separate_arguments(_fuzz_pkg_cflags)
  separate_arguments(_fuzz_pkg_libs)

  set(_fuzz_incs -I${CMAKE_SOURCE_DIR}/libuvch264src/src)
  if(LIBUVC_EXTRA_INCLUDES)
    list(APPEND _fuzz_incs -I${LIBUVC_EXTRA_INCLUDES})
  endif()
  foreach(_d ${LIBUVC_INCLUDE_DIRS})
    list(APPEND _fuzz_incs -I${_d})
  endforeach()

  set(_fuzz_bin ${CMAKE_CURRENT_BINARY_DIR}/fuzz_nal)
  add_custom_command(
    OUTPUT ${_fuzz_bin}
    COMMAND ${CLANG_FOR_FUZZ} -g -O1 -fno-omit-frame-pointer
            -fsanitize=fuzzer,address
            ${_fuzz_incs} ${_fuzz_pkg_cflags}
            ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_nal.c
            ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
            ${_fuzz_pkg_libs}
            -o ${_fuzz_bin}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_nal.c
            ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
            ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.h
            ${CMAKE_SOURCE_DIR}/libuvch264src/src/spspps_cache.h
    COMMENT "Building libFuzzer NAL harness (clang -fsanitize=fuzzer,address)"
    VERBATIM)
  add_custom_target(fuzz_nal ALL DEPENDS ${_fuzz_bin})

  # Build-local corpus copy so the smoke run never writes new finds into the
  # source tree (libFuzzer treats the positional dir as a writable corpus).
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus)
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/nal
       DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus)

  add_test(NAME fuzz_nal_smoke
    COMMAND ${_fuzz_bin} -runs=20000 -timeout=20
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus/nal)
  set_tests_properties(fuzz_nal_smoke PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1"
    TIMEOUT 120)
endif()

# -----------------------------------------------------------------------------
# Consolidated functional suite (Task 20).
#
# Reuses the mock-backed plugin built by add_mock_harness_variant (loaded via an
# isolated GST_PLUGIN_PATH, exactly like the smoke test) and fills the gaps the
# per-task suites leave: the POSITIVE caps-negotiation matrix (H.264 -> video/
# x-h264, H.265 -> video/x-h265, frames flow in both) and frame delivery under
# downstream backpressure. The H.265 case selects the codec purely via the
# MOCK_UVC_FRAME_FORMAT env var the mock reads at uvc_init() - the same .so backs
# every case. Built in plain, ASAN, and TSAN variants so the full functional path
# is exercised under both sanitizers in CI.
# -----------------------------------------------------------------------------
function(add_functional_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(plugin "gstlibuvch264src_mock${suffix}")   # built by add_mock_harness_variant
  set(testexe "test_functional${suffix}")
  set(plugdir "${CMAKE_BINARY_DIR}/gstreamer-1.0-mock${suffix}")

  add_executable(${testexe} test_functional.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # "<ctest name>:<gst-check test fn>:<extra env>" - one ctest entry each via
  # GST_CHECKS. The H.265 case adds MOCK_UVC_FRAME_FORMAT=H265 so the same mock
  # plugin advertises and feeds H.265.
  set(_functional_cases
    "functional_caps_h264${suffix}:test_caps_h264:"
    "functional_caps_h265${suffix}:test_caps_h265:MOCK_UVC_FRAME_FORMAT=H265"
    "functional_backpressure${suffix}:test_backpressure:"
  )
  # GST_PLUGIN_PATH (PREPEND_ENV) points the test at the mock-backed plugin; the
  # standard ASAN_OPTIONS / TSAN_OPTIONS (tsan.suppressions) come from the helper.
  # Shares the mock plugin with the other mock variants; keep it off the shared
  # control-socket resource like the rest (RESOURCE_LOCK, applied by helper).
  add_gstcheck_multicase_test(${testexe} "functional" "${sanitizer}"
    "${_functional_cases}" PREPEND_ENV "GST_PLUGIN_PATH=${plugdir}")
endfunction()

add_functional_variant("" "")
if(ENABLE_SANITIZERS)
  add_functional_variant("asan" "address")
  add_functional_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# Disconnect-detection + opt-in reconnect tests (Task 18).
#
# Same single-executable, statically-registered shape as test_pts_monotonic so
# the mock feeder drives the real create() disconnect path in-process and the
# mock's uvc_open() counter is observable (mock_uvc_open_count). Each case is its
# own ctest entry via GST_CHECKS so it gets its own mock configuration.
#   disconnect_error  reconnect off (default): a silent source posts a
#                     RESOURCE/READ bus error within the timeout window.
#   reconnect_resume  reconnect on: the element reopens the device and frames
#                     resume after a simulated replug instead of erroring.
# -----------------------------------------------------------------------------
add_static_gstcheck_exe(test_reconnect)

set(_reconnect_cases
  "disconnect_error:test_disconnect_error"
  "reconnect_resume:test_reconnect_resume"
  "reconnect_idr_regate:test_reconnect_idr_regate"
  "reconnect_exhaustion:test_reconnect_exhaustion"
  "reconnect_backoff_sequence:test_reconnect_backoff_sequence"
  "reconnect_backoff_interrupt:test_reconnect_backoff_interrupt"
)
# start() may spawn the control thread, so keep these off the shared socket
# resource like the other start()-driven suites (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_reconnect "reconnect"
  "${STATIC_SUITE_SANITIZER}" "${_reconnect_cases}")

# -----------------------------------------------------------------------------
# Reconnect exhaustion teardown-order test (Task 8, harden-v2).
#
# Reuses test_reconnect.c but, like test_usb_teardown, links a dedicated libusb
# mock (MOCK_LIBUSB_TEARDOWN) and deliberately does NOT link the real libusb, so
# uvc_close() models the single libusb_close() and the close counters are
# observable. With every reopen forced to fail (mock open-fail injection), the
# bounded backoff loop exhausts; the test then proves uvc_close() owned the one
# libusb_close() (balanced, no double-close) and that force_usb_release() never
# ran on the reconnect path (config-query count == 0) - i.e. it never preceded
# uvc_close() on any retry. The ASAN variant additionally aborts on a double-free
# if the teardown order ever regresses.
# -----------------------------------------------------------------------------
function(add_reconnect_teardown_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_reconnect_teardown${suffix}")

  add_executable(${testexe}
    test_reconnect.c
    ${_element_srcs}
    mock_libuvc.c
    mock_libusb.c
  )
  target_compile_definitions(${testexe} PRIVATE MOCK_LIBUSB_TEARDOWN)
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
  )
  # No PkgConfig::LIBUSB: mock_libusb.c supplies every libusb symbol, and linking
  # the real library too would multiply-define them.
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    Threads::Threads
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  setup_gst_test_environment("reconnect-teardown${suffix}" "${sanitizer}")
  set(_rt_env ${GST_TEST_ENV})
  list(APPEND _rt_env "GST_CHECKS=test_reconnect_teardown_order")

  add_test(NAME reconnect_teardown_order${suffix} COMMAND ${testexe})
  set_tests_properties(reconnect_teardown_order${suffix} PROPERTIES
    ENVIRONMENT "${_rt_env}"
    # start() binds the fixed control socket via the control thread, so keep this
    # off the shared socket resource like the other start()-driven suites.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_reconnect_teardown_variant("" "")
if(ENABLE_SANITIZERS)
  add_reconnect_teardown_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# PTS drift-bound regression guard (Task 1, harden-v2). Committed RED.
#
# Same single-executable, statically-registered shape as test_pts_thread_safety
# so the test can construct the element directly, install a GstTestClock, and
# drive frame_callback()'s real PTS arithmetic in-process. It does NOT use the
# mock feeder: the feeder paces with usleep() (wall clock) and cannot be driven
# by a controllable clock, so a jittered/lossy synthetic arrival schedule is fed
# by calling frame_callback() directly between gst_test_clock_set_time() steps.
#
# The test asserts the Option B contract - rate fidelity |(PTS_N-PTS_0) -
# (ts_N-ts_0)| <= 2 ms and instantaneous max|PTS_i-ts_i| <= 5 ms - which the
# current estimator cannot meet, so it is committed RED and turns green with the
# Task 5 rearchitecture. The ASAN variant proves the harness itself is clean:
# only the bound assertion fails, never a sanitizer error.
# -----------------------------------------------------------------------------
function(add_pts_drift_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_pts_drift${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe}
    test_pts_drift.c
    ${_element_srcs}
    mock_libuvc.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  set(_drift_home "${CMAKE_BINARY_DIR}/pts-drift-home${suffix}")
  file(MAKE_DIRECTORY ${_drift_home})

  set(_drift_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/pts-drift-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_drift_home}"
    "CK_FORK=no"
    "GST_CHECKS=test_pts_drift"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(sanitizer STREQUAL "address")
    list(APPEND _drift_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  # No RESOURCE_LOCK: the test never enters PLAYING and never calls start(), so
  # it spawns no control thread and binds no socket - it can run concurrently.
  add_test(NAME pts_drift${suffix} COMMAND ${testexe})
  set_tests_properties(pts_drift${suffix} PROPERTIES
    ENVIRONMENT "${_drift_env}"
    TIMEOUT 120
  )
endfunction()

add_pts_drift_variant("" "")
if(ENABLE_SANITIZERS)
  add_pts_drift_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# Framerate-mismatch tolerance tests (Task 9, harden-v2).
#
# Same single-executable, statically-registered shape as test_pts_monotonic so
# framerate_mismatch_pts can construct the element directly, install a
# GstTestClock, and drive frame_callback()'s PTS arithmetic deterministically at
# a cadence that differs from the negotiated framerate, while
# framerate_mismatch_no_renegotiate drives a real PLAYING pipeline against the
# mock feeder paced by MOCK_UVC_FRAME_INTERVAL_US.
#
#   framerate_mismatch_pts             negotiated 30 fps, frames injected at ~24
#                                      fps: PTS tracks running-time within 5 ms,
#                                      DURATION stays caps-derived (not the real
#                                      cadence), OFFSET is a strict +1 sequence,
#                                      no silent drops. RED under the estimator;
#                                      green after Option B (Task 5).
#   framerate_mismatch_no_renegotiate  the slower device must not trigger a caps
#                                      renegotiation (exactly one CAPS event) or a
#                                      silent drop over the streaming path.
# -----------------------------------------------------------------------------
add_static_gstcheck_exe(test_framerate_mismatch)

# "<ctest name>:<gst-check test fn>:<extra env>" - one ctest entry each via
# GST_CHECKS. The no-renegotiate case paces the mock feeder at ~24 fps
# (1e6/24 us) via MOCK_UVC_FRAME_INTERVAL_US so the device delivers slower than
# the advertised 30 fps - the mismatch under test.
set(_framerate_mismatch_cases
  "framerate_mismatch_pts:test_framerate_mismatch_pts:"
  "framerate_mismatch_no_renegotiate:test_framerate_mismatch_no_renegotiate:MOCK_UVC_FRAME_INTERVAL_US=41667"
)
# framerate_mismatch_no_renegotiate enters PLAYING -> start(); keep both off the
# shared control-socket resource like the rest (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_framerate_mismatch "framerate-mismatch"
  "${STATIC_SUITE_SANITIZER}" "${_framerate_mismatch_cases}")

# -----------------------------------------------------------------------------
# PAUSED->PLAYING PTS rebaseline continuity test (Task 13, harden-v2).
#
# Same single-executable, statically-registered shape as test_pts_drift /
# test_framerate_mismatch: constructs the element directly, installs a
# GstTestClock, and drives frame_callback()'s PTS arithmetic in-process across a
# simulated PLAYING -> PAUSED -> PLAYING cycle. The real change_state vmethod
# (called through the class vtable) resets the base_time + prev_pts latches on
# PAUSED->PLAYING (Task 5); the pipeline's base_time re-assignment is modelled by
# gst_element_set_base_time() advanced by the pause gap. Asserts the first
# post-resume PTS re-latches to the new running-time (now - new base_time) rather
# than being clamped to prev_pts + 1 or jumping the whole pause gap ahead, and
# that subsequent PTS stay strictly monotonic within the drift bound relative to
# the new base_time. Guards the Metis Option B rebaseline fix.
# -----------------------------------------------------------------------------
add_executable(test_pts_resume
  test_pts_resume.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_pts_resume PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_pts_resume PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
apply_sanitizer_flags(test_pts_resume "${STATIC_SUITE_SANITIZER}")

# No RESOURCE_LOCK: the test never enters PLAYING via the pipeline and never
# calls start(), so it spawns no control thread and binds no socket - it can run
# concurrently with everything else under `ctest -j`. (change_state is invoked
# directly for the PAUSED->PLAYING transition, which does not call start().)
setup_gst_test_environment("pts-resume" "${STATIC_SUITE_SANITIZER}")
set(_resume_env ${GST_TEST_ENV})
list(APPEND _resume_env "GST_CHECKS=test_paused_playing_rebaseline")

add_test(NAME paused_playing_rebaseline COMMAND test_pts_resume)
set_tests_properties(paused_playing_rebaseline PROPERTIES
  ENVIRONMENT "${_resume_env}"
  TIMEOUT 120
)

# -----------------------------------------------------------------------------
# Clock-discontinuity recovery tests (Task 16, harden-v2).
#
# Deterministic unit counterpart to the Task 13 state-transition test
# (test_pts_resume.c). Same single-executable, statically-registered shape as
# test_pts_drift so the test constructs the element directly, installs a
# GstTestClock, and drives frame_callback()'s PTS arithmetic in-process. It does
# NOT use the mock feeder (whose usleep() pacing a controllable clock cannot
# drive); the clock swap goes through the real gst_element_set_clock() vmethod so
# the Task 5 rebaseline path is exercised, not re-implemented.
#
#   clock_discontinuity        Stream on one test clock, swap to a second clock
#                              jumped far forward (1 s -> 100 s) via set_clock(),
#                              then stream more. Asserts PTS stays strictly
#                              monotonic across the swap (the thin guard never
#                              emits a backward PTS), rebaselines onto the new
#                              clock's running-time, and is back within the 5 ms
#                              drift bound within N frames - which a reintroduced
#                              smoothing estimator could not do.
#   clock_discontinuity_guard  Stall the test clock (repeat the running-time) so
#                              ts <= prev_pts and the thin guard fires; assert PTS
#                              is strictly increasing (clamped to prev_pts + 1,
#                              never backward or repeated). Both names carry the
#                              clock_discontinuity stem so `ctest -R
#                              clock_discontinuity` covers the guard case too.
#
# The TSan variant is single-threaded (direct frame_callback, no feeder thread),
# so it should report nothing; it uses the dedicated tsan_pts.suppressions to
# baseline the permanent GMutex-futex blind spot in the GST_OBJECT_LOCK that the
# PTS critical section takes (same rationale as the PTS race suite).
# -----------------------------------------------------------------------------
function(add_clock_discontinuity_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_clock_discontinuity${suffix}")

  add_executable(${testexe}
    test_clock_discontinuity.c
    ${_element_srcs}
    mock_libuvc.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  apply_sanitizer_flags(${testexe} "${sanitizer}")

  # "<ctest name>:<gst-check test fn>" - one ctest entry each via GST_CHECKS.
  set(_clockjump_cases
    "clock_discontinuity${suffix}:test_clock_discontinuity"
    "clock_discontinuity_guard${suffix}:test_clock_stall_guard"
  )
  foreach(_case ${_clockjump_cases})
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _ctestname)
    list(GET _parts 1 _testfn)

    # Like the PTS race suite, use the dedicated tsan_pts.suppressions (the
    # permanent GMutex-futex blind spot), so pass "" to the helper and append the
    # bespoke TSAN_OPTIONS here.
    setup_gst_test_environment("clockjump-${_ctestname}" "")
    set(_cj_env ${GST_TEST_ENV})
    list(APPEND _cj_env "GST_CHECKS=${_testfn}")
    if(sanitizer STREQUAL "thread")
      list(APPEND _cj_env
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan_pts.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
    endif()

    # No RESOURCE_LOCK: the test never enters PLAYING and never calls start(), so
    # it spawns no control thread and binds no socket - it can run concurrently.
    add_test(NAME ${_ctestname} COMMAND ${testexe})
    set_tests_properties(${_ctestname} PROPERTIES
      ENVIRONMENT "${_cj_env}"
      TIMEOUT 120
    )
  endforeach()
endfunction()

add_clock_discontinuity_variant("" "")
if(ENABLE_SANITIZERS)
  add_clock_discontinuity_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# mpegtsmux jitter-tolerance QA (Task 14, harden-v2).
#
# An end-to-end shell check (tests/qa/av_mux_check.sh), NOT a gst-check exe: it
# drives the mock-backed plugin through the real cerastream mux topology
# (libuvch264src ! h264parse ! mpegtsmux. + audiotestsrc ! aac/mp2 ! parser !
# mpegtsmux. ; mpegtsmux ! filesink) and runs ffprobe on the .ts, asserting no
# "non monotonous DTS" and monotonic per-stream DTS/PTS. It validates the Metis
# top-risk: raw post-Option-B arrival jitter into mpegtsmux. The script points
# itself at the mock plugin built by add_mock_harness_variant via MOCK_PLUGIN_DIR
# and synthesizes its own real H.264 reference frame with x264enc, so no camera
# is needed. Registered only when the external ffprobe/x264enc prerequisites are
# present so a stripped CI image skips it cleanly instead of failing.
# -----------------------------------------------------------------------------
find_program(FFPROBE_EXECUTABLE ffprobe)
find_program(BASH_EXECUTABLE bash)
if(FFPROBE_EXECUTABLE AND BASH_EXECUTABLE)
  add_test(NAME av_mux_jitter
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/qa/av_mux_check.sh av)
  set_tests_properties(av_mux_jitter PROPERTIES
    ENVIRONMENT "MOCK_PLUGIN_DIR=${CMAKE_BINARY_DIR}/gstreamer-1.0-mock"
    # The mock .so is built by add_mock_harness_variant; ctest does not build, so
    # keep this off the shared control-socket resource because it launches its
    # own gst process that may bind the element's control thread.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 180)

  add_test(NAME av_mux_jitter_audio_only
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/qa/av_mux_check.sh audio)
  set_tests_properties(av_mux_jitter_audio_only PROPERTIES
    ENVIRONMENT "MOCK_PLUGIN_DIR=${CMAKE_BINARY_DIR}/gstreamer-1.0-mock"
    TIMEOUT 180)
else()
  message(STATUS "av_mux_jitter QA disabled (ffprobe or bash not found)")
endif()

# -----------------------------------------------------------------------------
# Compatibility regression suite (Task 5).
#
# The DURABLE guardrail for the libuvc/GStreamer compatibility boundary. Same
# single-executable, statically-registered shape as test_negotiate so the
# element TUs, the libuvc mock, and the driver link into ONE binary: the symbol
# table in test_compat.c references the ~47-symbol libuvc/libusb contract whose
# DECLARATIONS come from the real <libuvc/libuvc.h> + <libusb-1.0/libusb.h> on
# the include path (uvc_* defined by the mock, libusb_* by the real libusb linked
# below), so a future libuvc bump that drops/renames any symbol fails the BUILD
# loudly here exactly as it would in the element TUs. The caps + pipeline-parse
# cases then prove both factories register, the dual-codec pad template holds,
# and the README-style fakesink pipelines negotiate video/x-h264 / video/x-h265
# against the mock - kernel-independent (no decoder/encoder elements in graph).
# -----------------------------------------------------------------------------
add_static_gstcheck_exe(test_compat)

# "<ctest name>:<gst-check test fn>" - one ctest entry each, selected by GST_CHECKS.
# The H.265 pipeline-parse case shapes the in-process mock via
# mock_uvc_set_frame_format() in the test body, so no per-case env is needed.
set(_compat_cases
  "compat_api_surface:test_compat_api_surface"
  "compat_caps_contract:test_compat_caps_contract"
  "compat_pipeline_parse_h264:test_compat_pipeline_parse_h264"
  "compat_pipeline_parse_h265:test_compat_pipeline_parse_h265"
)
# The pipeline-parse cases enter PLAYING -> start(), which may spawn the control
# thread, so keep them off the shared control-socket resource like the other
# start()-driven suites (RESOURCE_LOCK, applied by helper).
add_gstcheck_multicase_test(test_compat "compat"
  "${STATIC_SUITE_SANITIZER}" "${_compat_cases}")
