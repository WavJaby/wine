/*
 * wpnapps Unix interface: freedesktop notifications over the session bus
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

#ifndef __WINE_WPNAPPS_UNIXLIB_H
#define __WINE_WPNAPPS_UNIXLIB_H

#include "wine/unixlib.h"

/* All strings are UTF-8. */
struct notify_params
{
    const char *app_name;
    const char *summary;
    const char *body;
    const char *icon;        /* absolute Unix path or empty */
    UINT32 replaces_id;
    UINT32 id;               /* out */
};

struct close_params
{
    UINT32 id;
};

enum notification_event_type
{
    NOTIFICATION_EVENT_NONE,
    NOTIFICATION_EVENT_ACTIVATED,   /* ActionInvoked "default" */
    NOTIFICATION_EVENT_CLOSED,      /* NotificationClosed */
    NOTIFICATION_EVENT_SHUTDOWN,
};

struct wait_event_params
{
    UINT32 type;   /* out, enum notification_event_type */
    UINT32 id;     /* out */
    UINT32 reason; /* out, NotificationClosed reason */
};

struct init_params
{
    BOOL body_markup; /* out: server interprets markup in the body */
};

enum wpnapps_funcs
{
    unix_init,
    unix_notify,
    unix_close,
    unix_wait_event,
    unix_funcs_count,
};

#endif
