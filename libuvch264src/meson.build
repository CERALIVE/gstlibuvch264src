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

subdir('src')
