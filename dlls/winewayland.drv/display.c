/*
 * WAYLAND display device functions
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "ntstatus.h"
#include "waylanddrv.h"

#include "wine/debug.h"

#include "ntuser.h"

#include <math.h>
#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct output_info
{
    int x, y;
    struct wayland_output_state *output;
    uint32_t global_id;
};

static int output_info_cmp_primary_x_y(const void *va, const void *vb)
{
    const struct output_info *a = va;
    const struct output_info *b = vb;
    BOOL a_is_primary = a->x == 0 && a->y == 0;
    BOOL b_is_primary = b->x == 0 && b->y == 0;

    if (a_is_primary && !b_is_primary) return -1;
    if (!a_is_primary && b_is_primary) return 1;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    if (a->y < b->y) return -1;
    if (a->y > b->y) return 1;
    return strcmp(a->output->name, b->output->name);
}

static inline BOOL output_info_overlap(struct output_info *a, struct output_info *b)
{
    return b->x < a->x + a->output->current_mode->width &&
           b->x + b->output->current_mode->width > a->x &&
           b->y < a->y + a->output->current_mode->height &&
           b->y + b->output->current_mode->height > a->y;
}

/* Map a point to one of the four quadrants of our 2d coordinate space:
 * 0: bottom right (x >= 0, y >= 0)
 * 1: top right (x >= 0, y < 0)
 * 2: bottom left (x < 0, y >= 0)
 * 3: top left (x < 0, y < 0) */
static inline int point_to_quadrant(int x, int y)
{
    return (x < 0) * 2 + (y < 0);
}

/* Decide which of two outputs to keep stationary in order
 * to resolve an overlap. */
static struct output_info *output_info_get_overlap_anchor(struct output_info *a,
                                                          struct output_info *b)
{
    /* Preferences for the direction of growth in each quadrant, with a
     * lower value signifying a higher preference. */
    static const int quadrant_prefs[4][4] =
    {
        {0, 1, 2, 3}, /* quadrant 0 */
        {3, 0, 2, 1}, /* quadrant 1 */
        {2, 3, 0, 1}, /* quadrant 2 */
        {3, 2, 1, 0}, /* quadrant 3 */
    };
    int qa = point_to_quadrant(a->output->logical_x, a->output->logical_y);
    int qb = point_to_quadrant(b->output->logical_x, b->output->logical_y);
    /* Direction of growth if a is the anchor. */
    int qab = point_to_quadrant(b->output->logical_x - a->output->logical_x,
                                b->output->logical_y - a->output->logical_y);
    /* Direction of growth if b is the anchor. */
    int qba = point_to_quadrant(a->output->logical_x - b->output->logical_x,
                                a->output->logical_y - b->output->logical_y);

    /* If the two output origins are in different quadrants, use the output
     * in the lower valued quadrant as the anchor (so effectively outputs
     * grow/move away from quadrant 0). */
    if (qa != qb) return (qa < qb) ? a : b;

    /* If the outputs are in the same quadrant, use the preference for the
     * direction of growth in that quadrant to select the anchor. Again the
     * intended effect is to grow/move outputs away from the origin. */
    return (quadrant_prefs[qa][qab] < quadrant_prefs[qa][qba]) ? a : b;
}

static BOOL output_info_array_resolve_overlaps(struct wl_array *output_info_array)
{
    struct output_info *a, *b;
    BOOL found_overlap = FALSE;

    wl_array_for_each(a, output_info_array)
    {
        wl_array_for_each(b, output_info_array)
        {
            struct output_info *anchor, *move;
            BOOL x_use_end, y_use_end;
            double rel_x, rel_y;

            /* Break if we reach the same output in the inner loop, so that we
             * don't process output pairs twice (since order doesn't matter for
             * our algorithm.) */
            if (a == b) break;

            if (!output_info_overlap(a, b)) continue;
            found_overlap = TRUE;

            /* Decide which output to move to resolve the overlap. */
            anchor = output_info_get_overlap_anchor(a, b);
            move = anchor == a ? b : a;

            /* Move the selected output on the X axis to resolve the overlap,
             * while maintaining the same relative positioning of the outputs as
             * the one they have in logical space. Use either the start or end
             * of the moved output as the point to maintain the relative
             * position of, depending on whether the anchor is before or after
             * the moved output on the axis. */
            x_use_end = move->output->logical_x < anchor->output->logical_x;
            rel_x = (move->output->logical_x - anchor->output->logical_x +
                     (x_use_end ? move->output->logical_w : 0)) /
                    (double)anchor->output->logical_w;
            move->x = anchor->x + anchor->output->current_mode->width * rel_x -
                      (x_use_end ? move->output->current_mode->width : 0);

            /* Similarly for the Y axis. */
            y_use_end = move->output->logical_y < anchor->output->logical_y;
            rel_y = (move->output->logical_y - anchor->output->logical_y +
                     (y_use_end ? move->output->logical_h : 0)) /
                    (double)anchor->output->logical_h;
            move->y = anchor->y + anchor->output->current_mode->height * rel_y -
                      (y_use_end ? move->output->current_mode->height : 0);
        }
    }

    return found_overlap;
}

