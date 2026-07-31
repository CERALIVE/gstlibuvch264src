/* Deep USB recovery helper (Task 11).
 *
 * Every case drives the REAL implementation against a synthetic sysfs tree
 * built in a temp dir, so the sysfs layout these rungs depend on is asserted
 * rather than assumed. The layout mirrors what was measured on the board:
 * `authorized` exists only on a DEVICE directory and `disable` only on a PORT
 * directory, and a device points at its port through a `port` symlink.
 *
 *   <root>/bus/usb/devices/9-1            -> symlink to the device dir
 *   <root>/devices/usb9/9-0:1.0/usb9-port1
 *
 * A real board cannot be used here: the suite is hardware-independent, and the
 * writes under test are root-only and destructive. What hardware DID establish
 * is recorded in task-11-board-proof.md.
 */

#include <gst/check/gstcheck.h>

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "usb_port_recovery.h"

typedef struct
{
  gchar *root;                  /* synthetic sysfs root */
  gchar *devices_dir;           /* <root>/bus/usb/devices */
  gchar *device_dir;            /* the real device directory */
  gchar *hub_iface;             /* the hub interface dir that owns the ports */
  gchar *port_dir;              /* our port */
} SysfsTree;

static void
write_file (const gchar * dir, const gchar * name, const gchar * value)
{
  gchar *path = g_build_filename (dir, name, NULL);
  fail_unless (g_file_set_contents (path, value, -1, NULL),
      "could not write %s", path);
  g_free (path);
}

static gchar *
read_file (const gchar * dir, const gchar * name)
{
  gchar *path = g_build_filename (dir, name, NULL);
  gchar *out = NULL;
  if (!g_file_get_contents (path, &out, NULL, NULL))
    out = NULL;
  g_free (path);
  return out;
}

/* Build the minimum tree the helper walks: one hub, `n_ports` ports, and a
 * device on port 1 reachable both as <devices>/9-1 and through the port's
 * `device` link. */
static void
sysfs_tree_build (SysfsTree * t, guint n_ports)
{
  GError *err = NULL;
  t->root = g_dir_make_tmp ("libuvch264src-sysfs-XXXXXX", &err);
  fail_unless (t->root != NULL, "could not make temp sysfs root: %s",
      err ? err->message : "?");

  t->devices_dir = g_build_filename (t->root, "bus", "usb", "devices", NULL);
  fail_unless (g_mkdir_with_parents (t->devices_dir, 0755) == 0);

  t->device_dir = g_build_filename (t->root, "devices", "usb9", "9-1", NULL);
  fail_unless (g_mkdir_with_parents (t->device_dir, 0755) == 0);
  write_file (t->device_dir, "busnum", "9\n");
  write_file (t->device_dir, "devnum", "4\n");
  write_file (t->device_dir, "idVendor", "2ca3\n");
  write_file (t->device_dir, "idProduct", "0023\n");
  write_file (t->device_dir, "authorized", "1\n");

  t->hub_iface =
      g_build_filename (t->root, "devices", "usb9", "9-0:1.0", NULL);
  fail_unless (g_mkdir_with_parents (t->hub_iface, 0755) == 0);

  for (guint i = 1; i <= n_ports; i++) {
    gchar *name = g_strdup_printf ("usb9-port%u", i);
    gchar *dir = g_build_filename (t->hub_iface, name, NULL);
    fail_unless (g_mkdir_with_parents (dir, 0755) == 0);
    write_file (dir, "disable", "0\n");
    if (i == 1) {
      t->port_dir = g_strdup (dir);
      gchar *link = g_build_filename (dir, "device", NULL);
      fail_unless (symlink (t->device_dir, link) == 0);
      g_free (link);
    }
    g_free (dir);
    g_free (name);
  }

  gchar *alias = g_build_filename (t->devices_dir, "9-1", NULL);
  fail_unless (symlink (t->device_dir, alias) == 0);
  g_free (alias);

  gchar *port_link = g_build_filename (t->device_dir, "port", NULL);
  fail_unless (symlink (t->port_dir, port_link) == 0);
  g_free (port_link);
}

/* Add a second, unrelated device on `port_num` — the shared-hub shape. */
static void
sysfs_tree_add_neighbour (SysfsTree * t, guint port_num)
{
  gchar *other = g_build_filename (t->root, "devices", "usb9", "9-2", NULL);
  fail_unless (g_mkdir_with_parents (other, 0755) == 0);
  write_file (other, "idVendor", "1a40\n");
  write_file (other, "idProduct", "0101\n");

  gchar *name = g_strdup_printf ("usb9-port%u", port_num);
  gchar *dir = g_build_filename (t->hub_iface, name, NULL);
  gchar *link = g_build_filename (dir, "device", NULL);
  fail_unless (symlink (other, link) == 0);
  g_free (link);
  g_free (dir);
  g_free (name);
  g_free (other);
}

/* Detach the device exactly as a failed re-enumeration does: the device object
 * and every path that reaches it disappear; the port directory stays. */
