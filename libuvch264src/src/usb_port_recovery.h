#ifndef GST_LIBUVC_USB_PORT_RECOVERY_H
#define GST_LIBUVC_USB_PORT_RECOVERY_H

#include <glib.h>

G_BEGIN_DECLS

/* Deep USB recovery: the rung BELOW a USB port reset.
 *
 * A port reset (USBDEVFS_RESET / libusb_reset_device) can leave a device that
 * never comes back: the kernel re-enumerates, every attempt fails with
 * `error -71` (EPROTO), and after PORT_INIT_TRIES it logs `unable to enumerate
 * USB device` and stops. The device object is then GONE, so nothing addressed
 * by device path can be written any more.
 *
 * Which sysfs object to write is therefore decided by what SURVIVED, and the
 * two layers carry DIFFERENT attributes. Measured on the RK3588 board
 * (kernel 6.1.115-vendor-rk35xx), 14/14 USB ports and 12/12 USB devices:
 *
 *   /sys/bus/usb/devices/<bus>-<chain>       DEVICE  has `authorized`, no `disable`
 *   .../<hub>:1.0/<hub>-port<N>              PORT    has `disable`, no `authorized`
 *
 * So `authorized` is reachable only while a device object exists, and once
 * enumeration has failed outright the port's `disable` is the only handle left.
 * This module implements both and picks by what is actually present.
 *
 * THE TARGET MUST BE CAPTURED BEFORE THE RESET. After a failed re-enumeration
 * there is no device directory to resolve a port from, so the port path has to
 * have been read while the device was still open.
 *
 * HONESTY BOUND — this is a port STATE cycle, not a proven VBUS removal.
 * Writing 1 to `disable` clears PORT_POWER and the latched C_CONNECTION /
 * C_ENABLE bits, and writing 0 restores power so the hub sees a genuine
 * connect-change. On the measured board that transition is real (xHCI PORTSC
 * goes `Powered Connected Enabled` -> `Powered-off Not-connected Disabled` and
 * back with `Change: CSC`), yet the same root hub advertises
 * `wHubCharacteristic 0x000a` = "No power switching" and the board's Type-C
 * 5 V rail is a separate GPIO regulator. Whether VBUS physically dropped was
 * NOT established. Never describe this as a power cycle. */

typedef enum {
  /* Nothing was captured before the reset, so there is no path to act on. */
  USB_DEEP_RECOVERY_NO_TARGET = 0,
  /* The port's hub carries at least one OTHER enumerated device. Hubs may
   * switch power in ganged mode (measured: the board's Terminus 1a40:0101
   * reports `Ganged power switching`), so cycling one port can cut every port
   * on that hub. Refusing is the only safe answer, because nothing readable
   * from sysfs proves the switching is per-port. */
  USB_DEEP_RECOVERY_REFUSED_SHARED_HUB,
  /* A DIFFERENT device now holds the captured device path or port. Bus
   * addresses are recycled, so the vid:pid recorded at capture time is what
   * separates "our camera came back" from "something else moved in"; acting on
   * the latter would reset a device this element has no business touching. */
  USB_DEEP_RECOVERY_REFUSED_IDENTITY,
  /* Neither attribute exists at the captured paths. */
  USB_DEEP_RECOVERY_UNAVAILABLE,
  /* EACCES/EPERM: USB sysfs attributes are root-writable only. An element
   * embedded in a root service can escalate; one running as a normal user
   * cannot, and must say so rather than look like it tried. */
  USB_DEEP_RECOVERY_DENIED,
  /* The write reached the attribute and the kernel rejected it. */
  USB_DEEP_RECOVERY_FAILED,
  /* Device-level `authorized` 0->1 completed: a logical deauthorise and
   * re-probe of a device object that is still enumerated. */
  USB_DEEP_RECOVERY_DEVICE_REPROBED,
  /* Port-level `disable` 1->0 completed. */
  USB_DEEP_RECOVERY_PORT_CYCLED,
} UsbDeepRecoveryOutcome;

typedef struct {
  /* Device directory as it was at capture time. It may legitimately be gone by
   * the time the recovery runs — that is precisely the case rung 2 exists for. */
  gchar *device_path;
  /* Port directory, resolved through the device's `port` symlink. Survives the
   * device's disappearance, which is why it is captured rather than derived. */
  gchar *port_path;
  guint16 vendor_id;
  guint16 product_id;
} UsbRecoveryTarget;

/* Resolve the sysfs paths for the device at (bus, address) and record its
 * vid:pid so the recovery can refuse a path that a DIFFERENT device has taken
 * over in the meantime. `sysfs_root` is "/sys" in production and a synthetic
 * tree under test. Returns FALSE and leaves `out` zeroed if the device, its
 * identity, or its port link cannot be resolved. */
gboolean usb_recovery_target_capture(const gchar *sysfs_root, guint8 bus,
                                     guint8 address, UsbRecoveryTarget *out);

void usb_recovery_target_clear(UsbRecoveryTarget *target);

/* Run the escalation ONCE against a captured target:
 *   1. device-level `authorized` 0->1, if the device object is still there AND
 *      still reports the captured vid:pid;
 *   2. otherwise the port-level `disable` 1->0 cycle, if no other device on the
 *      hub would be disturbed.
 * `hold_ms` is how long the port is held disabled between the two writes. The
 * write itself can BLOCK for seconds (measured 2.5 s) because disable_store()
 * takes the hub device lock and an in-flight hub_event holds it. */
UsbDeepRecoveryOutcome usb_deep_recovery_run(const UsbRecoveryTarget *target,
                                             guint hold_ms);

/* Stable, loggable name for an outcome. */
const gchar *usb_deep_recovery_outcome_name(UsbDeepRecoveryOutcome outcome);

G_END_DECLS

#endif /* GST_LIBUVC_USB_PORT_RECOVERY_H */