static void output_info_array_arrange_physical_coords(struct wl_array *output_info_array)
{
    struct output_info *info;
    size_t num_outputs = output_info_array->size / sizeof(struct output_info);
    int steps = 0;

    /* Set the initial physical pixel coordinates. */
    wl_array_for_each(info, output_info_array)
    {
        info->x = info->output->logical_x;
        info->y = info->output->logical_y;
    }

    /* Try to iteratively resolve overlaps, but be defensive and set an upper
     * iteration bound to ensure we avoid infinite loops. */
    while (output_info_array_resolve_overlaps(output_info_array) &&
           ++steps < num_outputs)
        continue;

    /* Now that we have our physical pixel coordinates, sort from physical left
     * to right, but ensure the primary output is first. */
    qsort(output_info_array->data, num_outputs, sizeof(struct output_info),
          output_info_cmp_primary_x_y);
}

/* The caller must hold process_wayland.output_mutex. */
static void output_info_array_populate(struct wl_array *output_info_array)
{
    struct wayland_output *output;
    struct output_info *info;

    wl_list_for_each(output, &process_wayland.output_list, link)
    {
        if (!output->current.current_mode) continue;
        info = wl_array_add(output_info_array, sizeof(*info));
        if (info)
        {
            info->output = &output->current;
            info->global_id = output->global_id;
            if (process_wayland.auto_monitor_dpi && !info->output->has_logical_size)
            {
                uint32_t scale = wayland_output_get_surface_scale(output->global_id);
                info->output->learned_scale = scale;
                if (scale)
                {
                    info->output->logical_w = lround(info->output->current_mode->width * 120.0 / scale);
                    info->output->logical_h = lround(info->output->current_mode->height * 120.0 / scale);
                }
            }
        }
        else ERR("Failed to allocate space for output_info\n");
    }

    output_info_array_arrange_physical_coords(output_info_array);
}

/***********************************************************************
 *      wayland_output_get_initial_scale
 *
 * Estimate the scale for a new toplevel before it receives its per-surface
 * fractional-scale event. The event remains authoritative after first map.
 */
double wayland_output_get_initial_scale(const RECT *window_rect)
{
    MONITORINFO monitor_info = {.cbSize = sizeof(monitor_info)};
    struct output_info *info, *primary;
    struct wl_array output_info_array;
    HMONITOR monitor;
    double scale = 1.0;
    RECT output_rect;

    if (!(monitor = NtUserMonitorFromRect(window_rect, MONITOR_DEFAULTTONEAREST)) ||
        !NtUserGetMonitorInfo(monitor, &monitor_info)) return scale;

    wl_array_init(&output_info_array);
    pthread_mutex_lock(&process_wayland.output_mutex);
    output_info_array_populate(&output_info_array);

    primary = output_info_array.size ? output_info_array.data : NULL;
    wl_array_for_each(info, &output_info_array)
    {
        const struct wayland_output_state *output = info->output;
        double width_scale, height_scale;

        SetRect(&output_rect, info->x - primary->x, info->y - primary->y,
                info->x - primary->x + output->current_mode->width,
                info->y - primary->y + output->current_mode->height);

        if (output_rect.left != monitor_info.rcMonitor.left ||
            output_rect.top != monitor_info.rcMonitor.top ||
            output_rect.right != monitor_info.rcMonitor.right ||
            output_rect.bottom != monitor_info.rcMonitor.bottom ||
            !output->logical_w || !output->logical_h) continue;

        width_scale = (double)output->current_mode->width / output->logical_w;
        height_scale = (double)output->current_mode->height / output->logical_h;

        /* A rotated or otherwise mismatched output cannot provide a safe
         * estimate without its transform. Keep the existing 1.0 fallback. */
        if (width_scale >= 0.5 && width_scale <= 8.0 &&
            fabs(width_scale - height_scale) <= 0.01)
            scale = round((width_scale + height_scale) * 60.0) / 120.0;

        TRACE("window=%s output=%s mode=%dx%d logical=%dx%d initial_scale=%f\n",
              wine_dbgstr_rect(window_rect), output->name,
              output->current_mode->width, output->current_mode->height,
              output->logical_w, output->logical_h, scale);
        break;
    }

    pthread_mutex_unlock(&process_wayland.output_mutex);
    wl_array_release(&output_info_array);
    return scale;
}

