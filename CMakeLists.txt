# =============================================================================
# CMake build for the libuvch264src plugin-load smoke test (ctest target).
#
# SCOPE: This CMake project exists ONLY to compile the GStreamer plugin together
# with a hardware-independent gst-check smoke test and expose it via ctest.
#
# The CANONICAL production build remains Meson (libuvch264src/meson.build) and
# the Dockerfile. Device-image artifacts must NOT be produced from this build.
# The plugin source is compiled verbatim - no runtime behavior is changed here.
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(gstlibuvch264src C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

find_package(PkgConfig REQUIRED)
# Version floors. GStreamer 1.14 is the oldest series the plugin's pad/caps API
# usage compiles against (Ubuntu 18.04 baseline); no known incompatibility on
# the 1.x line, so no upper bound is imposed. libusb 1.0.20 is the floor for the
# auto-detach-kernel-driver call the libuvc patches rely on.
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.14)
pkg_check_modules(GST_BASE REQUIRED IMPORTED_TARGET gstreamer-base-1.0>=1.14)
pkg_check_modules(GST_CHECK REQUIRED IMPORTED_TARGET gstreamer-check-1.0>=1.14)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.20)

# -----------------------------------------------------------------------------
# libuvc dependency.
#
# Prefer a system/pkg-config libuvc; otherwise vendor it at build time by
# invoking scripts/build-libuvc.sh — the SAME script the Dockerfile runs, so the
# pinned SHAs, the CMake options, and the fallback patch steps live in exactly
# ONE place (no SHA/URL/patch duplication between this file and the Dockerfile).
# LIBUVC_RUNTIME_DIR is exported to the test so the vendored shared object is
# resolvable at dlopen() time.
#
# Two vendoring modes, selected by LIBUVC_USE_FORK and forwarded to the script as
# --mode (see libuvch264src/docs/notes/libuvc-fork-adr.md):
#
#   ON  (default): fork mode — the CeraLive/libuvc fork at a pinned SHA. The
#                  UVC 1.5, H.265, and configurable auto-detach changes live as
#                  commits on the fork, so NO build-time patching is needed.
#   OFF (rollback): upstream mode — upstream libuvc v0.0.7 at its pinned SHA with
#                  the UVC 1.5 + H.265 patches applied at build time (the pre-fork
#                  path). The patches/ directory is retained as the rollback
#                  artifact.
# -----------------------------------------------------------------------------
option(LIBUVC_USE_FORK "Use CeraLive/libuvc fork instead of upstream+patches" ON)

pkg_check_modules(LIBUVC IMPORTED_TARGET libuvc)

if(LIBUVC_FOUND)
  set(LIBUVC_LINK_TARGET PkgConfig::LIBUVC)
  set(LIBUVC_EXTRA_INCLUDES "")
  set(LIBUVC_RUNTIME_DIR "")
else()
  # Vendor libuvc through the single-source build script
  # (scripts/build-libuvc.sh) shared verbatim with the Dockerfile, so the pinned
  # SHAs, the CMake options, and the fallback patch steps live in exactly ONE
  # place. The script clones (and, on the LIBUVC_USE_FORK=OFF rollback, patches)
  # libuvc and installs it into a build-local prefix. LIBUVC_USE_FORK selects
  # fork vs upstream+patches and is forwarded as --mode.
  if(LIBUVC_USE_FORK)
    set(_libuvc_mode "fork")
    message(STATUS "libuvc not found via pkg-config; vendoring CeraLive/libuvc fork via scripts/build-libuvc.sh (no build-time patches)")
  else()
    set(_libuvc_mode "upstream")
    message(STATUS "libuvc not found via pkg-config; vendoring upstream v0.0.7 + patches via scripts/build-libuvc.sh (LIBUVC_USE_FORK=OFF rollback)")
  endif()

  set(_libuvc_prefix "${CMAKE_BINARY_DIR}/libuvc-install")
  set(_libuvc_srcdir "${CMAKE_BINARY_DIR}/libuvc-src")

  # Run at configure time so the headers and shared object exist before any
  # element/test target compiles — keeping every existing LIBUVC_* consumer
  # below working with no per-target add_dependencies wiring. Idempotent: skip
  # when the shared object is already present (delete <build>/libuvc-install to
  # force a rebuild).
  if(NOT EXISTS "${_libuvc_prefix}/lib/libuvc.so")
    execute_process(
      COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build-libuvc.sh
              --mode=${_libuvc_mode}
              --prefix=${_libuvc_prefix}
              --src-dir=${_libuvc_srcdir}
      RESULT_VARIABLE _libuvc_rc
    )
    if(NOT _libuvc_rc EQUAL 0)
      message(FATAL_ERROR "scripts/build-libuvc.sh failed (mode=${_libuvc_mode}, rc=${_libuvc_rc})")
    endif()
  endif()

  set(LIBUVC_LINK_TARGET "${_libuvc_prefix}/lib/libuvc.so")
  # The install lays libuvc.h and the generated libuvc_config.h side by side
  # under one include root, so a single include dir suffices.
  set(LIBUVC_EXTRA_INCLUDES "${_libuvc_prefix}/include")
  set(LIBUVC_RUNTIME_DIR "${_libuvc_prefix}/lib")
endif()

# -----------------------------------------------------------------------------
# GStreamer plugin module (compiled from the unmodified plugin source).
# Output: <build>/gstreamer-1.0/libgstlibuvch264src.so
# -----------------------------------------------------------------------------
set(GST_PLUGIN_BUILD_DIR ${CMAKE_BINARY_DIR}/gstreamer-1.0)

# Single source of truth for the element's .c files, shared verbatim with the
# Meson production build (libuvch264src/src/meson.build) and tests/CMakeLists.txt.
# sources.txt holds bare filenames; prefix each with the src dir for add_library.
file(STRINGS "${CMAKE_SOURCE_DIR}/libuvch264src/src/sources.txt" _element_src_names)
set(_element_srcs "")
foreach(_src ${_element_src_names})
  list(APPEND _element_srcs "${CMAKE_SOURCE_DIR}/libuvch264src/src/${_src}")
endforeach()

add_library(gstlibuvch264src MODULE ${_element_srcs})
set_target_properties(gstlibuvch264src PROPERTIES
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY ${GST_PLUGIN_BUILD_DIR}
)
target_include_directories(gstlibuvch264src PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/libuvch264src/src
  ${LIBUVC_EXTRA_INCLUDES}
)
target_link_libraries(gstlibuvch264src PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::LIBUSB
  ${LIBUVC_LINK_TARGET}
)
if(LIBUVC_RUNTIME_DIR)
  # Make the vendored libuvc resolvable for the test even outside ctest's env.
  set_target_properties(gstlibuvch264src PROPERTIES
    BUILD_RPATH ${LIBUVC_RUNTIME_DIR}
  )
endif()

add_subdirectory(tests)
