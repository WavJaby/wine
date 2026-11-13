/*
 * Wayland driver
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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

#ifndef __WINE_WAYLANDDRV_H
#define __WINE_WAYLANDDRV_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#include <pthread.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbregistry.h>
#include "cursor-shape-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-toplevel-tag-v1-client-protocol.h"
#include "pointer-warp-v1-client-protocol.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "wine/gdi_driver.h"
#include "wine/list.h"
#include "wine/rbtree.h"

#include "unixlib.h"

/* We only use 4 byte formats. */
#define WINEWAYLAND_BYTES_PER_PIXEL 4

/**********************************************************************
 *          Globals
 */

extern char *process_name;
extern struct wayland process_wayland;

/**********************************************************************
 *          Definitions for wayland types
 */

enum wayland_window_message
{
    WM_WAYLAND_INIT_DISPLAY_DEVICES = WM_WINE_FIRST_DRIVER_MSG,
    WM_WAYLAND_CONFIGURE,
    WM_WAYLAND_SET_FOREGROUND,
    WM_WAYLAND_MONITOR_CHANGED,
    WM_WAYLAND_FOCUS_OUT,
};

enum wayland_surface_config_state
{
    WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED = (1 << 0),
    WAYLAND_SURFACE_CONFIG_STATE_RESIZING = (1 << 1),
    WAYLAND_SURFACE_CONFIG_STATE_TILED = (1 << 2),
    WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN = (1 << 3)
};

enum wayland_surface_role
{
    WAYLAND_SURFACE_ROLE_NONE,
    WAYLAND_SURFACE_ROLE_TOPLEVEL,
    WAYLAND_SURFACE_ROLE_SUBSURFACE,
    WAYLAND_SURFACE_ROLE_POPUP,
};

struct wayland_keyboard
{
    struct wl_keyboard *wl_keyboard;
    struct xkb_context *xkb_context;
    struct xkb_state *xkb_state;
    HWND focused_hwnd;
    pthread_mutex_t mutex;
};

struct wayland_cursor
{
    struct wayland_shm_buffer *shm_buffer;
    struct wl_surface *wl_surface;
    struct wp_viewport *wp_viewport;
    int hotspot_x, hotspot_y;
};

struct wayland_pointer
{
    struct wl_pointer *wl_pointer;
    struct zwp_confined_pointer_v1 *zwp_confined_pointer_v1;
    struct zwp_locked_pointer_v1 *zwp_locked_pointer_v1;
    struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1;
    struct wp_cursor_shape_device_v1 *wp_cursor_shape_device_v1;
    HWND focused_hwnd;
    HWND constraint_hwnd;
    BOOL relative_mode;
    BOOL pending_warp;
    uint32_t enter_serial;
    uint32_t button_serial;
    POINT last_pos; /* latest motion, in the coordinates sent as hardware input */
    /* SC_MOVE waiting for the pointer to leave the drag rectangle; hwnd NULL when none. */
    struct
    {
        HWND hwnd;
        uint32_t serial;
        POINT origin;
        SIZE threshold;
    } deferred_move;
    struct wayland_cursor cursor;
    double accum_x;
    double accum_y;
    struct
    {
        double value;
        double remainder;
        int value120;
        BOOL has_value120;
        BOOL stopped;
        /* Recent finger-scroll samples (compositor ms, surface units) for the lift-off velocity. */
        struct { uint32_t time; double value; } samples[8];
        unsigned int sample_count;
        uint32_t stop_time;
        double velocity; /* inertia velocity, surface units per ms */
    } scroll[2];
    uint32_t scroll_source;
    BOOL inertia_active;
    HWND inertia_hwnd;
    UINT64 inertia_tick; /* monotonic ms of the last inertia step */
    pthread_mutex_t mutex;
};

struct wayland_touch_point
{
    struct wl_list link;
    WORD pointer_id;
    LPARAM xy;
    HWND focused_hwnd;
    int32_t id;
};

