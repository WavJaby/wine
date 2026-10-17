/*
 * wpnapps Unix side: org.freedesktop.Notifications client
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

#include <stdarg.h>
#include <poll.h>
#include <string.h>
#include <dbus/dbus.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpnapps);

#define NOTIFICATIONS_NAME "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"

/* Method calls run on the application's UI thread: never block it on a hung notification server (a stuck
 * tray host already froze LINE once through snidrv). A healthy server answers in single-digit ms. */
static const int call_timeout_ms = 1000;

static DBusConnection *connection;

static DBusMessage *call( const char *method, DBusMessage *msg )
{
    DBusMessage *reply;
    DBusError error;

    dbus_error_init( &error );
    reply = dbus_connection_send_with_reply_and_block( connection, msg, call_timeout_ms, &error );
    if (!reply) WARN( "%s failed: %s: %s\n", method, error.name, error.message );
    dbus_error_free( &error );
    dbus_message_unref( msg );
    return reply;
}

static BOOL server_has_capability( const char *capability )
{
    DBusMessageIter iter, array;
    DBusMessage *reply;
    BOOL found = FALSE;

    if (!(reply = call( "GetCapabilities", dbus_message_new_method_call( NOTIFICATIONS_NAME, NOTIFICATIONS_PATH,
                                                                         NOTIFICATIONS_NAME, "GetCapabilities" ) )))
        return FALSE;
    if (dbus_message_iter_init( reply, &iter ) && dbus_message_iter_get_arg_type( &iter ) == DBUS_TYPE_ARRAY)
    {
        dbus_message_iter_recurse( &iter, &array );
        while (dbus_message_iter_get_arg_type( &array ) == DBUS_TYPE_STRING)
        {
            const char *value;
            dbus_message_iter_get_basic( &array, &value );
            if (!strcmp( value, capability )) found = TRUE;
            dbus_message_iter_next( &array );
        }
    }
    dbus_message_unref( reply );
    return found;
}

static NTSTATUS wpnapps_init( void *args )
{
    struct init_params *params = args;
    DBusError error;

    dbus_threads_init_default();
    dbus_error_init( &error );
    if (!(connection = dbus_bus_get_private( DBUS_BUS_SESSION, &error )))
    {
        WARN( "no session bus: %s: %s\n", error.name, error.message );
        dbus_error_free( &error );
        return STATUS_NOT_SUPPORTED;
    }
    dbus_connection_set_exit_on_disconnect( connection, FALSE );
    dbus_bus_add_match( connection, "type='signal',interface='" NOTIFICATIONS_NAME "',member='ActionInvoked'", NULL );
    dbus_bus_add_match( connection, "type='signal',interface='" NOTIFICATIONS_NAME "',member='NotificationClosed'", NULL );
    params->body_markup = server_has_capability( "body-markup" );
    TRACE( "connected, body markup %d\n", params->body_markup );
    return STATUS_SUCCESS;
}