static void
sysfs_tree_unplug (SysfsTree * t)
{
  gchar *alias = g_build_filename (t->devices_dir, "9-1", NULL);
  gchar *occupant = g_build_filename (t->port_dir, "device", NULL);
  fail_unless (g_remove (alias) == 0);
  fail_unless (g_remove (occupant) == 0);
  g_free (alias);
  g_free (occupant);

  const gchar *attrs[] = { "busnum", "devnum", "idVendor", "idProduct",
    "authorized", "port"
  };
  for (guint i = 0; i < G_N_ELEMENTS (attrs); i++) {
    gchar *p = g_build_filename (t->device_dir, attrs[i], NULL);
    g_remove (p);
    g_free (p);
  }
  fail_unless (g_remove (t->device_dir) == 0);
}

static void
sysfs_tree_free (SysfsTree * t)
{
  gchar *cmd[] = { (gchar *) "rm", (gchar *) "-rf", t->root, NULL };
  g_spawn_sync (NULL, cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
      NULL, NULL);
  g_clear_pointer (&t->root, g_free);
  g_clear_pointer (&t->devices_dir, g_free);
  g_clear_pointer (&t->device_dir, g_free);
  g_clear_pointer (&t->hub_iface, g_free);
  g_clear_pointer (&t->port_dir, g_free);
}

/* ------------------------------------------------------------------------- */
/* Capture                                                                    */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_capture_resolves_device_and_port)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  UsbRecoveryTarget target = { 0 };
  fail_unless (usb_recovery_target_capture (t.root, 9, 4, &target),
      "a device at bus 9 address 4 must be resolvable");
  fail_unless (target.vendor_id == 0x2ca3 && target.product_id == 0x0023,
      "capture must record the identity, got %04x:%04x", target.vendor_id,
      target.product_id);
  fail_unless (target.port_path != NULL
      && strstr (target.port_path, "usb9-port1") != NULL,
      "capture must resolve the port through the `port` symlink, got %s",
      target.port_path ? target.port_path : "(null)");

  usb_recovery_target_clear (&target);
  fail_unless (target.device_path == NULL && target.port_path == NULL);
  sysfs_tree_free (&t);
}

GST_END_TEST;

GST_START_TEST (test_capture_rejects_a_foreign_address)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  UsbRecoveryTarget target = { 0 };
  fail_if (usb_recovery_target_capture (t.root, 9, 7, &target),
      "an address no device holds must not resolve");
  fail_unless (target.port_path == NULL,
      "a failed capture must leave nothing to act on");

  sysfs_tree_free (&t);
}

GST_END_TEST;

GST_START_TEST (test_capture_requires_the_port_link)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  gchar *port_link = g_build_filename (t.device_dir, "port", NULL);
  fail_unless (g_remove (port_link) == 0);
  g_free (port_link);

  UsbRecoveryTarget target = { 0 };
  fail_if (usb_recovery_target_capture (t.root, 9, 4, &target),
      "without a port link there is no path that survives the reset, so the "
      "capture must fail rather than hand back a device-only target");

  sysfs_tree_free (&t);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* Rung selection                                                             */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_surviving_device_takes_the_device_rung)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  UsbRecoveryTarget target = { 0 };
  fail_unless (usb_recovery_target_capture (t.root, 9, 4, &target));

  UsbDeepRecoveryOutcome rc = usb_deep_recovery_run (&target, 0);
  fail_unless (rc == USB_DEEP_RECOVERY_DEVICE_REPROBED,
      "a device that survived must take the cheap device-level rung, got %s",
      usb_deep_recovery_outcome_name (rc));

  gchar *authorized = read_file (t.device_dir, "authorized");
  fail_unless (g_strcmp0 (authorized, "1") == 0,
      "the re-probe must leave the device AUTHORIZED, got \"%s\"",
      authorized ? authorized : "(null)");
  g_free (authorized);

  gchar *disable = read_file (t.port_dir, "disable");
  fail_unless (g_strcmp0 (disable, "0\n") == 0,
      "the port must not be touched while a device rung was available, "
      "disable is now \"%s\"", disable ? disable : "(null)");
  g_free (disable);

  usb_recovery_target_clear (&target);
  sysfs_tree_free (&t);
}

GST_END_TEST;

