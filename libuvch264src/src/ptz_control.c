#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "gstlibuvch264src_internal.h"
#include "ptz_control.h"
#include "gstlibuvch264src_error.h"

static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command);

/* Per-process counter so two elements in one process derive distinct default
 * socket paths - the pid is shared, only this disambiguates them (M9). */
static gint control_socket_seq = 0;

gboolean gst_libuvc_h264_src_control_socket_bind(GstLibuvcH264Src *self) {
    struct sockaddr_un addr;

    if (self->control_socket_path == NULL) {
        const gchar *runtime = g_getenv("XDG_RUNTIME_DIR");
        if (runtime == NULL || *runtime == '\0') {
            GST_ERROR_OBJECT(self, "XDG_RUNTIME_DIR is unset; refusing to bind the "
                             "control socket in a world-accessible location");
            return FALSE;
        }
        guint id = (guint) g_atomic_int_add(&control_socket_seq, 1);
        self->control_socket_path = g_strdup_printf("%s/libuvch264src-%d-%u.sock",
                                                    runtime, (int) getpid(), id);
    }

    if (strlen(self->control_socket_path) >= sizeof(addr.sun_path)) {
        GST_ERROR_OBJECT(self, "Control socket path too long: %s",
                         self->control_socket_path);
        return FALSE;
    }

    /* Build the listening fd on a LOCAL until it is fully ready, then publish it
     * to self->control_socket under control_mutex (the same lock the control
     * thread snapshots it through and unbind() closes it under). A half-built fd
     * is never visible to the thread, and the fd's whole lifecycle is serialised
     * on one lock so the thread can never select()/accept() a torn-down fd. */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        GST_ERROR_OBJECT(self, "Failed to create control socket");
        return FALSE;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, self->control_socket_path, sizeof(addr.sun_path));

    /* Drop a socket left by an unclean prior exit; bind to a live path fails. */
    unlink(self->control_socket_path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        GST_ERROR_OBJECT(self, "Failed to bind control socket at %s",
                         self->control_socket_path);
        close(fd);
        return FALSE;
    }

    /* Owner-only: the socket carries unauthenticated PTZ control, so restrict it
     * to this user on top of the already-0700 $XDG_RUNTIME_DIR (M9). */
    if (chmod(self->control_socket_path, S_IRUSR | S_IWUSR) < 0) {
        GST_ERROR_OBJECT(self, "Failed to chmod control socket %s to 0600",
                         self->control_socket_path);
        close(fd);
        unlink(self->control_socket_path);
        return FALSE;
    }

    if (listen(fd, 5) < 0) {
        GST_ERROR_OBJECT(self, "Failed to listen on control socket");
        close(fd);
        unlink(self->control_socket_path);
        return FALSE;
    }

    g_mutex_lock(&self->control_mutex);
    self->control_socket = fd;
    g_mutex_unlock(&self->control_mutex);

    GST_INFO_OBJECT(self, "Control socket listening on %s", self->control_socket_path);
    return TRUE;
}

void gst_libuvc_h264_src_control_socket_unbind(GstLibuvcH264Src *self) {
    /* Close and invalidate the listening fd under control_mutex so a concurrent
     * control-thread snapshot sees either a live fd or -1, never a reused one.
     * stop() has already joined the thread before calling this, so in the normal
     * path there is no contention; the lock makes the invariant hold regardless. */
    g_mutex_lock(&self->control_mutex);
    if (self->control_socket >= 0) {
        close(self->control_socket);
        self->control_socket = -1;
    }
    g_mutex_unlock(&self->control_mutex);

    if (self->control_socket_path != NULL) {
        unlink(self->control_socket_path);
    }
}

