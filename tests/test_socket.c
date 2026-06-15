/* Opt-in hardened control-socket tests for the libuvch264src element (Task 16).
 *
 * Same self-contained shape as test_device_select.c / test_ptz.c: the element
 * translation units, the libuvc mock, and the driver are linked into ONE
 * executable with the element type registered statically, so start()/stop() run
 * the REAL socket bind/unbind against the filesystem (the mock only stands in for
 * libuvc, not for socket(2)). Each gst-check test is its own ctest entry, and
 * each creates a private 0700 $XDG_RUNTIME_DIR so the cases never collide under
 * `ctest -j`.
 *
 * Covered:
 *   test_socket_default_off  control-socket defaults FALSE: start() binds no
 *                            socket, the path property stays NULL, the runtime
 *                            dir gains no socket file.
 *   test_socket_hardened     two enabled instances: each binds a per-instance
 *                            path under $XDG_RUNTIME_DIR, mode 0600, the two
 *                            paths differ, and both are unlinked on stop().
 *
 * Results are captured while the pipeline is live and only asserted after
 * teardown: with CK_FORK=no a failing fail_unless longjmps out and would
 * otherwise leave the control thread keeping the process alive until the ctest
 * timeout (see test_device_select.c).
 */

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <glib/gstdio.h>
#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

static void
setup (void)
{
  /* A client that disconnects before reading its reply would otherwise drop a
   * SIGPIPE on the element's response write() and kill the whole test process;
   * ignore it so a half-closed client surfaces as a benign write() error. */
  signal (SIGPIPE, SIG_IGN);

  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }

  mock_uvc_reset ();
}

/* A private runtime dir per test so the cases stay isolated; g_dir_make_tmp
 * creates it 0700, exactly the protection $XDG_RUNTIME_DIR carries in prod. */
static gchar *
make_runtime_dir (void)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("uvc-sock-XXXXXX", &err);
  fail_unless (dir != NULL, "could not create runtime dir: %s",
      err ? err->message : "(unknown)");
  g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
  return dir;
}

static int
count_sockets_in_dir (const gchar * dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d == NULL)
    return -1;

  int n = 0;
  const gchar *name;
  while ((name = g_dir_read_name (d)) != NULL) {
    gchar *full = g_build_filename (dir, name, NULL);
    GStatBuf st;
    if (g_lstat (full, &st) == 0 && S_ISSOCK (st.st_mode))
      n++;
    g_free (full);
  }
  g_dir_close (d);
  return n;
}

static GstElement *
build_pipeline (const gchar * name, GstElement ** src_out)
{
  GstElement *pipeline = gst_pipeline_new (name);
  GstElement *src = gst_element_factory_make ("libuvch264src", NULL);
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "index", "0", NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  *src_out = src;
  return pipeline;
}

/* Connect to the element's bound control socket, write cmd_len raw bytes of cmd
 * (so a test can send oversized/unterminated payloads verbatim), then drain the
 * reply to EOF and hand it back NUL-terminated. Returns NULL only if the connect
 * itself fails. The control thread's accepted client socket is blocking, so a
 * single read on its side returns whatever we wrote here; we read the reply with
 * MSG-safe writes so a torn-down server never SIGPIPEs us. */
static gchar *
socket_send_command (const gchar * path, const char *cmd, size_t cmd_len)
{
  int fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return NULL;

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  g_strlcpy (addr.sun_path, path, sizeof (addr.sun_path));

  if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
    close (fd);
    return NULL;
  }

  size_t off = 0;
  while (off < cmd_len) {
    ssize_t n = send (fd, cmd + off, cmd_len - off, MSG_NOSIGNAL);
    if (n <= 0)
      break;
    off += (size_t) n;
  }

  GString *reply = g_string_new (NULL);
  char rbuf[512];
  ssize_t n;
  while ((n = read (fd, rbuf, sizeof (rbuf))) > 0)
    g_string_append_len (reply, rbuf, n);

  close (fd);
  return g_string_free (reply, FALSE);
}