GST_START_TEST (test_vanished_device_takes_the_port_rung)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  UsbRecoveryTarget target = { 0 };
  fail_unless (usb_recovery_target_capture (t.root, 9, 4, &target));
  /* The post-`error -71` shape: the device object is gone, the port remains. */
  sysfs_tree_unplug (&t);

  UsbDeepRecoveryOutcome rc = usb_deep_recovery_run (&target, 0);
  fail_unless (rc == USB_DEEP_RECOVERY_PORT_CYCLED,
      "with no device object left the port rung is the only one addressable, "
      "got %s", usb_deep_recovery_outcome_name (rc));

  gchar *disable = read_file (t.port_dir, "disable");
  fail_unless (g_strcmp0 (disable, "0") == 0,
      "the cycle must RESTORE the port; a port left disabled cannot even be "
      "recovered by a replug. disable is now \"%s\"",
      disable ? disable : "(null)");
  g_free (disable);

  usb_recovery_target_clear (&target);
  sysfs_tree_free (&t);
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* Target validation                                                          */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_hub_with_another_device_is_refused)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 4);
  sysfs_tree_add_neighbour (&t, 3);

  UsbRecoveryTarget target = { 0 };
  fail_unless (usb_recovery_target_capture (t.root, 9, 4, &target));
  sysfs_tree_unplug (&t);

  UsbDeepRecoveryOutcome rc = usb_deep_recovery_run (&target, 0);
  fail_unless (rc == USB_DEEP_RECOVERY_REFUSED_SHARED_HUB,
      "a hub carrying another device may be switching power in ganged mode, "
      "so the cycle must be refused, got %s",
      usb_deep_recovery_outcome_name (rc));

  gchar *disable = read_file (t.port_dir, "disable");
  fail_unless (g_strcmp0 (disable, "0\n") == 0,
      "a refused cycle must not have written anything, disable is \"%s\"",
      disable ? disable : "(null)");
  g_free (disable);

  usb_recovery_target_clear (&target);
  sysfs_tree_free (&t);
}

GST_END_TEST;

GST_START_TEST (test_foreign_device_on_the_port_is_refused)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  UsbRecoveryTarget target = { 0 };
  fail_unless (usb_recovery_target_capture (t.root, 9, 4, &target));

  /* Bus addresses are recycled: a different camera now answers at our path. */
  write_file (t.device_dir, "idVendor", "046d\n");
  write_file (t.device_dir, "idProduct", "0892\n");

  UsbDeepRecoveryOutcome rc = usb_deep_recovery_run (&target, 0);
  fail_unless (rc == USB_DEEP_RECOVERY_REFUSED_IDENTITY,
      "a device that is not ours must never be reset by us, got %s",
      usb_deep_recovery_outcome_name (rc));

  gchar *authorized = read_file (t.device_dir, "authorized");
  fail_unless (g_strcmp0 (authorized, "1\n") == 0,
      "a refused rung must not have written anything, authorized is \"%s\"",
      authorized ? authorized : "(null)");
  g_free (authorized);

  usb_recovery_target_clear (&target);
  sysfs_tree_free (&t);
}

GST_END_TEST;

GST_START_TEST (test_missing_target_is_reported_not_guessed)
{
  UsbRecoveryTarget empty = { 0 };
  UsbDeepRecoveryOutcome rc = usb_deep_recovery_run (&empty, 0);
  fail_unless (rc == USB_DEEP_RECOVERY_NO_TARGET,
      "an uncaptured target must be reported, not guessed at, got %s",
      usb_deep_recovery_outcome_name (rc));

  rc = usb_deep_recovery_run (NULL, 0);
  fail_unless (rc == USB_DEEP_RECOVERY_NO_TARGET);
}

GST_END_TEST;

GST_START_TEST (test_unwritable_attribute_reports_denied)
{
  SysfsTree t = { 0 };
  sysfs_tree_build (&t, 1);

  UsbRecoveryTarget target = { 0 };
  fail_unless (usb_recovery_target_capture (t.root, 9, 4, &target));

  gchar *authorized = g_build_filename (t.device_dir, "authorized", NULL);
  fail_unless (g_chmod (authorized, 0444) == 0);
  g_free (authorized);

  UsbDeepRecoveryOutcome rc = usb_deep_recovery_run (&target, 0);
  /* Running as root defeats a mode-0444 guard, so accept either the honest
   * denial or a completed re-probe — what must never happen is a silent
   * success that hides a permission problem behind a generic failure. */
  fail_unless (rc == USB_DEEP_RECOVERY_DENIED
      || rc == USB_DEEP_RECOVERY_DEVICE_REPROBED,
      "an unwritable attribute must surface as denied (or, as root, succeed), "
      "got %s", usb_deep_recovery_outcome_name (rc));

  usb_recovery_target_clear (&target);
  sysfs_tree_free (&t);
}

GST_END_TEST;

static Suite *
usb_port_recovery_suite (void)
{
  Suite *s = suite_create ("usb_port_recovery");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (s, tc);
  tcase_set_timeout (tc, 30);
  tcase_add_test (tc, test_capture_resolves_device_and_port);
  tcase_add_test (tc, test_capture_rejects_a_foreign_address);
  tcase_add_test (tc, test_capture_requires_the_port_link);
  tcase_add_test (tc, test_surviving_device_takes_the_device_rung);
  tcase_add_test (tc, test_vanished_device_takes_the_port_rung);
  tcase_add_test (tc, test_hub_with_another_device_is_refused);
  tcase_add_test (tc, test_foreign_device_on_the_port_is_refused);
  tcase_add_test (tc, test_missing_target_is_reported_not_guessed);
  tcase_add_test (tc, test_unwritable_attribute_reports_denied);

  return s;
}

GST_CHECK_MAIN (usb_port_recovery);
