#include "usb_port_recovery.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read a small sysfs attribute into `buf`, NUL-terminated and trimmed. sysfs
 * attributes are generated on read, so a single read(2) returns the whole
 * value; anything longer than the caller's buffer is not an attribute we
 * understand and is rejected rather than truncated. */
static gboolean read_attr(const gchar *dir, const gchar *attr, gchar *buf,
                          gsize len) {
  gchar *path = g_build_filename(dir, attr, NULL);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  g_free(path);
  if (fd < 0) {
    return FALSE;
  }

  ssize_t n = read(fd, buf, len - 1);
  close(fd);
  if (n <= 0) {
    return FALSE;
  }
  buf[n] = '\0';
  g_strchomp(buf);
  return buf[0] != '\0';
}

static gboolean read_attr_uint(const gchar *dir, const gchar *attr, guint base,
                               guint64 *out) {
  gchar buf[64];
  if (!read_attr(dir, attr, buf, sizeof buf)) {
    return FALSE;
  }

  gchar *end = NULL;
  errno = 0;
  guint64 value = g_ascii_strtoull(buf, &end, (gint) base);
  if (errno != 0 || end == buf || *end != '\0') {
    return FALSE;
  }
  *out = value;
  return TRUE;
}

/* Write one value to a sysfs attribute. Deliberately a raw write(2): the glib
 * file helpers write a temporary and rename it, which sysfs cannot accept.
 * Returns TRUE on success; on failure sets *error to the outcome that explains
 * it, so a caller can tell "no privilege" apart from "no such knob". */
static gboolean write_attr(const gchar *dir, const gchar *attr,
                           const gchar *value, UsbDeepRecoveryOutcome *error) {
  gchar *path = g_build_filename(dir, attr, NULL);
  /* O_TRUNC so the write REPLACES the value rather than overwriting its first
   * bytes: sysfs ignores the file offset, but nothing else guarantees that, and
   * a partial overwrite would leave a stale tail behind the new value. */
  int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  g_free(path);
  if (fd < 0) {
    if (errno == EACCES || errno == EPERM) {
      *error = USB_DEEP_RECOVERY_DENIED;
    } else {
      *error = errno == ENOENT ? USB_DEEP_RECOVERY_UNAVAILABLE
                               : USB_DEEP_RECOVERY_FAILED;
    }
    return FALSE;
  }

  gsize len = strlen(value);
  ssize_t written = write(fd, value, len);
  int saved = errno;
  close(fd);
  if (written == (ssize_t) len) {
    return TRUE;
  }

  *error = (saved == EACCES || saved == EPERM) ? USB_DEEP_RECOVERY_DENIED
                                               : USB_DEEP_RECOVERY_FAILED;
  return FALSE;
}

static gboolean identity_matches(const gchar *device_path, guint16 vendor_id,
                                 guint16 product_id) {
  guint64 vid = 0, pid = 0;
  if (!read_attr_uint(device_path, "idVendor", 16, &vid)
      || !read_attr_uint(device_path, "idProduct", 16, &pid)) {
    return FALSE;
  }
  return vid == vendor_id && pid == product_id;
}

gboolean usb_recovery_target_capture(const gchar *sysfs_root, guint8 bus,
                                     guint8 address, UsbRecoveryTarget *out) {
  g_return_val_if_fail(out != NULL, FALSE);
  memset(out, 0, sizeof *out);
  if (sysfs_root == NULL) {
    return FALSE;
  }

  gchar *devices = g_build_filename(sysfs_root, "bus", "usb", "devices", NULL);
  GDir *dir = g_dir_open(devices, 0, NULL);
  if (dir == NULL) {
    g_free(devices);
    return FALSE;
  }

  gboolean found = FALSE;
  const gchar *name;
  while (!found && (name = g_dir_read_name(dir)) != NULL) {
    /* `<dev>:<cfg>.<iface>` entries are interfaces, not devices. */
    if (strchr(name, ':') != NULL) {
      continue;
    }

    gchar *device_path = g_build_filename(devices, name, NULL);
    guint64 busnum = 0, devnum = 0, vid = 0, pid = 0;
    if (read_attr_uint(device_path, "busnum", 10, &busnum) && busnum == bus
        && read_attr_uint(device_path, "devnum", 10, &devnum)
        && devnum == address
        && read_attr_uint(device_path, "idVendor", 16, &vid)
        && read_attr_uint(device_path, "idProduct", 16, &pid)) {
      /* The `port` symlink is the authoritative device->port mapping. Deriving
       * the port directory from the device name by string surgery would have to
       * re-implement the kernel's own naming for root hubs, hub chains and
       * SuperSpeed peers; the link is exact and always present on an enumerated
       * non-root device. */
      gchar *link = g_build_filename(device_path, "port", NULL);
      gchar *port_path = realpath(link, NULL);
      g_free(link);
      if (port_path != NULL) {
        out->device_path = g_strdup(device_path);
        out->port_path = g_strdup(port_path);
        out->vendor_id = (guint16) vid;
        out->product_id = (guint16) pid;
        free(port_path);
        found = TRUE;
      }
    }
    g_free(device_path);
  }

  g_dir_close(dir);
  g_free(devices);
  return found;
}