GST_START_TEST (test_socket_default_off)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline ("off", &src);

  GstStateChangeReturn sret =
      gst_element_set_state (pipeline, GST_STATE_PAUSED);

  gboolean enabled = TRUE;
  gchar *path = NULL;
  g_object_get (src, "control-socket", &enabled, "control-socket-path", &path,
      NULL);
  int sock_files = count_sockets_in_dir (runtime);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (sret != GST_STATE_CHANGE_FAILURE, "start() should have succeeded");
  fail_unless (!enabled, "control-socket must default to FALSE");
  fail_unless (path == NULL,
      "control-socket-path must be NULL when disabled, got '%s'",
      path ? path : "(null)");
  fail_unless (sock_files == 0,
      "no socket must be bound when disabled, found %d in %s", sock_files,
      runtime);

  g_free (path);
  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

GST_START_TEST (test_socket_hardened)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);

  GstElement *srcA = NULL, *srcB = NULL;
  GstElement *pipeA = build_pipeline ("a", &srcA);
  GstElement *pipeB = build_pipeline ("b", &srcB);
  g_object_set (srcA, "control-socket", TRUE, NULL);
  g_object_set (srcB, "control-socket", TRUE, NULL);

  GstStateChangeReturn sretA =
      gst_element_set_state (pipeA, GST_STATE_PAUSED);
  GstStateChangeReturn sretB =
      gst_element_set_state (pipeB, GST_STATE_PAUSED);

  gchar *pathA = NULL, *pathB = NULL;
  g_object_get (srcA, "control-socket-path", &pathA, NULL);
  g_object_get (srcB, "control-socket-path", &pathB, NULL);

  GStatBuf stA, stB;
  int rA = (pathA != NULL) ? g_lstat (pathA, &stA) : -1;
  int rB = (pathB != NULL) ? g_lstat (pathB, &stB) : -1;
  gboolean issockA = (rA == 0) && S_ISSOCK (stA.st_mode);
  gboolean issockB = (rB == 0) && S_ISSOCK (stB.st_mode);
  unsigned int modeA = (rA == 0) ? (stA.st_mode & 0777) : 0;
  unsigned int modeB = (rB == 0) ? (stB.st_mode & 0777) : 0;
  gboolean underA = (pathA != NULL) && g_str_has_prefix (pathA, runtime);
  gboolean underB = (pathB != NULL) && g_str_has_prefix (pathB, runtime);
  gboolean distinct =
      (pathA != NULL) && (pathB != NULL) && (g_strcmp0 (pathA, pathB) != 0);

  gst_element_set_state (pipeA, GST_STATE_NULL);
  gst_element_set_state (pipeB, GST_STATE_NULL);

  GStatBuf gone;
  gboolean goneA = (pathA != NULL) && (g_lstat (pathA, &gone) != 0);
  gboolean goneB = (pathB != NULL) && (g_lstat (pathB, &gone) != 0);

  gst_object_unref (pipeA);
  gst_object_unref (pipeB);

  fail_unless (sretA != GST_STATE_CHANGE_FAILURE
      && sretB != GST_STATE_CHANGE_FAILURE,
      "start() should succeed for both instances");
  fail_unless (pathA != NULL && pathB != NULL,
      "each enabled instance must resolve a path (A='%s' B='%s')",
      pathA ? pathA : "(null)", pathB ? pathB : "(null)");
  fail_unless (underA && underB,
      "socket paths must live under $XDG_RUNTIME_DIR (%s): A='%s' B='%s'",
      runtime, pathA, pathB);
  fail_unless (issockA && issockB, "bound paths must be sockets");
  fail_unless (modeA == 0600 && modeB == 0600,
      "sockets must be mode 0600 (A=%o B=%o)", modeA, modeB);
  fail_unless (distinct,
      "two instances must not collide on one path: '%s'", pathA);
  fail_unless (goneA && goneB, "sockets must be unlinked on stop()");

  g_free (pathA);
  g_free (pathB);
  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

/* (b) Well-formed commands parse correctly under explicit framing. A line-
 * oriented client terminates each command with LF or CRLF; the parser must
 * strip the terminator before dispatch (the old parser left it inside the
 * strcmp(), so "GET_CAPABILITIES\n" fell through to "Unknown command"). A bare
 * command with no terminator (single-shot write) must still parse. Captured live
 * and asserted only after teardown, matching the other cases. */
