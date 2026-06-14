#include <libusb-1.0/libusb.h>
#include "gstlibuvch264src_internal.h"
#include "uvc_device.h"

// Force USB device release by directly accessing libusb
void gst_libuvc_h264_src_force_usb_release(GstLibuvcH264Src *self) {
    GST_DEBUG_OBJECT(self, "Forcing USB device release");
    
    if (!self->uvc_devh) return;
    
    // Get the underlying libusb handle
    struct libusb_device_handle *usb_devh = uvc_get_libusb_handle(self->uvc_devh);
    if (!usb_devh) {
        GST_WARNING_OBJECT(self, "Cannot get libusb handle from uvc");
        return;
    }
    
    // Get USB device info
    struct libusb_device *usb_dev = libusb_get_device(usb_devh);
    if (!usb_dev) {
        GST_WARNING_OBJECT(self, "Cannot get libusb device");
        return;
    }
    
    int bus = libusb_get_bus_number(usb_dev);
    int addr = libusb_get_device_address(usb_dev);
    GST_INFO_OBJECT(self, "USB device at bus %d, address %d", bus, addr);
    
    // Try to release all interfaces
    for (int interface = 0; interface < 8; interface++) {
        int ret = libusb_release_interface(usb_devh, interface);
        if (ret == LIBUSB_SUCCESS) {
            GST_DEBUG_OBJECT(self, "Released interface %d", interface);
        } else if (ret == LIBUSB_ERROR_NOT_FOUND) {
            // Interface doesn't exist, that's fine
            break;
        }
    }
    
    // Try kernel detach if needed
    #ifdef LIBUSB_OPTION_DETACH_KERNEL_DRIVER
    for (int interface = 0; interface < 8; interface++) {
        if (libusb_kernel_driver_active(usb_devh, interface) == 1) {
            GST_DEBUG_OBJECT(self, "Detaching kernel driver from interface %d", interface);
            libusb_detach_kernel_driver(usb_devh, interface);
        }
    }
    #endif
    
    // Force close the libusb handle
    GST_DEBUG_OBJECT(self, "Force closing libusb handle");
    libusb_close(usb_devh);
    
    // Reset the device if possible (requires newer libusb)
    #ifdef LIBUSB_HAS_GET_DEVICE
    // This forces a USB port reset
    libusb_reset_device(usb_devh);
    #endif
    
    // Clear the uvc handle pointer since we've closed it
    // Note: uvc_close() will fail if we call it now, but that's OK
}