struct wayland_touch
{
    struct wl_touch *wl_touch;
    struct wl_list touch_points;
    HWND last_hwnd; /* window of the latest contact, until the pointer is used again; interlocked */
};

struct wayland_text_input
{
    struct zwp_text_input_v3 *zwp_text_input_v3;
    struct
    {
        WCHAR *string;
        DWORD cursor_pos;
    } preedit, current_preedit;
    WCHAR *commit_string;
    HWND focused_hwnd;
    pthread_mutex_t mutex;
};

struct wayland_seat
{
    struct wl_seat *wl_seat;
    uint32_t global_id;
    pthread_mutex_t mutex;
};

struct wayland_data_device
{
    union
    {
        struct
        {
            struct zwlr_data_control_device_v1 *zwlr_data_control_device_v1;
            struct zwlr_data_control_source_v1 *zwlr_data_control_source_v1;
            struct zwlr_data_control_offer_v1 *clipboard_zwlr_data_control_offer_v1;
        };
        struct
        {
            struct wl_data_device *wl_data_device;
            struct wl_data_source *wl_data_source;
            struct wl_data_offer *clipboard_wl_data_offer;
        };
    };
    pthread_mutex_t mutex;
};

struct wayland
{
    BOOL initialized;
    BOOL client_window_state;
    BOOL auto_monitor_dpi;
    struct wl_display *wl_display;
    struct wl_event_queue *wl_event_queue;
    struct wl_registry *wl_registry;
    struct zxdg_output_manager_v1 *zxdg_output_manager_v1;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_shm *wl_shm;
    struct wp_viewporter *wp_viewporter;
    struct wl_subcompositor *wl_subcompositor;
    struct wp_fractional_scale_manager_v1 *wp_fractional_scale_manager_v1;
    struct zwp_pointer_constraints_v1 *zwp_pointer_constraints_v1;
    struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager_v1;
    struct zwp_text_input_manager_v3 *zwp_text_input_manager_v3;
    struct zwlr_data_control_manager_v1 *zwlr_data_control_manager_v1;
    struct wl_data_device_manager *wl_data_device_manager;
    struct xdg_toplevel_icon_manager_v1 *xdg_toplevel_icon_manager_v1;
    struct xdg_toplevel_tag_manager_v1 *xdg_toplevel_tag_manager_v1;
    struct xdg_activation_v1 *xdg_activation_v1;
    struct wp_cursor_shape_manager_v1 *wp_cursor_shape_manager_v1;
    struct wp_pointer_warp_v1 *wp_pointer_warp_v1;
    struct wp_alpha_modifier_v1 *wp_alpha_modifier_v1;
    struct wayland_seat seat;
    struct wayland_keyboard keyboard;
    struct wayland_pointer pointer;
    struct wayland_touch touch;
    struct wayland_text_input text_input;
    struct wayland_data_device data_device;
#ifdef WL_FIXES_ACK_GLOBAL_REMOVE
    struct wl_fixes *wl_fixes;
#endif
    struct wl_list output_list;
    /* Protects the output_list and the wayland_output.current states. */
    pthread_mutex_t output_mutex;
    LONG input_serial;
};

struct wayland_output_mode
{
    struct rb_entry entry;
    int32_t width;
    int32_t height;
    int32_t refresh;
};

struct wayland_output_state
{
    int modes_count;
    struct rb_tree modes;
    struct wayland_output_mode *current_mode;
    char *name;
    int logical_x, logical_y;
    int logical_w, logical_h;
    BOOL has_logical_size;
    uint32_t learned_scale;
};

struct wayland_output
{
    struct wl_list link;
    struct wl_output *wl_output;
    struct zxdg_output_v1 *zxdg_output_v1;
    uint32_t global_id;
    unsigned int pending_flags;
    struct wayland_output_state pending;
    struct wayland_output_state current;
};

struct wayland_surface_config
{
    RECT rect;
    enum wayland_surface_config_state state;
    uint32_t serial;
    BOOL processed;
    BOOL answers_size_request; /* received after the compositor saw the last client size request */
    BOOL acked;                /* re-applied config whose serial was already acknowledged */
};