GST_START_TEST (test_socket_command_framing)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline ("framing", &src);
  g_object_set (src, "control-socket", TRUE, NULL);

  GstStateChangeReturn sret =
      gst_element_set_state (pipeline, GST_STATE_PAUSED);

  gchar *path = NULL;
  g_object_get (src, "control-socket-path", &path, NULL);

  gboolean live = (sret != GST_STATE_CHANGE_FAILURE) && (path != NULL);
  gchar *caps = live
      ? socket_send_command (path, "GET_CAPABILITIES\n", 17) : NULL;
  gchar *pos_crlf = live
      ? socket_send_command (path, "GET_POSITION\r\n", 14) : NULL;
  gchar *pos_bare = live
      ? socket_send_command (path, "GET_POSITION", 12) : NULL;

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (sret != GST_STATE_CHANGE_FAILURE, "start() should have succeeded");
  fail_unless (path != NULL, "enabled instance must resolve a path");
  fail_unless (caps != NULL && g_str_has_prefix (caps, "CAPABILITIES:"),
      "LF-terminated GET_CAPABILITIES must parse, got '%s'",
      caps ? caps : "(null)");
  fail_unless (pos_crlf != NULL && g_str_has_prefix (pos_crlf, "OK pan="),
      "CRLF-terminated GET_POSITION must parse, got '%s'",
      pos_crlf ? pos_crlf : "(null)");
  fail_unless (pos_bare != NULL && g_str_has_prefix (pos_bare, "OK pan="),
      "unterminated GET_POSITION must still parse, got '%s'",
      pos_bare ? pos_bare : "(null)");

  g_free (caps);
  g_free (pos_crlf);
  g_free (pos_bare);
  g_free (path);
  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

/* (a) Oversized / garbage input is rejected cleanly: no crash, no wrong command
 * executed. An oversized blob whose PREFIX is a valid sscanf command ("PAN_TILT
 * 10 20 ...") would make the old parser latch the two ints and drive the device;
 * the framed parser must reject it (buffer fills with no terminator) and never
 * touch the device. Pure garbage with a terminator must frame to "Unknown
 * command". A normal command after the bad ones proves the thread survived. */
GST_START_TEST (test_socket_command_oversized)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);
  /* Wide ranges so the injected 10/20 are unambiguously in-range: were the
   * oversized PAN_TILT dispatched (old parser), the device WOULD latch them. */
  mock_uvc_set_ptz_range (-648000, 648000, -648000, 648000, 0, 65535);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline ("oversized", &src);
  g_object_set (src, "control-socket", TRUE, NULL);

  GstStateChangeReturn sret =
      gst_element_set_state (pipeline, GST_STATE_PAUSED);

  gchar *path = NULL;
  g_object_get (src, "control-socket-path", &path, NULL);
  gboolean live = (sret != GST_STATE_CHANGE_FAILURE) && (path != NULL);

  /* 600 bytes >> the element's 256 B read buffer, no terminator anywhere. */
  const gsize big_len = 600;
  char *big = g_malloc (big_len);
  memset (big, 'X', big_len);
  memcpy (big, "PAN_TILT 10 20 ", 15);
  gchar *big_resp = live ? socket_send_command (path, big, big_len) : NULL;

  gchar *garbage_resp = live
      ? socket_send_command (path, "\x01\x02\x03 not-a-command\n", 19) : NULL;

  gchar *ok_resp = live
      ? socket_send_command (path, "GET_POSITION\n", 13) : NULL;

  /* Did the oversized PAN_TILT reach the device? It must not have. */
  int32_t last_pan = 0, last_tilt = 0;
  mock_uvc_get_last_pantilt (&last_pan, &last_tilt);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (sret != GST_STATE_CHANGE_FAILURE, "start() should have succeeded");
  fail_unless (path != NULL, "enabled instance must resolve a path");
  fail_unless (big_resp != NULL && !g_str_has_prefix (big_resp, "OK pan="),
      "oversized PAN_TILT must NOT be dispatched, got '%s'",
      big_resp ? big_resp : "(null)");
  fail_unless (big_resp != NULL && g_str_has_prefix (big_resp, "ERROR"),
      "oversized command must be rejected with an ERROR, got '%s'",
      big_resp ? big_resp : "(null)");
  fail_unless (last_pan != 10 && last_tilt != 20,
      "oversized PAN_TILT must not drive the device (pan=%d tilt=%d)",
      (int) last_pan, (int) last_tilt);
  fail_unless (garbage_resp != NULL && g_str_has_prefix (garbage_resp, "ERROR"),
      "framed garbage must be rejected cleanly, got '%s'",
      garbage_resp ? garbage_resp : "(null)");
  fail_unless (ok_resp != NULL && g_str_has_prefix (ok_resp, "OK pan="),
      "control thread must survive bad input and still serve, got '%s'",
      ok_resp ? ok_resp : "(null)");

  g_free (big);
  g_free (big_resp);
  g_free (garbage_resp);
  g_free (ok_resp);
  g_free (path);
  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