void usb_recovery_target_clear(UsbRecoveryTarget *target) {
  if (target == NULL) {
    return;
  }
  g_clear_pointer(&target->device_path, g_free);
  g_clear_pointer(&target->port_path, g_free);
  target->vendor_id = 0;
  target->product_id = 0;
}

/* TRUE when any port of the same hub, other than this one, still has a child
 * device. Such a hub may be switching power in ganged mode, in which case
 * cycling our port cuts the others too — and no sysfs attribute reports the
 * switching mode, so the only safe reading of an ambiguous hub is "do not
 * touch it". */
static gboolean hub_carries_other_devices(const gchar *port_path) {
  gchar *hub_iface = g_path_get_dirname(port_path);
  gchar *self_name = g_path_get_basename(port_path);

  GDir *dir = g_dir_open(hub_iface, 0, NULL);
  if (dir == NULL) {
    g_free(hub_iface);
    g_free(self_name);
    /* Cannot see the siblings, so cannot prove the port is unshared. */
    return TRUE;
  }

  gboolean shared = FALSE;
  const gchar *name;
  while (!shared && (name = g_dir_read_name(dir)) != NULL) {
    if (strstr(name, "-port") == NULL || g_strcmp0(name, self_name) == 0) {
      continue;
    }
    gchar *child = g_build_filename(hub_iface, name, "device", NULL);
    shared = g_file_test(child, G_FILE_TEST_EXISTS);
    g_free(child);
  }

  g_dir_close(dir);
  g_free(hub_iface);
  g_free(self_name);
  return shared;
}

UsbDeepRecoveryOutcome usb_deep_recovery_run(const UsbRecoveryTarget *target,
                                             guint hold_ms) {
  if (target == NULL || target->port_path == NULL
      || target->device_path == NULL) {
    return USB_DEEP_RECOVERY_NO_TARGET;
  }

  /* Rung 1 — the device object survived the reset, so the cheap logical
   * re-probe is available and is preferred: it disturbs exactly one device and
   * leaves the port's power state alone. The identity re-check is what stops it
   * firing at a DIFFERENT device that has since taken the address. */
  UsbDeepRecoveryOutcome error = USB_DEEP_RECOVERY_FAILED;

  gchar *occupant = g_build_filename(target->port_path, "device", NULL);
  gboolean port_occupied = g_file_test(occupant, G_FILE_TEST_EXISTS);
  gboolean occupant_is_ours =
      port_occupied
      && identity_matches(occupant, target->vendor_id, target->product_id);
  g_free(occupant);
  if (port_occupied && !occupant_is_ours) {
    return USB_DEEP_RECOVERY_REFUSED_IDENTITY;
  }

  if (g_file_test(target->device_path, G_FILE_TEST_IS_DIR)) {
    if (!identity_matches(target->device_path, target->vendor_id,
                          target->product_id)) {
      return USB_DEEP_RECOVERY_REFUSED_IDENTITY;
    }
    if (!write_attr(target->device_path, "authorized", "0", &error)
        || !write_attr(target->device_path, "authorized", "1", &error)) {
      return error;
    }
    return USB_DEEP_RECOVERY_DEVICE_REPROBED;
  }

  /* Rung 2 — no device object at the captured path, which is the post-
   * `error -71` shape. Only the port is left. */
  if (hub_carries_other_devices(target->port_path)) {
    return USB_DEEP_RECOVERY_REFUSED_SHARED_HUB;
  }

  if (!write_attr(target->port_path, "disable", "1", &error)) {
    return error;
  }

  if (hold_ms > 0) {
    g_usleep((gulong) hold_ms * G_TIME_SPAN_MILLISECOND);
  }

  /* The restore is not optional: a port left disabled is worse than the wedge
   * it was meant to clear, because a replug cannot revive it either. */
  if (!write_attr(target->port_path, "disable", "0", &error)) {
    return error;
  }
  return USB_DEEP_RECOVERY_PORT_CYCLED;
}

const gchar *usb_deep_recovery_outcome_name(UsbDeepRecoveryOutcome outcome) {
  switch (outcome) {
    case USB_DEEP_RECOVERY_NO_TARGET:
      return "no-target";
    case USB_DEEP_RECOVERY_REFUSED_SHARED_HUB:
      return "refused-shared-hub";
    case USB_DEEP_RECOVERY_REFUSED_IDENTITY:
      return "refused-identity";
    case USB_DEEP_RECOVERY_UNAVAILABLE:
      return "unavailable";
    case USB_DEEP_RECOVERY_DENIED:
      return "denied";
    case USB_DEEP_RECOVERY_FAILED:
      return "failed";
    case USB_DEEP_RECOVERY_DEVICE_REPROBED:
      return "device-reprobed";
    case USB_DEEP_RECOVERY_PORT_CYCLED:
      return "port-cycled";
  }
  return "unknown";
}
