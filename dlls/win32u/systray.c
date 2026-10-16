/*
 * Copyright 2023 Rémi Bernon for CodeWeavers
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
#include "win32u_private.h"
#include "ntuser_private.h"
#ifdef SONAME_LIBDBUS_1
#include "snidrv/snidrv.h"
#endif
#include "shellapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

#ifdef SONAME_LIBDBUS_1
static volatile LONG sni_initialized;
static volatile LONG dbus_notifications_initialized;
#endif

LRESULT system_tray_call( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, void *data )
{
#ifdef SONAME_LIBDBUS_1
    LONG initialized = InterlockedCompareExchange( &sni_initialized, FALSE, FALSE );

    if (!initialized && snidrv_init())
    {
        InterlockedCompareExchange( &sni_initialized, TRUE, FALSE );
        initialized = TRUE;
    }
    if (initialized)
    {
        if (msg == WINE_SYSTRAY_NOTIFY_ICON)
        {
            const NOTIFYICONDATAW *icon = data;

            switch (wparam)
            {
            case NIM_ADD:
                return snidrv_add_notify_icon( icon );
            case NIM_MODIFY:
                return snidrv_modify_notify_icon( icon );
            case NIM_DELETE:
                return snidrv_delete_notify_icon( hwnd, icon->uID );
            case NIM_SETVERSION:
                return snidrv_set_notify_icon_version( hwnd, icon->uID, icon->uVersion );
            default:
                FIXME( "Unknown NotifyIcon message %#x\n", (unsigned int)wparam );
                break;
            }
        }
        else if (msg == WINE_SYSTRAY_RUN_LOOP)
            return snidrv_run_loop();
        else if (msg == WINE_SYSTRAY_CLEANUP_ICONS)
            return snidrv_cleanup_notify_icons( hwnd );
    }

    if (msg == WINE_SYSTRAY_SHOW_BALLOON)
    {
        LONG initialized = InterlockedCompareExchange( &dbus_notifications_initialized, FALSE, FALSE );

        if (!initialized && snidrv_notification_init())
        {
            InterlockedCompareExchange( &dbus_notifications_initialized, TRUE, FALSE );
            initialized = TRUE;
        }
        if (initialized)
            return snidrv_show_balloon( hwnd, wparam, lparam, data );
    }
#endif

    switch (msg)
    {
    case WINE_SYSTRAY_NOTIFY_ICON:
        return user_driver->pNotifyIcon( hwnd, wparam, data );
    case WINE_SYSTRAY_CLEANUP_ICONS:
        user_driver->pCleanupIcons( hwnd );
        return 0;

    case WINE_SYSTRAY_DOCK_INIT:
        user_driver->pSystrayDockInit( hwnd );
        return 0;
    case WINE_SYSTRAY_DOCK_INSERT:
        return user_driver->pSystrayDockInsert( hwnd, wparam, lparam, data );
    case WINE_SYSTRAY_DOCK_CLEAR:
        user_driver->pSystrayDockClear( hwnd );
        return 0;
    case WINE_SYSTRAY_DOCK_REMOVE:
        return user_driver->pSystrayDockRemove( hwnd );

    case WINE_SYSTRAY_RUN_LOOP:
        return -1;

    case WINE_SYSTRAY_SHOW_BALLOON:
        return user_driver->pSystrayShowBalloon( hwnd, wparam, lparam, data );

    default:
        FIXME( "Unknown NtUserSystemTrayCall msg %#x\n", msg );
        break;
    }

    return -1;
}