struct wayland_window_config
{
    RECT rect;
    enum wayland_surface_config_state state;
    /* The scale (i.e., normalized dpi) the window is rendering at. */
    double scale;
    UINT raw_dpi; /* DPI used to produce the cached Win32 window rectangles */
    BOOL visible;
    BOOL managed;
    BOOL minimized;
    BOOL resizeable;
    POINT min_track_size, max_track_size; /* visible raw-pixel bounds; zero maximum is unrestricted */
};

struct wayland_client_surface
{
    struct client_surface client;
    HWND toplevel;
    RECT rect;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct wp_viewport *wp_viewport;
};

extern struct wayland_client_surface *impl_from_client_surface(struct client_surface *client);

struct wayland_shm_buffer
{
    struct wl_list link;
    struct wl_buffer *wl_buffer;
    int width, height;
    uint32_t format;
    void *map_data;
    size_t map_size;
    BOOL busy;
    LONG ref;
    HRGN damage_region;
};

struct wayland_surface
{
    HWND hwnd;

    struct wl_surface *wl_surface;
    struct wl_array entered_outputs; /* Registry IDs, not output object pointers */
    BOOL has_preferred_scale;
    double preferred_scale; /* compositor preference, independent of current Win32 render DPI */
    struct wp_viewport *wp_viewport;
    struct wp_fractional_scale_v1 *wp_fractional_scale_v1;
    struct wayland_shm_buffer *small_icon_buffer;
    struct wayland_shm_buffer *big_icon_buffer;

    enum wayland_surface_role role;
    struct xdg_surface *xdg_surface; /* toplevel and popup */
    HWND owner_hwnd; /* subsurface and popup */
    union
    {
        struct
        {
            struct xdg_toplevel *xdg_toplevel;
            struct xdg_toplevel_icon_v1 *xdg_toplevel_icon;
            HWND toplevel_parent; /* window last passed to xdg_toplevel_set_parent */
            BOOL toplevel_placed; /* detached-popup decision taken; latched because pointer focus moves once mapped */
            BOOL toplevel_detached; /* opened while the pointer was off this process: tagged, never a subsurface */
        };
        struct
        {
            struct wl_subsurface *wl_subsurface;
        };
        struct
        {
            struct xdg_popup *xdg_popup;
            RECT popup_rect; /* last rect given to the positioner, owner surface coordinates */
            BOOL popup_configured;
        };
    };
    struct wp_alpha_modifier_surface_v1 *wp_alpha_modifier_surface_v1;

    struct wayland_surface_config pending, requested, processing, current;
    enum wayland_surface_config_state sent_window_state;
    RECT client_size_request;
    BOOL client_size_pending;
    struct wl_callback *client_size_sync; /* round trip in flight after a size request */
    unsigned int client_size_round;       /* 1: first sync in flight, 2: second sync in flight */
    BOOL client_size_answered;            /* a configure arrived during the second round trip */
    BOOL resizing;
    struct wayland_window_config window;
    int content_width, content_height;
    int geometry_width, geometry_height; /* last xdg window geometry size sent */
    uint32_t acked_serial;               /* newest configure serial acknowledged, 0 before the first */
    HCURSOR hcursor;
};

/**********************************************************************
 *          Wayland initialization
 */

BOOL wayland_process_init(void);

/**********************************************************************
 *          Wayland output
 */

BOOL wayland_output_create(uint32_t id, uint32_t version);
void wayland_output_destroy(struct wayland_output *output);
void wayland_output_use_xdg_extension(struct wayland_output *output);
double wayland_output_get_initial_scale(const RECT *window_rect);
BOOL wayland_output_get_monitor_rect(uint32_t global_id, RECT *rect);
BOOL wayland_output_set_surface_scale(uint32_t global_id, uint32_t scale);
BOOL wayland_output_set_work_size(uint32_t global_id, int width, int height);
BOOL wayland_output_get_work_size(uint32_t global_id, int *width, int *height);
BOOL wayland_output_update_monitor_dpi(uint32_t global_id, UINT dpi);
uint32_t wayland_output_get_surface_scale(uint32_t global_id);

