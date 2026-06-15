/* Test-only mock of the libuvc API surface the libuvch264src element calls.
 *
 * This header declares ONLY the control knobs a functional test uses to inject
 * behavior into the mock. The libuvc functions themselves (uvc_init, uvc_open,
 * uvc_start_streaming, ...) keep their real prototypes from <libuvc/libuvc.h>;
 * the mock provides definitions for exactly the handful the element references
 * and nothing else (see mock_libuvc.c).
 *
 * Two equivalent ways to drive the mock:
 *   1. Programmatically, from a test linked against the mock object, via the
 *      mock_uvc_* setters below.
 *   2. Via environment variables, for a test that only dlopen()s the
 *      mock-backed plugin and never links the mock directly:
 *        MOCK_UVC_DEVICE_COUNT   integer, devices uvc_find_devices() exposes (default 1)
 *        MOCK_UVC_MAX_FRAMES     integer, frames the feeder delivers then stops (default 0 = until stop)
 *        MOCK_UVC_FRAME_FORMAT   "H264" | "H265" (default H264)
 *        MOCK_UVC_FRAME_MODE     "valid" | "oversized_sps" | "disconnect" (default valid)
 *   Environment variables, when present, win over the programmatic setters.
 */

#ifndef MOCK_LIBUVC_H
#define MOCK_LIBUVC_H

#include <stdint.h>
#include <libuvc/libuvc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Behavior the frame feeder injects into the element's frame_callback. */
typedef enum {
  /* Well-formed access units: SPS + PPS + IDR with 4-byte start codes. */
  MOCK_UVC_FRAME_VALID = 0,
  /* An SPS NAL larger than SPSPPSBUFSZ (1024 B) to exercise the element's
   * fixed-size SPS/PPS copy buffers. For overflow-detection tests only. */
  MOCK_UVC_FRAME_OVERSIZED_SPS,
  /* Simulated cable pull: the feeder stops delivering frames and goes silent.
   * Real libuvc does NOT pass a NULL frame on disconnect in callback mode -
   * the callback simply stops being invoked (spike verdict, Task 4). */
  MOCK_UVC_FRAME_DISCONNECT,
} mock_uvc_frame_mode_t;

/* Restore every mock knob to its default (1 device, H264, valid frames,
 * unlimited feed, nominal PTZ ranges). Also re-reads environment overrides. */
void mock_uvc_reset(void);

/* Number of devices the next uvc_find_devices()/uvc_open() will expose.
 * 0 makes uvc_find_devices() report UVC_ERROR_NO_DEVICE. */
void mock_uvc_set_device_count(int count);

/* Pixel format the feeder crafts and uvc_get_format_descs() advertises. */
void mock_uvc_set_frame_format(enum uvc_frame_format format);

/* Frame feeder behavior; see mock_uvc_frame_mode_t. */
void mock_uvc_set_frame_mode(mock_uvc_frame_mode_t mode);

/* Stop the feeder after delivering this many frames (0 = run until
 * uvc_stop_streaming()). DISCONNECT mode treats this as the silence point. */
void mock_uvc_set_max_frames(int max_frames);

/* Stubbed PTZ ranges returned by the uvc_get_*_abs() MIN/MAX requests. */
void mock_uvc_set_ptz_range(int32_t pan_min, int32_t pan_max,
                            int32_t tilt_min, int32_t tilt_max,
                            uint16_t zoom_min, uint16_t zoom_max);

/* Frames the feeder has delivered since the last uvc_start_streaming()
 * (observability for assertions). */
int mock_uvc_frames_delivered(void);

/* Device-list arrays handed out by uvc_find_devices() that have not yet been
 * released with uvc_free_device_list(). A correct caller leaves this at its
 * starting value; a leak makes it grow. */
int mock_uvc_device_lists_outstanding(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LIBUVC_H */
