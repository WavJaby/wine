/*
 * Host desktop integration for shell32 (XDG desktop portal)
 *
 * Copyright 2026 WavJaby
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
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <dbus/dbus.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#ifndef SONAME_LIBDBUS_1
#define SONAME_LIBDBUS_1 "libdbus-1.so.3"
#endif

/* The method reply only carries the Request handle (measured 70-300 ms); bounds a dead or hung portal -> fallback. */
#define PORTAL_CALL_TIMEOUT_MS 5000
/* Response follows once the file manager answered ShowItems: measured 70-300 ms, Nautilus cold start included.
 * Past this the request is still in flight -> reported as success, so Wine's window never doubles a late Nautilus. */
#define PORTAL_RESPONSE_TIMEOUT_MS 10000

#define DBUS_FUNCS \
    DO_FUNC(dbus_bus_add_match); \
    DO_FUNC(dbus_bus_get_private); \
    DO_FUNC(dbus_connection_close); \
    DO_FUNC(dbus_connection_pop_message); \
    DO_FUNC(dbus_connection_read_write); \
    DO_FUNC(dbus_connection_send_with_reply_and_block); \
    DO_FUNC(dbus_connection_set_exit_on_disconnect); \
    DO_FUNC(dbus_connection_unref); \
    DO_FUNC(dbus_error_free); \
    DO_FUNC(dbus_error_init); \
    DO_FUNC(dbus_message_append_args); \
    DO_FUNC(dbus_message_get_args); \
    DO_FUNC(dbus_message_get_path); \
    DO_FUNC(dbus_message_is_signal); \
    DO_FUNC(dbus_message_iter_close_container); \
    DO_FUNC(dbus_message_iter_init_append); \
    DO_FUNC(dbus_message_iter_open_container); \
    DO_FUNC(dbus_message_new_method_call); \
    DO_FUNC(dbus_message_unref);

#define DO_FUNC(f) static typeof(f) * p_##f
DBUS_FUNCS;
#undef DO_FUNC

static BOOL dbus_loaded;

static void load_dbus(void)
{
    void *handle;

    if (!(handle = dlopen( SONAME_LIBDBUS_1, RTLD_NOW )))
    {
        WARN( "failed to load %s: %s\n", SONAME_LIBDBUS_1, dlerror() );
        return;
    }
#define DO_FUNC(f) if (!(p_##f = dlsym( handle, #f ))) { WARN( "missing %s\n", #f ); return; }
    DBUS_FUNCS;
#undef DO_FUNC
    dbus_loaded = TRUE;
}

static int elapsed_ms( const struct timespec *start )
{
    struct timespec now;

    clock_gettime( CLOCK_MONOTONIC, &now );
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* Response code of portal request `handle`: 0 success, 1 cancelled, 2 failed; -1 none within the timeout.
 * Signals that arrived while send_with_reply_and_block waited sit in the incoming queue, so none is missed. */
static int wait_response( DBusConnection *conn, const char *handle )
{
    struct timespec start;
    DBusMessage *msg;
    dbus_uint32_t code;
    BOOL match;
    int left;

    clock_gettime( CLOCK_MONOTONIC, &start );
    for (;;)
    {
        while ((msg = p_dbus_connection_pop_message( conn )))
        {
            match = p_dbus_message_is_signal( msg, "org.freedesktop.portal.Request", "Response" )
                    && p_dbus_message_get_path( msg ) && !strcmp( p_dbus_message_get_path( msg ), handle )
                    && p_dbus_message_get_args( msg, NULL, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID );
            p_dbus_message_unref( msg );
            if (match) return code;
        }
        if ((left = PORTAL_RESPONSE_TIMEOUT_MS - elapsed_ms( &start )) <= 0) return -1;
        if (!p_dbus_connection_read_write( conn, left )) return -1;  /* disconnected */
    }
}

/* org.freedesktop.portal.OpenURI.OpenDirectory: the host file manager opens the parent folder with the item selected.
 * Portal rather than org.freedesktop.FileManager1 directly -> works inside a Flatpak sandbox without a talk permission.
 * Stays connected until the Response: a caller that disconnects right after the method reply leaves the portal's
 * ShowItems call to a cold-starting Nautilus hanging for its full 25 s timeout before the window appears (measured,
 * sandboxed caller; waiting -> 0.3 s). Explorer exits right after this returns, so the wait cannot be skipped. */
static NTSTATUS reveal_in_host( void *args )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    static const char match_rule[] = "type='signal',sender='org.freedesktop.portal.Desktop',"
                                     "interface='org.freedesktop.portal.Request',member='Response'";
    struct reveal_in_host_params *params = args;
    DBusMessageIter iter, options;
    DBusMessage *msg, *reply = NULL;
    DBusConnection *conn;
    const char *parent_window = "", *handle;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    DBusError error;
    int fd, code;

    pthread_once( &once, load_dbus );
    if (!dbus_loaded) return STATUS_NOT_SUPPORTED;

    /* Not O_PATH: from inside a Flatpak sandbox the portal answers an O_PATH fd with Response 2 (failure), while the
     * same call from an unsandboxed host process succeeds. Unreadable item -> open fails -> Wine explorer fallback. */
    if ((fd = open( params->unix_path, O_RDONLY | O_CLOEXEC )) < 0)
    {
        WARN( "cannot open %s\n", debugstr_a(params->unix_path) );
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    p_dbus_error_init( &error );
    /* Private connection: the shared one is libdbus process state other modules (snidrv) also use, and its default
     * exit-on-disconnect would let a bus restart kill the application. */
    if (!(conn = p_dbus_bus_get_private( DBUS_BUS_SESSION, &error )))
    {
        WARN( "no session bus: %s\n", debugstr_a(error.message) );
        p_dbus_error_free( &error );
        close( fd );
        return STATUS_NOT_SUPPORTED;
    }
    p_dbus_connection_set_exit_on_disconnect( conn, FALSE );
    /* Before the call: the Response may be emitted before the method reply is read. */
    p_dbus_bus_add_match( conn, match_rule, NULL );

    if ((msg = p_dbus_message_new_method_call( "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.OpenURI", "OpenDirectory" )))
    {
        p_dbus_message_append_args( msg, DBUS_TYPE_STRING, &parent_window, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID );
        p_dbus_message_iter_init_append( msg, &iter );
        p_dbus_message_iter_open_container( &iter, DBUS_TYPE_ARRAY, "{sv}", &options );
        p_dbus_message_iter_close_container( &iter, &options );
        reply = p_dbus_connection_send_with_reply_and_block( conn, msg, PORTAL_CALL_TIMEOUT_MS, &error );
        p_dbus_message_unref( msg );
    }
    close( fd );  /* append_args dups the fd into the message */

    if (!reply) WARN( "OpenDirectory failed: %s\n", debugstr_a(error.message) );
    else if (p_dbus_message_get_args( reply, NULL, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID ))
    {
        /* Only 2 (portal failed) falls back. 1 = user cancelled; -1 = in flight, see PORTAL_RESPONSE_TIMEOUT_MS. */
        code = wait_response( conn, handle );
        TRACE( "%s -> response %d\n", debugstr_a(params->unix_path), code );
        if (code != 2) status = STATUS_SUCCESS;
    }
    if (reply) p_dbus_message_unref( reply );

    p_dbus_error_free( &error );
    p_dbus_connection_close( conn );
    p_dbus_connection_unref( conn );
    return status;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    reveal_in_host,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_reveal_in_host + 1 );

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    reveal_in_host,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_reveal_in_host + 1 );

#endif /* _WIN64 */
