#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "gstlibuvch264src_internal.h"
#include "ptz_control.h"
#include "gstlibuvch264src_error.h"

static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command);

// Control socket thread function
gpointer gst_libuvc_h264_src_control_thread(gpointer data) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)data;
    struct sockaddr_un addr;
    int client_fd;
    char buffer[256];
    fd_set read_fds;
    struct timeval timeout;
    
    self->control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (self->control_socket < 0) {
        GST_ERROR_OBJECT(self, "Failed to create control socket");
        return NULL;
    }
    
    int flags = fcntl(self->control_socket, F_GETFL, 0);
    fcntl(self->control_socket, F_SETFL, flags | O_NONBLOCK);
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/libuvc_control");
    
    unlink(addr.sun_path);
    
    if (bind(self->control_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        GST_ERROR_OBJECT(self, "Failed to bind control socket");
        close(self->control_socket);
        self->control_socket = -1;
        return NULL;
    }
    
    if (listen(self->control_socket, 5) < 0) {
        GST_ERROR_OBJECT(self, "Failed to listen on control socket");
        close(self->control_socket);
        self->control_socket = -1;
        return NULL;
    }
    
    GST_INFO_OBJECT(self, "Control socket listening on /tmp/libuvc_control");
    
    while (self->control_running) {
        FD_ZERO(&read_fds);
        FD_SET(self->control_socket, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int result = select(self->control_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (result > 0 && FD_ISSET(self->control_socket, &read_fds)) {
            client_fd = accept(self->control_socket, NULL, NULL);
            if (client_fd > 0) {
                ssize_t len = read(client_fd, buffer, sizeof(buffer)-1);
                if (len > 0) {
                    buffer[len] = 0;
                    GST_INFO_OBJECT(self, "Received control command: %s", buffer);
                    char *response = gst_libuvc_h264_src_process_control_command(self, buffer);
                    if (response) {
                        if (write(client_fd, response, strlen(response)) < 0) {
                            GST_WARNING_OBJECT(self, "Failed to write response to control socket");
                        }
                        g_free(response);
                    } else {
                        const char *default_response = "OK";
                        if (write(client_fd, default_response, strlen(default_response)) < 0) {
                            GST_WARNING_OBJECT(self, "Failed to write default response to control socket");
                        }
                    }
                }
                close(client_fd);
            }
        } else if (result == 0) {
            continue;
        } else {
            if (self->control_running) {
                GST_WARNING_OBJECT(self, "Select error in control thread");
            }
            break;
        }
    }
    
    GST_DEBUG_OBJECT(self, "Control thread exiting");
    return NULL;
}

static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command) {
    int pan, tilt, zoom;
    uint16_t zoom_abs;
    
    g_mutex_lock(&self->control_mutex);
    
    if (sscanf(command, "PAN_TILT %d %d", &pan, &tilt) == 2) {
        if (self->uvc_devh) {
            uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, pan, tilt);
            if (res == UVC_SUCCESS) {
                GST_INFO_OBJECT(self, "Set pan/tilt to: %d/%d", pan, tilt);
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("OK pan=%d tilt=%d", pan, tilt);
            } else {
                GST_WARNING_OBJECT(self, "Failed to set pan/tilt: %s", uvc_strerror(res));
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("ERROR: %s", uvc_strerror(res));
            }
        }
    } 
    else if (sscanf(command, "ZOOM %d", &zoom) == 1) {
        if (self->uvc_devh) {
            zoom_abs = (uint16_t)zoom;
            uvc_error_t res = uvc_set_zoom_abs(self->uvc_devh, zoom_abs);
            if (res == UVC_SUCCESS) {
                GST_INFO_OBJECT(self, "Set zoom to: %d", zoom_abs);
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("OK zoom=%d", zoom_abs);
            } else {
                GST_WARNING_OBJECT(self, "Failed to set zoom: %s", uvc_strerror(res));
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("ERROR: %s", uvc_strerror(res));
            }
        }
    }
    else if (strcmp(command, "GET_POSITION") == 0) {
        if (self->uvc_devh) {
            int32_t current_pan, current_tilt;
            uint16_t current_zoom;
            char *response = NULL;
            
            uvc_error_t res_pan = uvc_get_pantilt_abs(self->uvc_devh, &current_pan, &current_tilt, UVC_GET_CUR);
            uvc_error_t res_zoom = uvc_get_zoom_abs(self->uvc_devh, &current_zoom, UVC_GET_CUR);
            
            if (res_pan == UVC_SUCCESS && res_zoom == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=%d tilt=%d zoom=%d", current_pan, current_tilt, current_zoom);
            } else if (res_pan == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=%d tilt=%d zoom=unknown", current_pan, current_tilt);
            } else if (res_zoom == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=unknown tilt=unknown zoom=%d", current_zoom);
            } else {
                response = g_strdup("ERROR: Cannot read position");
            }
            
            GST_INFO_OBJECT(self, "Current position: pan=%d, tilt=%d, zoom=%d", 
                           current_pan, current_tilt, current_zoom);
            g_mutex_unlock(&self->control_mutex);
            return response;
        }
    }
    else if (strcmp(command, "GET_CAPABILITIES") == 0) {
        if (self->uvc_devh) {
            GString *caps = g_string_new("CAPABILITIES:");
            
            int32_t pan_min, pan_max, pan_step;
            int32_t tilt_min, tilt_max, tilt_step;
            uvc_error_t res_pt = uvc_get_pantilt_abs(self->uvc_devh, &pan_min, &tilt_min, UVC_GET_MIN);
            if (res_pt == UVC_SUCCESS) {
                uvc_get_pantilt_abs(self->uvc_devh, &pan_max, &tilt_max, UVC_GET_MAX);
                uvc_get_pantilt_abs(self->uvc_devh, &pan_step, &tilt_step, UVC_GET_RES);
                g_string_append_printf(caps, " pan=[%d,%d,step=%d] tilt=[%d,%d,step=%d]", 
                                      pan_min, pan_max, pan_step, tilt_min, tilt_max, tilt_step);
            }
            
            uint16_t zoom_min, zoom_max, zoom_step;
            uvc_error_t res_zoom = uvc_get_zoom_abs(self->uvc_devh, &zoom_min, UVC_GET_MIN);
            if (res_zoom == UVC_SUCCESS) {
                uvc_get_zoom_abs(self->uvc_devh, &zoom_max, UVC_GET_MAX);
                uvc_get_zoom_abs(self->uvc_devh, &zoom_step, UVC_GET_RES);
                g_string_append_printf(caps, " zoom=[%d,%d,step=%d]", zoom_min, zoom_max, zoom_step);
            }
            
            GST_INFO_OBJECT(self, "Capabilities: %s", caps->str);
            g_mutex_unlock(&self->control_mutex);
            return g_string_free(caps, FALSE);
        }
    }
    
    g_mutex_unlock(&self->control_mutex);
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
