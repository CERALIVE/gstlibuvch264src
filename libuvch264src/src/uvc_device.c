#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "gstlibuvch264src_internal.h"
#include "uvc_device.h"

// Cheap, read-only V4L2 capability probe. Opens /dev/video<N>, issues exactly
// one VIDIOC_TRY_FMT with V4L2_PIX_FMT_H264, logs the result, and closes.
// Non-fatal: if the node is absent or the ioctl fails the element continues.
void gst_libuvc_h264_src_v4l2_probe(GstElement *element, int device_index) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/video%d", device_index);

    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        GST_INFO_OBJECT(element, "V4L2 probe unavailable: cannot open %s", path);
        return;
    }

    struct v4l2_format fmt = { 0 };
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = 1920;
    fmt.fmt.pix.height      = 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    int ret = ioctl(fd, VIDIOC_TRY_FMT, &fmt);
    close(fd);

    // sizeimage > 0 means the kernel accepted the H.264 format.
    gboolean available = (ret == 0 && fmt.fmt.pix.sizeimage > 0 &&
                          fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_H264);
    GST_INFO_OBJECT(element, "V4L2 native H.264: %s",
                    available ? "available" : "unavailable");
}