/**********************************************************************
 *          Wayland surface
 */

struct wayland_surface *wayland_surface_create(HWND hwnd);
double wayland_surface_get_render_scale(struct wayland_surface *surface, double preferred);
void wayland_surface_update_track_sizes(struct wayland_surface *surface);
void wayland_surface_destroy(struct wayland_surface *surface);
void wayland_surface_make_toplevel(struct wayland_surface *surface, const RECT *window_rect);
void wayland_surface_make_popup(struct wayland_surface *surface, struct wayland_surface *owner,
                                const RECT *window_rect);
void wayland_surface_make_subsurface(struct wayland_surface *surface,
                                     struct wayland_surface *parent);
void wayland_surface_clear_role(struct wayland_surface *surface);
void wayland_surface_attach_shm(struct wayland_surface *surface,
                                struct wayland_shm_buffer *shm_buffer,
                                HRGN surface_damage_region);
BOOL wayland_surface_reconfigure(struct wayland_surface *surface);
void wayland_surface_request_client_size(struct wayland_surface *surface);
void wayland_surface_ack_configure(struct wayland_surface *surface, uint32_t serial);
BOOL wayland_surface_config_is_compatible(struct wayland_surface_config *conf, RECT rect,
                                          enum wayland_surface_config_state state);
RECT map_rect_to_surface(struct wayland_surface *surface, RECT rect);
POINT map_point_to_surface(struct wayland_surface *surface, POINT point);
RECT map_rect_from_surface(struct wayland_surface *surface, RECT rect);
POINT map_point_from_surface(struct wayland_surface *surface, POINT point);
void wayland_client_surface_attach(struct wayland_client_surface *client, HWND toplevel, const RECT *rect);
void wayland_surface_ensure_contents(struct wayland_surface *surface);
void wayland_surface_set_title(struct wayland_surface *surface, LPCWSTR title);
void wayland_surface_assign_icon(struct wayland_surface *surface);
void wayland_surface_set_icon_buffer(struct wayland_surface *surface, UINT type, const ICONINFO *ii);
void wayland_surface_set_opacity(struct wayland_surface *surface, BYTE alpha, UINT flags);

static inline BOOL wayland_surface_is_toplevel(struct wayland_surface *surface)
{
    return surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->xdg_toplevel;
}

/**********************************************************************
 *          Wayland SHM buffer
 */

struct wayland_shm_buffer *wayland_shm_buffer_create(int width, int height,
                                                     enum wl_shm_format format);
struct wayland_shm_buffer *wayland_shm_buffer_from_color_bitmaps(HDC hdc, HBITMAP color,
                                                                 HBITMAP mask, BOOL allow_padding);
void wayland_shm_buffer_ref(struct wayland_shm_buffer *shm_buffer);
void wayland_shm_buffer_unref(struct wayland_shm_buffer *shm_buffer);

/**********************************************************************
 *          Wayland Window
 */

/* private window data */
struct wayland_win_data
{
    struct rb_entry entry;
    /* hwnd that this private data belongs to */
    HWND hwnd;
    /* last buffer that was set as window contents */
    struct wayland_shm_buffer *window_contents;
    /* wayland surface (if any) for this window */
    struct wayland_surface *wayland_surface;
    /* wayland client surface (if any) for this window */
    struct wayland_client_surface *client_surface;
    /* window rects, relative to parent client area */
    struct window_rects rects;
    UINT raw_dpi;
    BOOL is_fullscreen;
    BOOL resizeable;
    POINT min_track_size, max_track_size; /* visible raw-pixel bounds; zero maximum is unrestricted */
    BOOL managed;
    BOOL layered_attribs_set;
    BOOL layered_lwa; /* layered through SetLayeredWindowAttributes: its non-layered surface carries the real content */
    BOOL ex_layered; /* WS_EX_LAYERED cache: surface flush runs under the window surface lock and must not call win32u */
    BOOL monitor_change_pending;
};