/* Resolve an entered output to the current virtual Win32 display coordinates. */
BOOL wayland_output_get_monitor_rect(uint32_t global_id, RECT *rect)
{
    struct output_info *info;
    struct wl_array outputs;
    DEVMODEW mode = {.dmSize = sizeof(mode)};
    UNICODE_STRING device;
    WCHAR name[32];
    char ascii[32];
    unsigned int index = 0, found = 0, i;

    wl_array_init(&outputs);
    pthread_mutex_lock(&process_wayland.output_mutex);
    output_info_array_populate(&outputs);
    wl_array_for_each(info, &outputs)
    {
        ++index;
        if (info->global_id == global_id) found = index;
    }
    pthread_mutex_unlock(&process_wayland.output_mutex);
    wl_array_release(&outputs);
    if (!found) return FALSE;

    snprintf(ascii, sizeof(ascii), "\\\\.\\DISPLAY%u", found);
    for (i = 0; (name[i] = ascii[i]); ++i) continue;
    RtlInitUnicodeString(&device, name);
    if (!NtUserEnumDisplaySettings(&device, ENUM_CURRENT_SETTINGS, &mode, 0)) return FALSE;
    SetRect(rect, mode.dmPosition.x, mode.dmPosition.y,
            mode.dmPosition.x + mode.dmPelsWidth, mode.dmPosition.y + mode.dmPelsHeight);
    return TRUE;
}

static void wayland_add_device_gpu(const struct gdi_device_manager *device_manager,
                                   void *param)
{
    struct pci_id pci_id = {0};

    TRACE("\n");

    device_manager->add_gpu(NULL, &pci_id, NULL, param);
}

static void wayland_add_device_source(const struct gdi_device_manager *device_manager,
                                       void *param, UINT state_flags, struct output_info *output_info)
{
    UINT base_dpi = NtUserCallNoParam( NtUserCallNoParam_GetBaseDpi );
    UINT dpi = base_dpi;

    /* The configured system DPI is the user's base size at output scale 1.
     * Apply the observed output scale once, independently of buffer scaling.
     * Missing or rotated metadata must retain the configured base DPI. */
    if (process_wayland.auto_monitor_dpi && output_info->output->has_logical_size &&
        output_info->output->logical_w > 0 && output_info->output->logical_h > 0)
    {
        double scale_x = (double)output_info->output->current_mode->width / output_info->output->logical_w;
        double scale_y = (double)output_info->output->current_mode->height / output_info->output->logical_h;

        if (scale_x >= 0.5 && scale_x <= 8.0 && fabs(scale_x - scale_y) <= 0.01)
            dpi = lround(base_dpi * (scale_x + scale_y) / 2.0);
    }
    else if (process_wayland.auto_monitor_dpi && output_info->output->learned_scale)
        dpi = ((UINT64)output_info->output->learned_scale * base_dpi + 60) / 120;
    TRACE("output=%s auto_dpi=%u base_dpi=%u dpi=%u metadata=%u\n", output_info->output->name,
          process_wayland.auto_monitor_dpi, base_dpi, dpi, output_info->output->has_logical_size);
    TRACE("name=%s state_flags=0x%x\n",
          output_info->output->name, state_flags);
    device_manager->add_source(output_info->output->name, state_flags, dpi, param);
}

