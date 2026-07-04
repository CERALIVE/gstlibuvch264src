project('libuvch264src', 'c',
  # No version here by design: the .deb CalVer is derived purely from git tags at
  # publish time (.github/workflows/publish-release.yml, job calculate-version).
  # There is no tracked VERSION file - the git tag namespace (v*) is the single
  # source of truth.
  default_options: ['warning_level=2', 'buildtype=debugoptimized', 'c_std=c11']
)

# Strict ISO C11 (-std=c11) hides the _DEFAULT_SOURCE/_GNU_SOURCE feature macros
# the compiler-default dialect enabled implicitly. Without them POSIX usleep()
# and the struct timeval embedded in <linux/videodev2.h> (used by the V4L2
# capability probe) become incomplete/implicit and fail to compile. Define
# _GNU_SOURCE so the pinned c11 standard matches the GNU-extension behaviour the
# sibling CMake test build already relies on (CMAKE_C_STANDARD 11 +
# CMAKE_C_EXTENSIONS ON == -std=gnu11).
add_project_arguments('-D_GNU_SOURCE', language: 'c')

gst_dep = dependency('gstreamer-1.0', version: '>=1.14')
gst_base_dep = dependency('gstreamer-base-1.0', version: '>=1.14')
libuvc_dep = dependency('libuvc', required: true)
libusb_dep = dependency('libusb-1.0', required: true)

# Feature guard: the CeraLive fork exports uvc_set_transfer_buffers() (A2 API);
# raw upstream libuvc (LIBUVC_USE_FORK=OFF) does not. Probe the SELECTED libuvc
# dependency and, when present, define HAVE_UVC_TRANSFER_BUFFERS for every element
# TU (add_project_arguments applies to the library built in subdir('src')). The
# opt-in transfer-buffers property then compiles its real apply path against the
# fork and a benign warn-and-no-op path against upstream.
#
# args '-D_GNU_SOURCE': compiler checks do NOT inherit add_project_arguments, and
# under strict -std=c11 libuvc.h's `struct timespec capture_time_finished` is an
# incomplete type without the feature macro, so the probe's header include fails
# and the check falsely reports NO. Pass it here so the probe matches the element
# TUs' own build environment (mirrors the _GNU_SOURCE rationale above).
cc = meson.get_compiler('c')
if cc.has_function('uvc_set_transfer_buffers', dependencies: libuvc_dep,
    prefix: '#include <libuvc/libuvc.h>', args: '-D_GNU_SOURCE')
  add_project_arguments('-DHAVE_UVC_TRANSFER_BUFFERS', language: 'c')
endif

subdir('src')