// Control socket accept loop. The socket is already created, bound and listening
// (gst_libuvc_h264_src_control_socket_bind, run in start()), which closes the old
// race where the listening fd was assigned from inside this thread.
gpointer gst_libuvc_h264_src_control_thread(gpointer data) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)data;
    char buffer[256];

    /* control_running is read atomically: stop() flips it (atomically) and
     * self-connects to break the select(), so the thread observes the shutdown
     * on its very next loop turn without relying on the wakeup landing first. */
    while (g_atomic_int_get(&self->control_running)) {
        /* Snapshot the listening fd under control_mutex. bind() publishes it and
         * unbind() closes+invalidates it under the same lock, so the snapshot is
         * either a live listening fd or -1 - never a closed-then-reused one. */
        g_mutex_lock(&self->control_mutex);
        int listen_fd = self->control_socket;
        g_mutex_unlock(&self->control_mutex);

        if (listen_fd < 0)
            break;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);

        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        int result = select(listen_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (result < 0) {
            if (g_atomic_int_get(&self->control_running))
                GST_WARNING_OBJECT(self, "Select error in control thread");
            break;
        }
        if (result == 0 || !FD_ISSET(listen_fd, &read_fds))
            continue;

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        ssize_t len = read(client_fd, buffer, sizeof(buffer) - 1);
        if (len > 0) {
            /* Explicit framing: the command runs from buffer[0] up to the first
             * line terminator (LF/CR) or embedded NUL, whichever comes first.
             * This strips the trailing newline a line-oriented client sends and
             * bounds the token strcmp()/sscanf() inspect to clean bytes. */
            size_t cmd_len = 0;
            while (cmd_len < (size_t) len &&
                   buffer[cmd_len] != '\n' &&
                   buffer[cmd_len] != '\r' &&
                   buffer[cmd_len] != '\0') {
                cmd_len++;
            }
            gboolean terminated = (cmd_len < (size_t) len);

            char *response;
            if (!terminated && len == (ssize_t) (sizeof(buffer) - 1)) {
                /* The read filled the buffer with no terminator in sight: the
                 * command is longer than we accept (or unframed garbage). Reject
                 * it cleanly instead of letting sscanf() latch onto a valid
                 * prefix of an unbounded blob and drive the device. */
                response = g_strdup("ERROR: command too long");
            } else {
                /* NUL-terminate at the ACTUAL framed length, never at
                 * sizeof(buffer)-1, so a non-terminated or embedded-NUL read can
                 * never run strcmp()/sscanf() past the command's real bytes. */
                buffer[cmd_len] = '\0';
                GST_INFO_OBJECT(self, "Received control command: %s", buffer);
                response = gst_libuvc_h264_src_process_control_command(self, buffer);
                if (response == NULL)
                    response = g_strdup("OK");
            }

            /* MSG_NOSIGNAL: a client that disconnected before reading must not
             * raise SIGPIPE and tear the whole element down. */
            if (send(client_fd, response, strlen(response), MSG_NOSIGNAL) < 0)
                GST_WARNING_OBJECT(self, "Failed to write response to control socket");
            g_free(response);
        }
        close(client_fd);
    }

    GST_DEBUG_OBJECT(self, "Control thread exiting");
    return NULL;
}

/* Routes through the shared Task-12 PTZ helpers (one clamp/gate/lock path for
 * both properties and socket). The setters lock control_mutex internally, so
 * they MUST be called without it held; GET_* snapshots cached state separately. */
