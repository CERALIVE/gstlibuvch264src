/* Standalone reproduction harness for CVE-2026-1991 (libuvc Issue #300):
 * a NULL-pointer / out-of-bounds dereference in libuvc's uvc_scan_streaming()
 * (src/device.c) triggered by a malformed USB VideoControl descriptor.
 *
 * THE BUG
 * -------
 * uvc_parse_vc_header() walks the VideoControl HEADER's baInterfaceNr array and
 * forwards each attacker-controlled byte straight into uvc_scan_streaming():
 *
 *     for (i = 12; i < block_size; ++i)
 *       uvc_scan_streaming(dev, info, block[i]);   // block[i] is 0..255, unchecked
 *
 * uvc_scan_streaming() then did, with NO bounds check:
 *
 *     if_desc = &(info->config->interface[interface_idx].altsetting[0]);
 *     buffer  = if_desc->extra;                    // <-- dereference
 *
 * A crafted descriptor reaches the dereference along two paths:
 *   (1) interface_idx >= config->bNumInterfaces  -> the interface[] array is
 *       indexed out of bounds, then the garbage libusb_interface found there is
 *       dereferenced (.altsetting[0]).
 *   (2) the referenced interface legitimately has num_altsetting == 0 /
 *       altsetting == NULL  -> altsetting[0] dereferences NULL.
 *
 * THE HARNESS
 * -----------
 * Both faults are reproduced deterministically WITHOUT any USB hardware by
 * handing uvc_scan_streaming() a hand-built libusb_config_descriptor:
 *
 *   "oob"  - a config that claims one interface, with the 1-element interface[]
 *            array butted against a PROT_NONE guard page so that reading
 *            interface[1] (== interface[bNumInterfaces]) faults. This is exactly
 *            the boundary the bounds guard protects.
 *   "null" - an in-range interface (index 0) whose altsetting is NULL, so
 *            altsetting[0] dereferences NULL.
 *
 * On the UNPATCHED fork each case SIGSEGVs (RED). On the patched fork the guards
 * reject the malformed index / missing altsetting and return
 * UVC_ERROR_INVALID_DEVICE, so the harness prints the surfaced error and exits 0
 * (GREEN). Run under AddressSanitizer for a clean fault backtrace.
 *
 * SELF-CONTAINED ON PURPOSE
 * -------------------------
 * libuvc's internal uvc_device_info_t is not part of the installed public
 * headers. uvc_scan_streaming() reads only info->config (its first member)
 * before the (un)guarded dereference and, on every path this harness drives,
 * returns or faults before touching any later member. So we mirror just the
 * leading `config` field and call the exported symbol through void* parameters,
 * which are ABI-identical to the real uvc_device_info_t* / uvc_device_t*. This
 * keeps the one harness source buildable against either a system libuvc or the
 * vendored fork, with no dependency on internal headers.
 */

#include <libusb.h>
#include <libuvc/libuvc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* libuvc internal entry point. Real signature:
 *   uvc_error_t uvc_scan_streaming(uvc_device_t *, uvc_device_info_t *, int);
 * Declared here with void* pointer parameters (ABI-identical) so the harness
 * needs neither the internal uvc_device_info_t nor uvc_device_t layout. */
extern uvc_error_t uvc_scan_streaming (void *dev, void *info, int interface_idx);

/* Mirror of the only field uvc_scan_streaming() reads before the vulnerable
 * dereference. `config` MUST stay first (offset 0), matching the internal
 * uvc_device_info_t. */
typedef struct
{
  struct libusb_config_descriptor *config;
} fake_info_t;

/* Path (2): in-range interface index, but the interface has no altsetting.
 * Unpatched: interface[0].altsetting[0] dereferences NULL -> SIGSEGV (RED).
 * Patched:   num_altsetting < 1 is rejected -> UVC_ERROR_INVALID_DEVICE (GREEN). */
