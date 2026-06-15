library_name = 'gstlibuvch264src'

# Single source of truth for the element's .c files, shared verbatim with the
# CMake test build (top-level CMakeLists.txt + tests/CMakeLists.txt). sources.txt
# holds bare .c filenames, one per line; fs.read resolves it relative to this
# meson.build (libuvch264src/src/).
fs = import('fs')
src_files = fs.read('sources.txt').strip().split('\n')

shared_library(library_name, src_files,
  dependencies: [gst_dep, gst_base_dep, libuvc_dep, libusb_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'gstreamer-1.0')
)