static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command) {
    int pan, tilt, zoom;

    if (sscanf(command, "PAN_TILT %d %d", &pan, &tilt) == 2) {
        gboolean any = FALSE, ok = TRUE;
        if (self->pan_supported) {
            any = TRUE;
            ok = gst_libuvc_h264_src_ptz_set_pan(self, pan) && ok;
        }
        if (self->tilt_supported) {
            any = TRUE;
            ok = gst_libuvc_h264_src_ptz_set_tilt(self, tilt) && ok;
        }
        if (!any) return g_strdup("ERROR: pan/tilt not supported");
        if (!ok) return g_strdup("ERROR: pan/tilt transfer failed");

        g_mutex_lock(&self->control_mutex);
        gint p = self->pan_cur, t = self->tilt_cur;
        g_mutex_unlock(&self->control_mutex);
        return g_strdup_printf("OK pan=%d tilt=%d", p, t);
    }
    else if (sscanf(command, "ZOOM %d", &zoom) == 1) {
        if (!self->zoom_supported) return g_strdup("ERROR: zoom not supported");
        if (!gst_libuvc_h264_src_ptz_set_zoom(self, zoom))
            return g_strdup("ERROR: zoom transfer failed");

        g_mutex_lock(&self->control_mutex);
        gint z = self->zoom_cur;
        g_mutex_unlock(&self->control_mutex);
        return g_strdup_printf("OK zoom=%d", z);
    }
    else if (strcmp(command, "GET_POSITION") == 0) {
        g_mutex_lock(&self->control_mutex);
        gint p = self->pan_cur, t = self->tilt_cur, z = self->zoom_cur;
        g_mutex_unlock(&self->control_mutex);
        return g_strdup_printf("OK pan=%d tilt=%d zoom=%d", p, t, z);
    }
    else if (strcmp(command, "GET_CAPABILITIES") == 0) {
        g_mutex_lock(&self->control_mutex);
        gboolean ps = self->pan_supported, ts = self->tilt_supported, zs = self->zoom_supported;
        gint pmin = self->pan_min, pmax = self->pan_max;
        gint tmin = self->tilt_min, tmax = self->tilt_max;
        gint zmin = self->zoom_min, zmax = self->zoom_max;
        g_mutex_unlock(&self->control_mutex);

        GString *caps = g_string_new("CAPABILITIES:");
        if (ps) g_string_append_printf(caps, " pan=[%d,%d]", pmin, pmax);
        if (ts) g_string_append_printf(caps, " tilt=[%d,%d]", tmin, tmax);
        if (zs) g_string_append_printf(caps, " zoom=[%d,%d]", zmin, zmax);
        return g_string_free(caps, FALSE);
    }

    return g_strdup("ERROR: Unknown command");
}