static int
run_null_altsetting (void)
{
  struct libusb_interface *iface = calloc (1, sizeof (*iface));
  iface->altsetting = NULL;     /* malformed: referenced interface has no altsetting */
  iface->num_altsetting = 0;

  struct libusb_config_descriptor *config = calloc (1, sizeof (*config));
  config->bNumInterfaces = 1;
  config->interface = iface;

  fake_info_t info = {.config = config };

  fprintf (stderr,
      "null-altsetting: calling uvc_scan_streaming(idx=0) on a config whose "
      "interface[0].altsetting is NULL ...\n");

  /* Index 0 is in range; the NULL altsetting is the trap. */
  uvc_error_t ret = uvc_scan_streaming (NULL, &info, 0);

  /* Reached only on the patched fork. */
  printf ("null-altsetting: returned %d (%s)\n", ret, uvc_strerror (ret));

  int rc = 0;
  if (ret != UVC_ERROR_INVALID_DEVICE)
    {
      fprintf (stderr,
          "FAIL: expected UVC_ERROR_INVALID_DEVICE (%d), got %d\n",
          UVC_ERROR_INVALID_DEVICE, ret);
      rc = 1;
    }

  free (config);
  free (iface);
  return rc;
}

/* Path (1): interface index out of range. The 1-element interface[] array is
 * placed so interface[1] starts on a PROT_NONE guard page.
 * Unpatched: reading interface[1] faults -> SIGSEGV (RED).
 * Patched:   interface_idx >= bNumInterfaces is rejected before any read of
 *            interface[] -> UVC_ERROR_INVALID_DEVICE (GREEN). */
static int
run_oob_index (void)
{
  long pg = sysconf (_SC_PAGESIZE);
  if (pg <= 0)
    {
      perror ("sysconf");
      return 2;
    }

  /* Two pages; the second is made inaccessible. */
  char *region = mmap (NULL, (size_t) pg * 2, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED)
    {
      perror ("mmap");
      return 2;
    }
  if (mprotect (region + pg, (size_t) pg, PROT_NONE) != 0)
    {
      perror ("mprotect");
      munmap (region, (size_t) pg * 2);
      return 2;
    }

  /* Put the single valid interface immediately before the guard page so that
   * interface[1] == (region + pg) lands in the inaccessible page. */
  size_t isz = sizeof (struct libusb_interface);
  struct libusb_interface *iface =
      (struct libusb_interface *) (region + pg - isz);
  memset (iface, 0, isz);

  /* Give interface[0] a real altsetting so the ONLY defect is the out-of-range
   * index, not a NULL altsetting. */
  static struct libusb_interface_descriptor altset0;
  memset (&altset0, 0, sizeof (altset0));
  iface->altsetting = &altset0;
  iface->num_altsetting = 1;

  struct libusb_config_descriptor config;
  memset (&config, 0, sizeof (config));
  config.bNumInterfaces = 1;
  config.interface = iface;

  fake_info_t info = {.config = &config };

  fprintf (stderr,
      "oob-index: calling uvc_scan_streaming(idx=1) on a config with "
      "bNumInterfaces=1 (interface[1] is a guard page) ...\n");

  /* interface_idx == bNumInterfaces: out of range. */
  uvc_error_t ret = uvc_scan_streaming (NULL, &info, 1);

  /* Reached only on the patched fork. */
  printf ("oob-index: returned %d (%s)\n", ret, uvc_strerror (ret));

  int rc = 0;
  if (ret != UVC_ERROR_INVALID_DEVICE)
    {
      fprintf (stderr,
          "FAIL: expected UVC_ERROR_INVALID_DEVICE (%d), got %d\n",
          UVC_ERROR_INVALID_DEVICE, ret);
      rc = 1;
    }

  munmap (region, (size_t) pg * 2);
  return rc;
}

int
main (int argc, char **argv)
{
  const char *which = (argc > 1) ? argv[1] : "null";

  if (strcmp (which, "oob") == 0)
    return run_oob_index ();
  if (strcmp (which, "null") == 0)
    return run_null_altsetting ();

  fprintf (stderr, "usage: %s {oob|null}\n", argv[0]);
  return 2;
}
