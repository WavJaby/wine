/*
 * Wayland pointer handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <linux/input.h>
#undef SW_MAX /* Also defined in winuser.rh */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#define OEMRESOURCE

#include "waylanddrv.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);

/* The cursor-shape-v1 protocol file references the zwp_tablet_tool_v2
 * interface object. Since we don't currently use the tablet protocol,
 * provide a dummy object here to avoid linking errors. */
void *zwp_tablet_tool_v2_interface = NULL;

struct system_cursors
{
    WORD id;
    enum wp_cursor_shape_device_v1_shape shape;
};

static const struct system_cursors user32_cursors[] =
{
    {OCR_NORMAL,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {OCR_IBEAM,       WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
    {OCR_WAIT,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
    {OCR_CROSS,       WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
    {OCR_SIZE,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
    {OCR_SIZENWSE,    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
    {OCR_SIZENESW,    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
    {OCR_SIZEWE,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
    {OCR_SIZENS,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
    {OCR_SIZEALL,     WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
    {OCR_NO,          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
    {OCR_HAND,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {OCR_APPSTARTING, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
    {OCR_HELP,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
    {OCR_RDR2DIM,     WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},
    {0}
};

static const struct system_cursors comctl32_cursors[] =
{
    {102, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {104, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {105, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {106, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {107, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {108, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {135, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
    {0}
};

static const struct system_cursors ole32_cursors[] =
{
    {1, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {2, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {3, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {4, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
    {0}
};

static const struct system_cursors riched20_cursors[] =
{
    {105, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {109, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {110, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {111, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {0}
};

static const struct
{
    const struct system_cursors *cursors;
    WCHAR name[16];
} module_cursors[] =
{
    {user32_cursors, {'u','s','e','r','3','2','.','d','l','l',0}},
    {comctl32_cursors, {'c','o','m','c','t','l','3','2','.','d','l','l',0}},
    {ole32_cursors, {'o','l','e','3','2','.','d','l','l',0}},
    {riched20_cursors, {'r','i','c','h','e','d','2','0','.','d','l','l',0}}
};

static HWND wayland_pointer_get_focused_hwnd(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    pthread_mutex_lock(&pointer->mutex);
    hwnd = pointer->focused_hwnd;
    pthread_mutex_unlock(&pointer->mutex);

    return hwnd;
}

/* Windows' SC_MOVE loop moves nothing until the pointer does, so a plain click on a draggable area is harmless there and
 * apps (LINE) route every press on those areas through it. xdg_toplevel_move grabs the pointer at once and eats the click,
 * so hold it back until the press turns into a drag. The press stays implicitly grabbed by this surface meanwhile. */
void wayland_pointer_defer_move(HWND hwnd, uint32_t serial)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    SIZE threshold = { NtUserGetSystemMetrics(SM_CXDRAG), NtUserGetSystemMetrics(SM_CYDRAG) };

    pthread_mutex_lock(&pointer->mutex);
    /* The button may have been released since the caller read the serial. */
    if (pointer->button_serial == serial)
    {
        pointer->deferred_move.hwnd = hwnd;
        pointer->deferred_move.serial = serial;
        pointer->deferred_move.origin = pointer->last_pos;
        pointer->deferred_move.threshold = threshold;
    }
    pthread_mutex_unlock(&pointer->mutex);
}

static void wayland_pointer_track_move(HWND hwnd, POINT pos)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    struct wl_seat *wl_seat;
    uint32_t serial = 0;

    pthread_mutex_lock(&pointer->mutex);
    pointer->last_pos = pos;
    if (pointer->deferred_move.hwnd == hwnd &&
        (abs(pos.x - pointer->deferred_move.origin.x) >= pointer->deferred_move.threshold.cx ||
         abs(pos.y - pointer->deferred_move.origin.y) >= pointer->deferred_move.threshold.cy))
    {
        serial = pointer->deferred_move.serial;
        pointer->deferred_move.hwnd = NULL;
    }
    pthread_mutex_unlock(&pointer->mutex);

    if (!serial || !(data = wayland_win_data_get(hwnd))) return;

    TRACE("hwnd=%p starting deferred move\n", hwnd);

    pthread_mutex_lock(&process_wayland.seat.mutex);
    wl_seat = process_wayland.seat.wl_seat;
    if (wl_seat && (surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
        xdg_toplevel_move(surface->xdg_toplevel, wl_seat, serial);
    pthread_mutex_unlock(&process_wayland.seat.mutex);
    wayland_win_data_release(data);

    wl_display_flush(process_wayland.wl_display);
}

static void pointer_handle_motion_internal(wl_fixed_t sx, wl_fixed_t sy)
{
    INPUT input = {0};
    RECT *window_rect;
    HWND hwnd;
    POINT screen = { wl_fixed_to_double(sx), wl_fixed_to_double(sy) };
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    InterlockedExchangePointer((void **)&process_wayland.touch.last_hwnd, NULL);
    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;
    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    window_rect = &surface->window.rect;

    screen = map_point_from_surface(surface, screen);
    screen.x += window_rect->left;
    screen.y += window_rect->top;
    /* Sometimes, due to rounding, we may end up with pointer coordinates
     * slightly outside the target window, so bring them within bounds. */
    if (screen.x >= window_rect->right) screen.x = window_rect->right - 1;
    else if (screen.x < window_rect->left) screen.x = window_rect->left;
    if (screen.y >= window_rect->bottom) screen.y = window_rect->bottom - 1;
    else if (screen.y < window_rect->top) screen.y = window_rect->top;

    wayland_win_data_release(data);

    wayland_pointer_track_move(hwnd, screen);

    input.type = INPUT_MOUSE;
    input.mi.dx = screen.x;
    input.mi.dy = screen.y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    TRACE("hwnd=%p wayland_xy=%.2f,%.2f screen_xy=%d,%d\n",
          hwnd, wl_fixed_to_double(sx), wl_fixed_to_double(sy),
          screen.x, screen.y);

    NtUserSendHardwareInput(hwnd, SEND_HWMSG_RAWINPUT, &input, 0);
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    /* Ignore absolute motion events if in relative mode. */
    if (pointer->relative_mode) return;

    pointer_handle_motion_internal(sx, sy);
}

static void wayland_set_cursor(HWND hwnd, HCURSOR hcursor, BOOL use_hcursor);

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *wl_surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;
    /* The wl_surface user data remains valid and immutable for the whole
     * lifetime of the object, so it's safe to access without locking. */
    hwnd = wl_surface_get_user_data(wl_surface);

    TRACE("hwnd=%p\n", hwnd);

    pthread_mutex_lock(&pointer->mutex);
    pointer->focused_hwnd = hwnd;
    pointer->enter_serial = serial;
    memset(pointer->scroll, 0, sizeof(pointer->scroll));
    pthread_mutex_unlock(&pointer->mutex);

    /* The cursor is undefined at every enter, so we set it again with
     * the latest information we have. */
    wayland_set_cursor(hwnd, NULL, FALSE);

    /* Handle the enter as a motion, to account for cases where the
     * window first appears beneath the pointer and won't get a separate
     * motion event. */
    pointer_handle_motion_internal(sx, sy);
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *wl_surface)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;

    TRACE("hwnd=%p\n", wl_surface_get_user_data(wl_surface));

    pthread_mutex_lock(&pointer->mutex);
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    pointer->deferred_move.hwnd = NULL;
    memset(pointer->scroll, 0, sizeof(pointer->scroll));
    pointer->inertia_active = FALSE;
    pthread_mutex_unlock(&pointer->mutex);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    INPUT input = {0};
    HWND hwnd;

    InterlockedExchange(&process_wayland.input_serial, serial);
    InterlockedExchangePointer((void **)&process_wayland.touch.last_hwnd, NULL);

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    input.type = INPUT_MOUSE;

    switch (button)
    {
    case BTN_LEFT: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case BTN_RIGHT: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case BTN_MIDDLE: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    case BTN_SIDE:
    case BTN_BACK:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = XBUTTON1;
        break;
    case BTN_EXTRA:
    case BTN_FORWARD:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = XBUTTON2;
        break;
    default: break;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) input.mi.dwFlags <<= 1;

    pthread_mutex_lock(&pointer->mutex);
    pointer->button_serial = state == WL_POINTER_BUTTON_STATE_PRESSED ?
                             serial : 0;
    pointer->deferred_move.hwnd = NULL;
    pointer->inertia_active = FALSE;
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd=%p button=%#x state=%u\n", hwnd, button, state);

    NtUserSendHardwareInput(hwnd, SEND_HWMSG_RAWINPUT, &input, 0);
}

/* Kinetic scrolling for finger (touchpad) axes. Windows synthesizes decaying wheel input after a precision
 * touchpad lift for applications that only read WM_MOUSEWHEEL (Qt, LINE); Wayland leaves that to the client.
 * Velocity decays as v(t) = v0 * exp(-t / tau), the iOS-like curve used by most kinetic-scrolling toolkits. */
static const double inertia_time_constant_ms = 325.0;
/* Lift-off speed below one wheel notch (10 surface units) per 100 ms reads as a deliberate stop, not a flick. */
static const double inertia_start_velocity = 10.0 / 100.0;
/* End once it has decayed below one notch per second; further steps are invisible single-pixel creep. */
static const double inertia_stop_velocity = 10.0 / 1000.0;
/* Only samples this close to axis_stop count: a finger that paused before lifting should not fling. */
static const uint32_t inertia_sample_window_ms = 80;
/* ~60 Hz steps, one per typical display frame. */
static const int inertia_step_ms = 16;

static UINT64 monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (UINT64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Continuous axis units have no universal wheel conversion. Use the conventional ten surface units per wheel
 * notch, as Qt Wayland does. */
static double surface_units_to_wheel(double value)
{
    return value * WHEEL_DELTA / 10.0;
}

static void send_scroll(HWND hwnd, const int deltas[2])
{
    unsigned int axis;

    for (axis = 0; axis < 2; ++axis)
    {
        INPUT input = {.type = INPUT_MOUSE};
        if (!deltas[axis]) continue;
        input.mi.dwFlags = axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? -deltas[axis] : deltas[axis];
        TRACE("hwnd=%p axis=%u value120=%d\n", hwnd, axis, deltas[axis]);
        NtUserSendHardwareInput(hwnd, SEND_HWMSG_RAWINPUT, &input, 0);
    }
}

/* Mean velocity over the samples inside the window before axis_stop; 0 if the finger rested before lifting. */
static double lift_velocity(const typeof(process_wayland.pointer.scroll[0]) *scroll)
{
    unsigned int i, first = 0, count = 0;
    double sum = 0.0;
    uint32_t span;

    for (i = 0; i < scroll->sample_count; ++i)
    {
        if (scroll->stop_time - scroll->samples[i].time > inertia_sample_window_ms) continue;
        if (!count++) first = i;
        else sum += scroll->samples[i].value; /* the first sample's value accrued before its timestamp */
    }
    if (count < 2) return 0.0;
    if (!(span = scroll->samples[scroll->sample_count - 1].time - scroll->samples[first].time)) return 0.0;
    return sum / span;
}

static void pointer_flush_scroll(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    BOOL fling = FALSE;
    int deltas[2];
    HWND hwnd;
    unsigned int axis;

    pthread_mutex_lock(&pointer->mutex);
    hwnd = pointer->focused_hwnd;
    for (axis = 0; axis < ARRAY_SIZE(pointer->scroll); ++axis)
    {
        /* Discrete/value120 events take precedence over their paired axis. */
        double value = pointer->scroll[axis].has_value120 ? pointer->scroll[axis].value120 :
                       surface_units_to_wheel(pointer->scroll[axis].value);
        value += pointer->scroll[axis].remainder;
        deltas[axis] = value;
        pointer->scroll[axis].remainder = pointer->scroll[axis].stopped ? 0.0 : value - deltas[axis];
        pointer->scroll[axis].velocity = 0.0;
        if (pointer->scroll[axis].stopped && pointer->scroll_source == WL_POINTER_AXIS_SOURCE_FINGER)
        {
            double velocity = lift_velocity(&pointer->scroll[axis]);
            if (fabs(velocity) >= inertia_start_velocity)
            {
                pointer->scroll[axis].velocity = velocity;
                fling = TRUE;
            }
            pointer->scroll[axis].sample_count = 0;
        }
        pointer->scroll[axis].value = 0.0;
        pointer->scroll[axis].value120 = 0;
        pointer->scroll[axis].has_value120 = FALSE;
        pointer->scroll[axis].stopped = FALSE;
    }
    if (fling && hwnd)
    {
        TRACE("hwnd=%p fling velocity %.3f,%.3f units/ms\n", hwnd,
              pointer->scroll[0].velocity, pointer->scroll[1].velocity);
        pointer->inertia_active = TRUE;
        pointer->inertia_hwnd = hwnd;
        pointer->inertia_tick = monotonic_ms();
    }
    pthread_mutex_unlock(&pointer->mutex);

    if (hwnd) send_scroll(hwnd, deltas);
}

/* Poll timeout for the event thread: the next inertia step, or -1 (block) when idle. */
int wayland_pointer_inertia_timeout(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    UINT64 now, due;
    int timeout = -1;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->inertia_active)
    {
        now = monotonic_ms();
        due = pointer->inertia_tick + inertia_step_ms;
        timeout = due > now ? due - now : 0;
    }
    pthread_mutex_unlock(&pointer->mutex);
    return timeout;
}

void wayland_pointer_inertia_step(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    int deltas[2] = {0};
    BOOL moving = FALSE;
    unsigned int axis;
    UINT64 now;
    double dt, decay;
    HWND hwnd;

    pthread_mutex_lock(&pointer->mutex);
    if (!pointer->inertia_active || (now = monotonic_ms()) < pointer->inertia_tick + inertia_step_ms)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return;
    }
    dt = now - pointer->inertia_tick;
    pointer->inertia_tick = now;
    decay = exp(-dt / inertia_time_constant_ms);
    for (axis = 0; axis < ARRAY_SIZE(pointer->scroll); ++axis)
    {
        double velocity = pointer->scroll[axis].velocity, value;
        if (fabs(velocity) < inertia_stop_velocity) continue;
        /* Distance covered over dt is the integral of v0 * exp(-t / tau). */
        value = surface_units_to_wheel(velocity * inertia_time_constant_ms * (1.0 - decay));
        value += pointer->scroll[axis].remainder;
        deltas[axis] = value;
        pointer->scroll[axis].remainder = value - deltas[axis];
        pointer->scroll[axis].velocity = velocity * decay;
        moving = TRUE;
    }
    if (!moving)
    {
        pointer->inertia_active = FALSE;
        for (axis = 0; axis < ARRAY_SIZE(pointer->scroll); ++axis) pointer->scroll[axis].remainder = 0.0;
    }
    hwnd = pointer->inertia_hwnd;
    pthread_mutex_unlock(&pointer->mutex);

    send_scroll(hwnd, deltas);
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    if (axis >= ARRAY_SIZE(pointer->scroll)) return;
    pthread_mutex_lock(&pointer->mutex);
    pointer->scroll[axis].value += wl_fixed_to_double(value);
    if (pointer->inertia_active)
    {
        /* A finger touching down again (or a wheel) takes over from the fling. */
        pointer->inertia_active = FALSE;
        pointer->scroll[0].velocity = pointer->scroll[1].velocity = 0.0;
    }
    if (pointer->scroll_source == WL_POINTER_AXIS_SOURCE_FINGER)
    {
        typeof(pointer->scroll[0]) *scroll = &pointer->scroll[axis];
        if (scroll->sample_count == ARRAY_SIZE(scroll->samples))
            memmove(scroll->samples, scroll->samples + 1, sizeof(scroll->samples) - sizeof(scroll->samples[0]));
        else scroll->sample_count++;
        scroll->samples[scroll->sample_count - 1].time = time;
        scroll->samples[scroll->sample_count - 1].value = wl_fixed_to_double(value);
    }
    pthread_mutex_unlock(&pointer->mutex);
    if (wl_pointer_get_version(wl_pointer) < WL_POINTER_FRAME_SINCE_VERSION) pointer_flush_scroll();
}

static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
    static const INPUT input = { .type = INPUT_MOUSE, .mi.dwFlags = MOUSEEVENTF_MOVE_NOCOALESCE };
    NtUserSendHardwareInput(NULL, SEND_HWMSG_RAWINPUT, &input, 0); /* flush win32u accumulated motion */
    pointer_flush_scroll();
}

static void pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
                                       uint32_t axis_source)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    unsigned int axis;
    pthread_mutex_lock(&pointer->mutex);
    if (pointer->scroll_source != axis_source)
        for (axis = 0; axis < ARRAY_SIZE(pointer->scroll); ++axis) pointer->scroll[axis].remainder = 0.0;
    pointer->scroll_source = axis_source;
    pthread_mutex_unlock(&pointer->mutex);
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t time, uint32_t axis)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    if (axis >= ARRAY_SIZE(pointer->scroll)) return;
    pthread_mutex_lock(&pointer->mutex);
    pointer->scroll[axis].stopped = TRUE;
    pointer->scroll[axis].stop_time = time;
    pthread_mutex_unlock(&pointer->mutex);
}

static void pointer_handle_axis_value120(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t value120)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    if (axis >= ARRAY_SIZE(pointer->scroll)) return;
    pthread_mutex_lock(&pointer->mutex);
    pointer->scroll[axis].value120 += value120;
    pointer->scroll[axis].has_value120 = TRUE;
    pthread_mutex_unlock(&pointer->mutex);
}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t discrete)
{
    pointer_handle_axis_value120(data, wl_pointer, axis, WHEEL_DELTA * discrete);
}

static const struct wl_pointer_listener pointer_listener =
{
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    pointer_handle_axis_value120
#endif
};

/**********************************************************************
 *          wayland_motion_delta_to_window
 *
 * Converts the surface-local delta to window (logical) coordinate delta.
 */
static void wayland_motion_delta_to_window(struct wayland_surface *surface,
                                           double surface_x, double surface_y,
                                           double *window_x, double *window_y)
{
    *window_x = surface_x * surface->window.scale;
    *window_y = surface_y * surface->window.scale;
}

static void relative_pointer_v1_relative_motion(void *private,
                                                struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1,
                                                uint32_t utime_hi, uint32_t utime_lo,
                                                wl_fixed_t dx, wl_fixed_t dy,
                                                wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    const POINT raw_pos = { .x = wl_fixed_to_double(dx_unaccel), .y = wl_fixed_to_double(dy_unaccel) };
    struct raw_mouse raw = { .count = 1, .data = { raw_pos } };
    INPUT input = { .type = INPUT_MOUSE };
    HWND hwnd;
    struct wayland_win_data *data;
    double screen_x = 0.0, screen_y = 0.0;
    struct wayland_pointer *pointer = &process_wayland.pointer;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    wayland_motion_delta_to_window(data->wayland_surface,
                                   wl_fixed_to_double(dx),
                                   wl_fixed_to_double(dy),
                                   &screen_x, &screen_y);
    wayland_win_data_release(data);

    pthread_mutex_lock(&pointer->mutex);

    if (pointer->relative_mode)
    {
        pointer->accum_x += screen_x;
        pointer->accum_y += screen_y;

        input.mi.dx = round(pointer->accum_x);
        input.mi.dy = round(pointer->accum_y);
        input.mi.dwFlags = MOUSEEVENTF_MOVE;

        pointer->accum_x -= input.mi.dx;
        pointer->accum_y -= input.mi.dy;
    }

    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd=%p wayland_dxdy=%.2f,%.2f accum_dxdy=%d,%d wayland_raw=%.2f,%.2f raw_dxdy=%d,%d\n",
          hwnd, wl_fixed_to_double(dx), wl_fixed_to_double(dy), input.mi.dx, input.mi.dy,
          wl_fixed_to_double(dx_unaccel), wl_fixed_to_double(dy_unaccel), raw_pos.x, raw_pos.y);

    NtUserSendHardwareInput(hwnd, SEND_HWMSG_RAWINPUT, &input, (LPARAM)&raw);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_v1_listener =
{
    relative_pointer_v1_relative_motion
};

void wayland_pointer_init(struct wl_pointer *wl_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    pointer->wl_pointer = wl_pointer;
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    memset(pointer->scroll, 0, sizeof(pointer->scroll));
    pointer->scroll_source = WL_POINTER_AXIS_SOURCE_WHEEL;
    pthread_mutex_unlock(&pointer->mutex);
    wl_pointer_add_listener(pointer->wl_pointer, &pointer_listener, NULL);

    if (!process_wayland.zwp_relative_pointer_manager_v1)
        ERR("zwp_relative_pointer_manager_v1 isn't supported, skipping relative motion\n");
    else
    {
        pointer->zwp_relative_pointer_v1 =
            zwp_relative_pointer_manager_v1_get_relative_pointer(
                process_wayland.zwp_relative_pointer_manager_v1,
                pointer->wl_pointer);
        zwp_relative_pointer_v1_add_listener(pointer->zwp_relative_pointer_v1,
                                             &relative_pointer_v1_listener, NULL);
    }
}

void wayland_pointer_deinit(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_confined_pointer_v1)
    {
        zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
        pointer->zwp_confined_pointer_v1 = NULL;
    }
    if (pointer->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
        pointer->zwp_locked_pointer_v1 = NULL;
    }
    if (pointer->zwp_relative_pointer_v1)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
    }
    if (pointer->wp_cursor_shape_device_v1)
    {
        wp_cursor_shape_device_v1_destroy(pointer->wp_cursor_shape_device_v1);
        pointer->wp_cursor_shape_device_v1 = NULL;
    }
    wl_pointer_release(pointer->wl_pointer);
    pointer->wl_pointer = NULL;
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    memset(pointer->scroll, 0, sizeof(pointer->scroll));
    pointer->inertia_active = FALSE;
    pthread_mutex_unlock(&pointer->mutex);
}

/***********************************************************************
 *           create_mono_cursor_buffer
 *
 * Create a wayland_shm_buffer for a monochrome cursor bitmap.
 *
 * Adapted from wineandroid.drv code.
 */
static struct wayland_shm_buffer *create_mono_cursor_buffer(HBITMAP bmp)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    BITMAP bm;
    char *mask = NULL;
    unsigned int i, j, stride, mask_size, *ptr;

    if (!NtGdiExtGetObjectW(bmp, sizeof(bm), &bm)) goto done;
    stride = ((bm.bmWidth + 15) >> 3) & ~1;
    mask_size = stride * bm.bmHeight;
    if (!(mask = malloc(mask_size))) goto done;
    if (!NtGdiGetBitmapBits(bmp, mask_size, mask)) goto done;

    bm.bmHeight /= 2;
    shm_buffer = wayland_shm_buffer_create(bm.bmWidth, bm.bmHeight,
                                           WL_SHM_FORMAT_ARGB8888);
    if (!shm_buffer) goto done;

    ptr = shm_buffer->map_data;
    for (i = 0; i < bm.bmHeight; i++)
    {
        for (j = 0; j < bm.bmWidth; j++, ptr++)
        {
            int and = ((mask[i * stride + j / 8] << (j % 8)) & 0x80);
            int xor = ((mask[(i + bm.bmHeight) * stride + j / 8] << (j % 8)) & 0x80);
            if (!xor && and)
                *ptr = 0;
            else if (xor && !and)
                *ptr = 0xffffffff;
            else
                /* we can't draw "invert" pixels, so render them as black instead */
                *ptr = 0xff000000;
        }
    }

done:
    free(mask);
    return shm_buffer;
}

/***********************************************************************
 *           get_icon_info
 *
 * Local GetIconInfoExW helper implementation.
 */
static BOOL get_icon_info(HICON handle, ICONINFOEXW *ret)
{
    UNICODE_STRING module, res_name;
    ICONINFO info;

    module.Buffer = ret->szModName;
    module.MaximumLength = sizeof(ret->szModName) - sizeof(WCHAR);
    res_name.Buffer = ret->szResName;
    res_name.MaximumLength = sizeof(ret->szResName) - sizeof(WCHAR);
    if (!NtUserGetIconInfo(handle, &info, &module, &res_name, NULL, 0)) return FALSE;
    ret->fIcon = info.fIcon;
    ret->xHotspot = info.xHotspot;
    ret->yHotspot = info.yHotspot;
    ret->hbmColor = info.hbmColor;
    ret->hbmMask = info.hbmMask;
    ret->wResID = res_name.Length ? 0 : LOWORD(res_name.Buffer);
    ret->szModName[module.Length] = 0;
    ret->szResName[res_name.Length] = 0;
    return TRUE;
}

static BOOL cursor_buffer_is_transparent(struct wayland_shm_buffer *shm_buffer)
{
    uint32_t *pixel = shm_buffer->map_data;
    uint32_t *end = pixel + shm_buffer->map_size / WINEWAYLAND_BYTES_PER_PIXEL;

    for (; pixel < end; ++pixel)
        if ((*pixel & 0xff000000) != 0) return FALSE;

    return TRUE;
}

static void wayland_pointer_update_cursor_buffer(HCURSOR hcursor, double scale)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;
    ICONINFOEXW info = {0};

    if (!hcursor) goto clear_cursor;

    /* Create a new buffer for the specified cursor. */
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }

    if (!get_icon_info(hcursor, &info))
    {
        ERR("Failed to get icon info for cursor=%p\n", hcursor);
        goto clear_cursor;
    }

    if (info.hbmColor)
    {
        HDC hdc = NtGdiCreateCompatibleDC(0);
        cursor->shm_buffer =
            wayland_shm_buffer_from_color_bitmaps(hdc, info.hbmColor, info.hbmMask, FALSE);
        NtGdiDeleteObjectApp(hdc);
    }
    else
    {
        cursor->shm_buffer = create_mono_cursor_buffer(info.hbmMask);
    }

    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);

    cursor->hotspot_x = info.xHotspot;
    cursor->hotspot_y = info.yHotspot;

    if (!cursor->shm_buffer)
    {
        ERR("Failed to create shm_buffer for cursor=%p\n", hcursor);
        goto clear_cursor;
    }

    if (cursor_buffer_is_transparent(cursor->shm_buffer))
        goto clear_cursor;

    /* Make sure the hotspot is valid. */
    if (cursor->hotspot_x >= cursor->shm_buffer->width ||
        cursor->hotspot_y >= cursor->shm_buffer->height)
    {
        cursor->hotspot_x = cursor->shm_buffer->width / 2;
        cursor->hotspot_y = cursor->shm_buffer->height / 2;
    }

    cursor->hotspot_x = round(cursor->hotspot_x / scale);
    cursor->hotspot_y = round(cursor->hotspot_y / scale);

    return;

clear_cursor:
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }
}

static void wayland_pointer_clear_cursor_surface(void)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (cursor->wp_viewport)
    {
        wp_viewport_destroy(cursor->wp_viewport);
        cursor->wp_viewport = NULL;
    }
    if (cursor->wl_surface)
    {
        wl_surface_destroy(cursor->wl_surface);
        cursor->wl_surface = NULL;
    }
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }
}

static void wayland_pointer_update_cursor_surface(double scale)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (!cursor->shm_buffer) goto clear_cursor;

    if (!cursor->wl_surface)
    {
        cursor->wl_surface =
            wl_compositor_create_surface(process_wayland.wl_compositor);
        if (!cursor->wl_surface)
        {
            ERR("Failed to create wl_surface for cursor\n");
            goto clear_cursor;
        }
    }

    if (!cursor->wp_viewport)
    {
        cursor->wp_viewport =
            wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                       cursor->wl_surface);
        if (!cursor->wp_viewport)
        {
            ERR("Failed to create wp_viewport for cursor\n");
            goto clear_cursor;
        }
    }

    /* Commit the cursor buffer to the cursor surface. */
    wl_surface_attach(cursor->wl_surface,
                      cursor->shm_buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(cursor->wl_surface, 0, 0,
                             cursor->shm_buffer->width,
                             cursor->shm_buffer->height);
    /* Setting only the viewport is enough, but some compositors don't
     * support wp_viewport for cursor surfaces, so also set the buffer
     * scale. Note that setting the viewport destination overrides
     * the buffer scale, so it's fine to set both. */
    wl_surface_set_buffer_scale(cursor->wl_surface, round(scale));
    wp_viewport_set_destination(cursor->wp_viewport,
                                round(cursor->shm_buffer->width / scale),
                                round(cursor->shm_buffer->height / scale));
    wl_surface_commit(cursor->wl_surface);

    return;

clear_cursor:
    wayland_pointer_clear_cursor_surface();
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect)) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static enum wp_cursor_shape_device_v1_shape cursor_shape_from_info(ICONINFOEXW *info,
                                                                   uint32_t proto_version)
{
    const struct system_cursors *cursors;
    const WCHAR *module;
    unsigned int i;
    enum wp_cursor_shape_device_v1_shape shape = 0;

    if (!info->szModName[0]) return 0;
    if ((module = wcsrchr(info->szModName, '\\'))) module++;
    else module = info->szModName;
    for (i = 0; i < ARRAY_SIZE(module_cursors); i++)
        if (!wcsicmp(module, module_cursors[i].name)) break;
    if (i == ARRAY_SIZE(module_cursors)) return 0;

    cursors = module_cursors[i].cursors;
    for (i = 0; cursors[i].id; i++)
    {
        if (cursors[i].id == info->wResID)
        {
            shape = cursors[i].shape;
            break;
        }
    }

    if (shape >= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK && proto_version < 2)
        shape = 0;

    return shape;
}

static BOOL wayland_pointer_set_cursor_shape(HCURSOR hcursor)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    ICONINFOEXW info = {0};
    enum wp_cursor_shape_device_v1_shape shape = 0;
    uint32_t proto_version;

    if (!process_wayland.wp_cursor_shape_manager_v1) return FALSE;
    if (!hcursor) return FALSE;
    if (!get_icon_info(hcursor, &info)) return FALSE;
    proto_version = wp_cursor_shape_manager_v1_get_version(
        process_wayland.wp_cursor_shape_manager_v1);
    shape = cursor_shape_from_info(&info, proto_version);

    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);

    if (!shape) return FALSE;

    if (!pointer->wp_cursor_shape_device_v1)
    {
        pointer->wp_cursor_shape_device_v1 =
            wp_cursor_shape_manager_v1_get_pointer(
                process_wayland.wp_cursor_shape_manager_v1, pointer->wl_pointer);
        if (!pointer->wp_cursor_shape_device_v1) return FALSE;
    }

    wp_cursor_shape_device_v1_set_shape(pointer->wp_cursor_shape_device_v1,
                                        pointer->enter_serial, shape);

    return TRUE;
}

static void wayland_pointer_clear_cursor_shape(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    if (pointer->wp_cursor_shape_device_v1)
    {
        wp_cursor_shape_device_v1_destroy(pointer->wp_cursor_shape_device_v1);
        pointer->wp_cursor_shape_device_v1 = NULL;
    }
}

static void wayland_set_cursor(HWND hwnd, HCURSOR hcursor, BOOL use_hcursor)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    double scale;
    BOOL reapply_clip = FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (!(surface = data->wayland_surface))
        {
            wayland_win_data_release(data);
            return;
        }
        scale = surface->window.scale;
        if (use_hcursor) surface->hcursor = hcursor;
        else hcursor = surface->hcursor;
        use_hcursor = TRUE;
        wayland_win_data_release(data);
    }
    else
    {
        scale = 1.0;
    }

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->focused_hwnd == hwnd)
    {
        if ((!use_hcursor && pointer->wp_cursor_shape_device_v1) ||
            (use_hcursor && hcursor && wayland_pointer_set_cursor_shape(hcursor)))
        {
            wayland_pointer_clear_cursor_surface();
        }
        else
        {
            if (use_hcursor) wayland_pointer_update_cursor_buffer(hcursor, scale);
            wayland_pointer_update_cursor_surface(scale);
            wl_pointer_set_cursor(pointer->wl_pointer,
                                  pointer->enter_serial,
                                  pointer->cursor.wl_surface,
                                  pointer->cursor.hotspot_x,
                                  pointer->cursor.hotspot_y);
            wayland_pointer_clear_cursor_shape();
        }
        wl_display_flush(process_wayland.wl_display);
        reapply_clip = TRUE;
    }
    pthread_mutex_unlock(&pointer->mutex);

    /* Reapply cursor clip since cursor visibility affects pointer constraint
     * behavior. */
    if (reapply_clip) reapply_cursor_clipping();
}

/**********************************************************************
 *          wayland_surface_calc_confine
 *
 * Calculates the pointer confine rect (in surface-local coords)
 * for the specified clip rectangle (in screen coords using thread dpi).
 */
static void wayland_surface_calc_confine(struct wayland_surface *surface,
                                         const RECT *clip, RECT *confine)
{
    RECT window_clip;

    TRACE("hwnd=%p clip=%s window=%s\n",
          surface->hwnd, wine_dbgstr_rect(clip),
          wine_dbgstr_rect(&surface->window.rect));

    /* FIXME: surface->window.(client_)rect is in window dpi, whereas
     * clip is in thread dpi. */

    if (!intersect_rect(&window_clip, clip, &surface->window.rect))
    {
        SetRectEmpty(confine);
        return;
    }

    OffsetRect(&window_clip, -surface->window.rect.left, -surface->window.rect.top);
    *confine = map_rect_to_surface(surface, window_clip);
}

/***********************************************************************
 *           wayland_pointer_update_constraint
 *
 *  Enables/disables pointer confinement.
 */
static void wayland_pointer_update_constraint(struct wl_surface *wl_surface,
                                              RECT *confine_rect,
                                              BOOL force_lock)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    BOOL needs_relative, needs_lock, needs_confine, is_visible;
    static unsigned int once;

    if (!process_wayland.zwp_pointer_constraints_v1)
    {
        if (!once++)
            ERR("This function requires zwp_pointer_constraints_v1\n");
        return;
    }

    is_visible = pointer->cursor.wl_surface || pointer->wp_cursor_shape_device_v1;
    needs_lock = wl_surface &&
                 ((confine_rect && !is_visible) || force_lock) &&
                 pointer->wl_pointer;
    needs_confine = wl_surface && confine_rect && is_visible && !force_lock &&
                    pointer->wl_pointer;

    if (!needs_confine && pointer->zwp_confined_pointer_v1)
    {
        TRACE("Unconfining from hwnd=%p\n", pointer->constraint_hwnd);
        zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
        pointer->zwp_confined_pointer_v1 = NULL;
        pointer->constraint_hwnd = NULL;
    }

    if (!needs_lock && pointer->zwp_locked_pointer_v1)
    {
        TRACE("Unlocking from hwnd=%p\n", pointer->constraint_hwnd);
        zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
        pointer->zwp_locked_pointer_v1 = NULL;
        pointer->constraint_hwnd = NULL;
    }

    if (needs_confine)
    {
        HWND hwnd = wl_surface_get_user_data(wl_surface);
        struct wl_region *region;

        region = wl_compositor_create_region(process_wayland.wl_compositor);
        wl_region_add(region, confine_rect->left, confine_rect->top,
                      confine_rect->right - confine_rect->left,
                      confine_rect->bottom - confine_rect->top);

        if (!pointer->zwp_confined_pointer_v1 || pointer->constraint_hwnd != hwnd)
        {
            if (pointer->zwp_confined_pointer_v1)
                zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
            pointer->zwp_confined_pointer_v1 =
                zwp_pointer_constraints_v1_confine_pointer(
                    process_wayland.zwp_pointer_constraints_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    region,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            pointer->constraint_hwnd = hwnd;
        }
        else
        {
            zwp_confined_pointer_v1_set_region(pointer->zwp_confined_pointer_v1,
                                               region);
        }

        TRACE("Confining to hwnd=%p wayland=%d,%d+%d,%d\n",
              pointer->constraint_hwnd,
              confine_rect->left, confine_rect->top,
              confine_rect->right - confine_rect->left,
              confine_rect->bottom - confine_rect->top);

        wl_region_destroy(region);
    }
    else if (needs_lock)
    {
        HWND hwnd = wl_surface_get_user_data(wl_surface);

        if (!pointer->zwp_locked_pointer_v1 || pointer->constraint_hwnd != hwnd)
        {
            if (pointer->zwp_locked_pointer_v1)
                zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
            pointer->zwp_locked_pointer_v1 =
                zwp_pointer_constraints_v1_lock_pointer(
                    process_wayland.zwp_pointer_constraints_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    NULL,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            pointer->constraint_hwnd = hwnd;
            TRACE("Locking to hwnd=%p\n", pointer->constraint_hwnd);
        }
    }

    needs_relative = !is_visible && pointer->constraint_hwnd &&
                     pointer->constraint_hwnd == pointer->focused_hwnd;

    if (needs_relative && !pointer->relative_mode && pointer->zwp_relative_pointer_v1)
    {
        pointer->relative_mode = TRUE;
        pointer->accum_x = pointer->accum_y = 0;
        TRACE("Enabling relative motion\n");
    }
    else if (!needs_relative && pointer->relative_mode)
    {
        pointer->relative_mode = FALSE;
        TRACE("Disabling relative motion\n");
    }
}

void wayland_pointer_clear_constraint(void)
{
    wayland_pointer_update_constraint(NULL, NULL, FALSE);
}

/***********************************************************************
 *           WAYLAND_SetCursor
 */
void WAYLAND_SetCursor(HWND hwnd, HCURSOR hcursor)
{
    TRACE("hwnd=%p hcursor=%p\n", hwnd, hcursor);

    wayland_set_cursor(hwnd, hcursor, TRUE);
}

/***********************************************************************
 *           WAYLAND_SetCursorPos
 */
BOOL WAYLAND_SetCursorPos(INT x, INT y)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->relative_mode)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return FALSE;
    }
    pointer->pending_warp = TRUE;
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("warping to %d,%d\n", x, y);
    reapply_cursor_clipping();
    return TRUE;
}

/***********************************************************************
 *	     WAYLAND_ClipCursor
 */
BOOL WAYLAND_ClipCursor(const RECT *clip, BOOL reset)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;
    struct wl_surface *wl_surface = NULL;
    struct wayland_surface *surface = NULL;
    struct wayland_win_data *data;
    RECT confine_rect;
    POINT cursor_pos, warp;

    TRACE("clip=%s reset=%d\n", wine_dbgstr_rect(clip), reset);

    NtUserGetCursorPos(&cursor_pos);
    hwnd = NtUserGetForegroundWindow();

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    if ((surface = data->wayland_surface))
    {
        wl_surface = surface->wl_surface;
        if (clip) wayland_surface_calc_confine(surface, clip, &confine_rect);
        warp.x = cursor_pos.x - surface->window.rect.left;
        warp.y = cursor_pos.y - surface->window.rect.top;
        warp = map_point_to_surface(surface, warp);
    }
    wayland_win_data_release(data);

    pthread_mutex_lock(&pointer->mutex);
    if (wl_surface && pointer->pending_warp)
    {
        if (process_wayland.wp_pointer_warp_v1)
        {
            wp_pointer_warp_v1_warp_pointer(
                    process_wayland.wp_pointer_warp_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    wl_fixed_from_int(warp.x),
                    wl_fixed_from_int(warp.y),
                    pointer->enter_serial);
            TRACE("warp_pointer hwnd=%p wayland_xy=%s screen_xy=%s\n",
                    hwnd, wine_dbgstr_point(&warp), wine_dbgstr_point(&cursor_pos));
        }
        else
        {
            wayland_pointer_update_constraint(wl_surface, NULL, TRUE);
        }
        pointer->pending_warp = FALSE;
    }

    if (wl_surface && hwnd == pointer->constraint_hwnd && pointer->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_set_cursor_position_hint(
                pointer->zwp_locked_pointer_v1,
                wl_fixed_from_int(warp.x),
                wl_fixed_from_int(warp.y));
        pthread_mutex_unlock(&pointer->mutex);

        data = wayland_win_data_get(hwnd);
        wl_surface_commit(wl_surface);
        wayland_win_data_release(data);
        TRACE("position hint hwnd=%p wayland_xy=%s screen_xy=%s\n",
                hwnd, wine_dbgstr_point(&warp), wine_dbgstr_point(&cursor_pos));
        pthread_mutex_lock(&pointer->mutex);
    }

   /* Since we are running in the context of the foreground thread we know
    * that the wl_surface of the foreground HWND will not be invalidated,
    * so we can access it without having the win data lock. */
    wayland_pointer_update_constraint(wl_surface,
                                      (clip && wl_surface) ? &confine_rect : NULL,
                                      FALSE);
    pthread_mutex_unlock(&pointer->mutex);

    wl_display_flush(process_wayland.wl_display);

    return TRUE;
}