struct wayland_win_data *wayland_win_data_get(HWND hwnd);
void wayland_win_data_release(struct wayland_win_data *data);
HWND wayland_win_data_mark_monitor_change(struct wayland_win_data *data);
void wayland_post_monitor_change(HWND hwnd);
void wayland_windows_update_monitors(void);
void wayland_windows_remove_output(uint32_t global_id);

struct wayland_client_surface *get_client_surface(HWND hwnd);
void set_client_surface(HWND hwnd, struct wayland_client_surface *client);
BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer, HRGN damage_region);
struct wayland_shm_buffer *get_window_surface_contents(HWND hwnd);
void wayland_window_init(void);

/**********************************************************************
 *          Wayland Keyboard
 */

void wayland_keyboard_init(struct wl_keyboard *wl_keyboard);
void wayland_keyboard_deinit(void);
void wayland_keyboard_focus_out(HWND hwnd);
const KBDTABLES *WAYLAND_KbdLayerDescriptor(HKL hkl);
void WAYLAND_ReleaseKbdTables(const KBDTABLES *);
void activate_keyboard_hkl(HWND hwnd, BOOL ime);

/**********************************************************************
 *          Wayland pointer
 */

void wayland_pointer_init(struct wl_pointer *wl_pointer);
void wayland_pointer_deinit(void);
int wayland_pointer_inertia_timeout(void);
void wayland_pointer_inertia_step(void);
void wayland_pointer_clear_constraint(void);
void wayland_pointer_defer_move(HWND hwnd, uint32_t serial);


/**********************************************************************
 *          Wayland touch
 */
void wayland_touch_init(struct wl_touch *wl_touch);
void wayland_touch_deinit(void);

/**********************************************************************
 *          Wayland text input
 */

void wayland_text_input_init(void);
void wayland_text_input_deinit(void);

/**********************************************************************
 *          Wayland data device
 */

void wayland_data_device_init(void);

/**********************************************************************
 *          Helpers
 */

static inline BOOL intersect_rect(RECT *dst, const RECT *src1, const RECT *src2)
{
    dst->left = max(src1->left, src2->left);
    dst->top = max(src1->top, src2->top);
    dst->right = min(src1->right, src2->right);
    dst->bottom = min(src1->bottom, src2->bottom);
    return !IsRectEmpty(dst);
}

static inline LRESULT send_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return NtUserMessageCall(hwnd, msg, wparam, lparam, NULL, NtUserSendMessage, FALSE);
}

RGNDATA *get_region_data(HRGN region);

/**********************************************************************
 *          USER driver functions
 */

LRESULT WAYLAND_ClipboardWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
BOOL WAYLAND_ClipCursor(const RECT *clip, BOOL reset);
LRESULT WAYLAND_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void WAYLAND_DestroyWindow(HWND hwnd);
BOOL WAYLAND_SetIMECompositionRect(HWND hwnd, RECT rect);
void WAYLAND_SetCursor(HWND hwnd, HCURSOR hcursor);
BOOL WAYLAND_SetCursorPos(INT x, INT y);
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags);
void WAYLAND_SetWindowIcons(HWND hwnd, HICON icon, const ICONINFO *ii, HICON icon_small, const ICONINFO *ii_small);
void WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style);
void WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text);
void WAYLAND_ActivateWindow(HWND hwnd, HWND previous);
UINT WAYLAND_GetMaximumTouches(void);
LRESULT WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos);
void WAYLAND_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags);
UINT WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager, void *param);
LRESULT WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface);
BOOL WAYLAND_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects);
struct client_surface *WAYLAND_CreateClientSurface(HWND hwnd, int pixel_format, BOOL raw);
BOOL WAYLAND_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface);
UINT WAYLAND_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs);
UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs);

#endif /* __WINE_WAYLANDDRV_H */