static NTSTATUS wpnapps_notify( void *args )
{
    struct notify_params *params = args;
    static const char *actions[] = {"default", "Open"};
    const char **actions_ptr = actions;
    DBusMessageIter iter, hints, entry, variant;
    DBusMessage *msg, *reply;
    dbus_int32_t expire = -1; /* server default */
    dbus_uint32_t id = 0;
    const char *key = "image-path";

    if (!connection) return STATUS_NOT_SUPPORTED;
    if (!(msg = dbus_message_new_method_call( NOTIFICATIONS_NAME, NOTIFICATIONS_PATH, NOTIFICATIONS_NAME, "Notify" )))
        return STATUS_NO_MEMORY;
    dbus_message_append_args( msg, DBUS_TYPE_STRING, &params->app_name, DBUS_TYPE_UINT32, &params->replaces_id,
                              DBUS_TYPE_STRING, &params->icon, DBUS_TYPE_STRING, &params->summary,
                              DBUS_TYPE_STRING, &params->body,
                              DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &actions_ptr, (int)ARRAY_SIZE(actions),
                              DBUS_TYPE_INVALID );
    dbus_message_iter_init_append( msg, &iter );
    dbus_message_iter_open_container( &iter, DBUS_TYPE_ARRAY, "{sv}", &hints );
    if (params->icon[0])
    {
        dbus_message_iter_open_container( &hints, DBUS_TYPE_DICT_ENTRY, NULL, &entry );
        dbus_message_iter_append_basic( &entry, DBUS_TYPE_STRING, &key );
        dbus_message_iter_open_container( &entry, DBUS_TYPE_VARIANT, "s", &variant );
        dbus_message_iter_append_basic( &variant, DBUS_TYPE_STRING, &params->icon );
        dbus_message_iter_close_container( &entry, &variant );
        dbus_message_iter_close_container( &hints, &entry );
    }
    dbus_message_iter_close_container( &iter, &hints );
    dbus_message_iter_append_basic( &iter, DBUS_TYPE_INT32, &expire );

    if (!(reply = call( "Notify", msg ))) return STATUS_UNSUCCESSFUL;
    dbus_message_get_args( reply, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID );
    dbus_message_unref( reply );
    params->id = id;
    TRACE( "id %u\n", id );
    return id ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS wpnapps_close( void *args )
{
    struct close_params *params = args;
    dbus_uint32_t id = params->id;
    DBusMessage *msg, *reply;

    if (!connection) return STATUS_NOT_SUPPORTED;
    if (!(msg = dbus_message_new_method_call( NOTIFICATIONS_NAME, NOTIFICATIONS_PATH, NOTIFICATIONS_NAME,
                                              "CloseNotification" )))
        return STATUS_NO_MEMORY;
    dbus_message_append_args( msg, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID );
    if (!(reply = call( "CloseNotification", msg ))) return STATUS_UNSUCCESSFUL;
    dbus_message_unref( reply );
    return STATUS_SUCCESS;
}

/* While a thread sits in dbus_connection_read_write(-1) it owns the connection's I/O path, so a Notify or
 * CloseNotification from the UI thread cannot even be written: that deadlocked LINE's main window. Poll the
 * socket ourselves and only enter libdbus non-blocking. */
static const int event_poll_ms = 200; /* also catches signals the UI thread queued while awaiting a reply */

/* Blocks the dedicated PE event thread until a signal for some notification arrives. Signals for ids we did
 * not create are passed up too; the PE side ignores unknown ids. */
static NTSTATUS wpnapps_wait_event( void *args )
{
    struct wait_event_params *params = args;
    DBusMessage *msg;

    if (!connection) return STATUS_NOT_SUPPORTED;
    for (;;)
    {
        while ((msg = dbus_connection_pop_message( connection )))
        {
            dbus_uint32_t id = 0, reason = 0;
            const char *action = NULL;
            UINT32 type = NOTIFICATION_EVENT_NONE;

            if (dbus_message_is_signal( msg, NOTIFICATIONS_NAME, "ActionInvoked" ) &&
                dbus_message_get_args( msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &action, DBUS_TYPE_INVALID ) &&
                !strcmp( action, "default" ))
                type = NOTIFICATION_EVENT_ACTIVATED;
            else if (dbus_message_is_signal( msg, NOTIFICATIONS_NAME, "NotificationClosed" ) &&
                     dbus_message_get_args( msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID ))
                type = NOTIFICATION_EVENT_CLOSED;
            dbus_message_unref( msg );
            if (type == NOTIFICATION_EVENT_NONE) continue;
            params->type = type;
            params->id = id;
            params->reason = reason;
            return STATUS_SUCCESS;
        }
        {
            struct pollfd pfd = {.events = POLLIN};
            int fd;

            if (!dbus_connection_get_unix_fd( connection, &fd ) || !dbus_connection_get_is_connected( connection ))
            {
                params->type = NOTIFICATION_EVENT_SHUTDOWN;
                return STATUS_SUCCESS;
            }
            pfd.fd = fd;
            if (poll( &pfd, 1, event_poll_ms ) > 0 && !dbus_connection_read_write( connection, 0 ))
            {
                params->type = NOTIFICATION_EVENT_SHUTDOWN;
                return STATUS_SUCCESS;
            }
        }
    }
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    wpnapps_init,
    wpnapps_notify,
    wpnapps_close,
    wpnapps_wait_event,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

static NTSTATUS wow64_not_supported( void *args )
{
    return STATUS_NOT_SUPPORTED;
}

/* 32-bit callers would need thunked pointer structs; toasts are disabled for them. */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_not_supported,
    wow64_not_supported,
    wow64_not_supported,
    wow64_not_supported,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif
