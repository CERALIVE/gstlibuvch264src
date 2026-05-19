# libuvc patches

This directory contains patches that must be applied to libuvc before
building gstlibuvch264src. The plugin's H265 support depends on
`UVC_FRAME_FORMAT_H265`, which stock libuvc (including v0.0.7) does not
provide.

## libuvc-h265-support.patch

Adds `UVC_FRAME_FORMAT_H265` to `enum uvc_frame_format`, registers the
H265 fourcc GUID in the format table, includes H265 in the
`UVC_FRAME_FORMAT_COMPRESSED` group, and adds a `frame->step` case so
H265 frames flow through libuvc's pipeline.

Tested against `libuvc/libuvc` tag `v0.0.7`.

### Apply

```sh
git clone https://github.com/libuvc/libuvc.git
cd libuvc
git checkout v0.0.7
patch -p1 < /path/to/gstlibuvch264src/patches/libuvc-h265-support.patch
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
         -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF
make -j"$(nproc)"
sudo make install
sudo ldconfig
```
