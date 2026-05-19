# libuvc patches

This directory contains patches that must be applied to libuvc before
building gstlibuvch264src. They are bundled here so a single libuvc
checkout (tested against `libuvc/libuvc` tag `v0.0.7`) can support both
the plugin's H265 codepaths and the IRL streaming scenarios that
UnlimitedIRL hit with stock libuvc.

The `Dockerfile` at the repo root applies both patches automatically.
For host builds, apply them in the order shown below.

## uvc15-support.patch

Sourced from
[UnlimitedIRL-Team/libuvch264src](https://github.com/UnlimitedIRL-Team/libuvch264src/blob/experimental/patches/uvc15-support.patch).

Two changes:

1. `uvc_wrap()` calls `libusb_set_auto_detach_kernel_driver(usb_devh, 1)`
   so libuvc can claim USB interfaces that are already bound to the
   `uvcvideo` kernel driver, instead of failing in `uvc_open_internal`.
2. `uvc_parse_vc_header()` accepts UVC 1.5 (`bcdUVC == 0x0150`) headers
   in addition to UVC 1.0/1.1, so newer devices are no longer rejected
   with `UVC_ERROR_NOT_SUPPORTED`.

## libuvc-h265-support.patch

Adds `UVC_FRAME_FORMAT_H265` to `enum uvc_frame_format`, registers the
H265 fourcc GUID in the format table, includes H265 in the
`UVC_FRAME_FORMAT_COMPRESSED` group, and adds a `frame->step` case so
H265 frames flow through libuvc's pipeline. Required by the H265
codepaths ported from upstream BELABOX.

## Apply (host build)

```sh
git clone https://github.com/libuvc/libuvc.git
cd libuvc
git checkout v0.0.7
patch -p1 < /path/to/gstlibuvch264src/patches/uvc15-support.patch
patch -p1 < /path/to/gstlibuvch264src/patches/libuvc-h265-support.patch
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
         -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF
make -j"$(nproc)"
sudo make install
sudo ldconfig
```