static void wayland_add_device_monitor(const struct gdi_device_manager *device_manager,
                                       void *param, struct output_info *output_info,
                                       struct output_info *primary)
{
    struct gdi_monitor monitor = {0};
    int work_w, work_h;

    SetRect(&monitor.rc_monitor, output_info->x, output_info->y,
            output_info->x + output_info->output->current_mode->width,
            output_info->y + output_info->output->current_mode->height);
    OffsetRect(&monitor.rc_monitor, -primary->x, -primary->y);

    /* Wayland has no work-area protocol; use the maximized size learned for this output, if any. */
    monitor.rc_work = monitor.rc_monitor;
    if (wayland_output_get_work_size(output_info->global_id, &work_w, &work_h) &&
        work_w <= monitor.rc_monitor.right - monitor.rc_monitor.left &&
        work_h <= monitor.rc_monitor.bottom - monitor.rc_monitor.top)
    {
        monitor.rc_work.right = monitor.rc_work.left + work_w;
        monitor.rc_work.bottom = monitor.rc_work.top + work_h;
    }

    TRACE("name=%s rc_monitor=%s rc_work=%s\n", output_info->output->name,
          wine_dbgstr_rect(&monitor.rc_monitor), wine_dbgstr_rect(&monitor.rc_work));

    device_manager->add_monitor(&monitor, param);
}

static void populate_devmode(struct wayland_output_mode *output_mode, DEVMODEW *mode)
{
    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                     DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    mode->dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmDisplayFlags = 0;
    mode->dmBitsPerPel = 32;
    mode->dmPelsWidth = output_mode->width;
    mode->dmPelsHeight = output_mode->height;
    /* Round the refresh rate to calculate the win32 display frequency. */
    mode->dmDisplayFrequency = (output_mode->refresh + 500) / 1000;
}

static void wayland_add_device_modes(const struct gdi_device_manager *device_manager,
                                     void *param, struct output_info *output_info,
                                     struct output_info *primary)
{
    DEVMODEW *modes, current = {.dmSize = sizeof(current)};
    struct wayland_output_mode *output_mode;
    int modes_count = 0;

    if (!(modes = malloc(output_info->output->modes_count * sizeof(*modes))))
        return;

    populate_devmode(output_info->output->current_mode, &current);
    current.dmFields |= DM_POSITION;
    current.dmPosition.x = output_info->x - primary->x;
    current.dmPosition.y = output_info->y - primary->y;

    RB_FOR_EACH_ENTRY(output_mode, &output_info->output->modes,
                      struct wayland_output_mode, entry)
    {
        DEVMODEW mode = {.dmSize = sizeof(mode)};
        populate_devmode(output_mode, &mode);
        /* win32u can select a single host mode as the physical mode. Keep
         * its position consistent with the separately supplied current mode. */
        mode.dmFields |= DM_POSITION;
        mode.dmPosition = current.dmPosition;
        modes[modes_count++] = mode;
    }

    device_manager->add_modes(&current, modes_count, modes, param);
    free(modes);
}

/***********************************************************************
 *      UpdateDisplayDevices (WAYLAND.@)
 */
UINT WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager, void *param)
{
    DWORD state_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    struct output_info *primary = NULL, *output_info;
    struct wl_array output_info_array;

    TRACE("\n");

    wl_array_init(&output_info_array);

    pthread_mutex_lock(&process_wayland.output_mutex);

    output_info_array_populate(&output_info_array);

    /* Populate GDI devices. */
    wayland_add_device_gpu(device_manager, param);

    wl_array_for_each(output_info, &output_info_array)
    {
        if (!primary) primary = output_info;
        wayland_add_device_source(device_manager, param, state_flags, output_info);
        wayland_add_device_monitor(device_manager, param, output_info, primary);
        wayland_add_device_modes(device_manager, param, output_info, primary);
        state_flags &= ~DISPLAY_DEVICE_PRIMARY_DEVICE;
    }

    wl_array_release(&output_info_array);

    pthread_mutex_unlock(&process_wayland.output_mutex);

    return STATUS_SUCCESS;
}