static gint ptz_clamp(gint value, gint lo, gint hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void gst_libuvc_h264_src_ptz_probe_capabilities(GstLibuvcH264Src *self) {
    /* M6: start every axis gated off with zeroed ranges; only a fully-checked
     * probe re-enables one, so an unchecked GET_* value is never stored. */
    self->pan_supported = FALSE;
    self->tilt_supported = FALSE;
    self->zoom_supported = FALSE;
    self->ptz_supported = FALSE;
    self->pan_min = self->pan_max = self->pan_cur = 0;
    self->tilt_min = self->tilt_max = self->tilt_cur = 0;
    self->zoom_min = self->zoom_max = self->zoom_cur = 0;

    if (!self->uvc_devh) return;

    g_mutex_lock(&self->control_mutex);

    /* PanTilt is one UVC control carrying both axes. Trust the range only when
     * MIN, MAX and RES all succeed; an axis counts as supported only if its own
     * range is non-degenerate, so a device with a fixed axis stays gated off. */
    int32_t pan_min = 0, pan_max = 0, pan_res = 0;
    int32_t tilt_min = 0, tilt_max = 0, tilt_res = 0;
    uvc_error_t pt_min = uvc_get_pantilt_abs(self->uvc_devh, &pan_min, &tilt_min, UVC_GET_MIN);
    uvc_error_t pt_max = uvc_get_pantilt_abs(self->uvc_devh, &pan_max, &tilt_max, UVC_GET_MAX);
    uvc_error_t pt_res = uvc_get_pantilt_abs(self->uvc_devh, &pan_res, &tilt_res, UVC_GET_RES);

    if (pt_min == UVC_SUCCESS && pt_max == UVC_SUCCESS && pt_res == UVC_SUCCESS) {
        if (pan_min < pan_max) {
            self->pan_supported = TRUE;
            self->pan_min = pan_min;
            self->pan_max = pan_max;
        }
        if (tilt_min < tilt_max) {
            self->tilt_supported = TRUE;
            self->tilt_min = tilt_min;
            self->tilt_max = tilt_max;
        }
        int32_t pan_cur = 0, tilt_cur = 0;
        if (uvc_get_pantilt_abs(self->uvc_devh, &pan_cur, &tilt_cur, UVC_GET_CUR) == UVC_SUCCESS) {
            self->pan_cur = pan_cur;
            self->tilt_cur = tilt_cur;
        }
    }

    uint16_t zoom_min = 0, zoom_max = 0, zoom_res = 0;
    uvc_error_t z_min = uvc_get_zoom_abs(self->uvc_devh, &zoom_min, UVC_GET_MIN);
    uvc_error_t z_max = uvc_get_zoom_abs(self->uvc_devh, &zoom_max, UVC_GET_MAX);
    uvc_error_t z_res = uvc_get_zoom_abs(self->uvc_devh, &zoom_res, UVC_GET_RES);

    if (z_min == UVC_SUCCESS && z_max == UVC_SUCCESS && z_res == UVC_SUCCESS &&
        zoom_min < zoom_max) {
        self->zoom_supported = TRUE;
        self->zoom_min = zoom_min;
        self->zoom_max = zoom_max;
        uint16_t zoom_cur = 0;
        if (uvc_get_zoom_abs(self->uvc_devh, &zoom_cur, UVC_GET_CUR) == UVC_SUCCESS) {
            self->zoom_cur = zoom_cur;
        }
    }

    self->ptz_supported =
        self->pan_supported || self->tilt_supported || self->zoom_supported;

    g_mutex_unlock(&self->control_mutex);

    /* Only stored (checked) fields are logged; unsupported axes read back 0. */
    GST_INFO_OBJECT(self, "PTZ probe: pan=%s[%d,%d] tilt=%s[%d,%d] zoom=%s[%d,%d]",
                    self->pan_supported ? "on" : "off", self->pan_min, self->pan_max,
                    self->tilt_supported ? "on" : "off", self->tilt_min, self->tilt_max,
                    self->zoom_supported ? "on" : "off", self->zoom_min, self->zoom_max);
}

gboolean gst_libuvc_h264_src_ptz_set_pan(GstLibuvcH264Src *self, gint value) {
    if (!self->uvc_devh || !self->pan_supported) return FALSE;

    g_mutex_lock(&self->control_mutex);
    gint pan = ptz_clamp(value, self->pan_min, self->pan_max);
    /* Re-send the current tilt: pan and tilt are one control transfer. */
    uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, pan, self->tilt_cur);
    if (res == UVC_SUCCESS) self->pan_cur = pan;
    g_mutex_unlock(&self->control_mutex);

    if (res != UVC_SUCCESS) {
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "setting pan");
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "Pan set to %d", pan);
    return TRUE;
}

gboolean gst_libuvc_h264_src_ptz_set_tilt(GstLibuvcH264Src *self, gint value) {
    if (!self->uvc_devh || !self->tilt_supported) return FALSE;

    g_mutex_lock(&self->control_mutex);
    gint tilt = ptz_clamp(value, self->tilt_min, self->tilt_max);
    uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, self->pan_cur, tilt);
    if (res == UVC_SUCCESS) self->tilt_cur = tilt;
    g_mutex_unlock(&self->control_mutex);

    if (res != UVC_SUCCESS) {
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "setting tilt");
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "Tilt set to %d", tilt);
    return TRUE;
}

gboolean gst_libuvc_h264_src_ptz_set_zoom(GstLibuvcH264Src *self, gint value) {
    if (!self->uvc_devh || !self->zoom_supported) return FALSE;

    g_mutex_lock(&self->control_mutex);
    gint zoom = ptz_clamp(value, self->zoom_min, self->zoom_max);
    uvc_error_t res = uvc_set_zoom_abs(self->uvc_devh, (uint16_t)zoom);
    if (res == UVC_SUCCESS) self->zoom_cur = zoom;
    g_mutex_unlock(&self->control_mutex);

    if (res != UVC_SUCCESS) {
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "setting zoom");
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "Zoom set to %d", zoom);
    return TRUE;
}