/* (c) Tearing the element down while the control thread is mid-loop must not
 * crash or race. Each round connects a client (waking the thread out of select()
 * into accept()/read()) and then transitions to NULL, so stop() flips
 * control_running and unbind() closes the listening fd exactly as the thread is
 * using it. Repeated rounds widen the window. Under TSan this is the gate for the
 * control_socket/control_running lifecycle race; here it asserts no crash and a
 * clean per-round unbind. */
GST_START_TEST (test_socket_close_during_select)
{
  gchar *runtime = make_runtime_dir ();
  mock_uvc_set_device_count (1);

  gboolean all_started = TRUE;
  gboolean all_gone = TRUE;

  for (int i = 0; i < 6; i++) {
    GstElement *src = NULL;
    GstElement *pipeline = build_pipeline ("race", &src);
    g_object_set (src, "control-socket", TRUE, NULL);

    GstStateChangeReturn sret =
        gst_element_set_state (pipeline, GST_STATE_PAUSED);
    if (sret == GST_STATE_CHANGE_FAILURE)
      all_started = FALSE;

    gchar *path = NULL;
    g_object_get (src, "control-socket-path", &path, NULL);

    /* Connect and send a partial, unterminated command, then leave the socket
     * open: the server thread is parked in accept()/read() right as we tear
     * down, maximising overlap with stop()/unbind(). */
    int cfd = -1;
    if (path != NULL) {
      cfd = socket (AF_UNIX, SOCK_STREAM, 0);
      if (cfd >= 0) {
        struct sockaddr_un addr;
        memset (&addr, 0, sizeof (addr));
        addr.sun_family = AF_UNIX;
        g_strlcpy (addr.sun_path, path, sizeof (addr.sun_path));
        if (connect (cfd, (struct sockaddr *) &addr, sizeof (addr)) == 0)
          send (cfd, "GET_PO", 6, MSG_NOSIGNAL);
      }
    }

    gst_element_set_state (pipeline, GST_STATE_NULL);

    if (cfd >= 0)
      close (cfd);

    GStatBuf st;
    if (path != NULL && g_lstat (path, &st) == 0)
      all_gone = FALSE;

    gst_object_unref (pipeline);
    g_free (path);
  }

  fail_unless (all_started, "every enabled instance must start");
  fail_unless (all_gone, "every socket must be unlinked after stop()");

  g_rmdir (runtime);
  g_free (runtime);
}

GST_END_TEST;

static Suite *
socket_suite (void)
{
  Suite *s = suite_create ("libuvch264src-socket");
  TCase *tc = tcase_create ("socket");

  tcase_set_timeout (tc, 60);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_socket_default_off);
  tcase_add_test (tc, test_socket_hardened);
  tcase_add_test (tc, test_socket_command_framing);
  tcase_add_test (tc, test_socket_command_oversized);
  tcase_add_test (tc, test_socket_close_during_select);

  return s;
}

GST_CHECK_MAIN (socket);
