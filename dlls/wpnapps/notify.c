/*
 * Toast -> org.freedesktop.Notifications bridge
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

#include "private.h"
#include "unixlib.h"

#include "roapi.h"
#include "winreg.h"
#include "notificationactivationcallback.h"

#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpnapps);

/* A toast currently shown by the notification server. */
struct shown
{
    struct list entry;
    UINT32 id;
    IToastNotification *toast;
    WCHAR *tag;
    WCHAR *group;
    WCHAR *launch;
    WCHAR *app_id;  /* AppUserModelID of the notifier, keys the app's toast activator */
    BOOL activated; /* Windows raises Activated, not Dismissed, for a clicked toast */
};

static struct list shown_list = LIST_INIT( shown_list );
static CRITICAL_SECTION shown_cs;
static CRITICAL_SECTION_DEBUG shown_cs_debug =
{
    0, 0, &shown_cs,
    { &shown_cs_debug.ProcessLocksList, &shown_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": shown_cs") }
};
static CRITICAL_SECTION shown_cs = { &shown_cs_debug, -1, 0, 0, 0, 0 };

static BOOL body_markup;
static BOOL bridge_ready;
/* Windows enters a toast into the history asynchronously after Show, so a GetHistory issued right after
 * Show does not list it yet. LINE relies on that: after each Show it removes what GetHistory returns, to
 * keep one toast per chat. Listing the new toast made LINE close it immediately. */
static IToastNotification *newest_toast;

static void free_shown( struct shown *shown )
{
    IToastNotification_Release( shown->toast );
    free( shown->tag );
    free( shown->group );
    free( shown->launch );
    free( shown->app_id );
    free( shown );
}

/* ---- minimal XML reading: LINE (and the documented templates) emit flat, generated XML ---- */

static WCHAR *unescape( const WCHAR *start, const WCHAR *end )
{
    WCHAR *out = malloc( (end - start + 1) * sizeof(WCHAR) ), *dst = out;
    const WCHAR *src = start;

    if (!out) return NULL;
    while (src < end)
    {
        if (*src != '&') { *dst++ = *src++; continue; }
        if (!wcsncmp( src, L"&lt;", 4 )) { *dst++ = '<'; src += 4; }
        else if (!wcsncmp( src, L"&gt;", 4 )) { *dst++ = '>'; src += 4; }
        else if (!wcsncmp( src, L"&amp;", 5 )) { *dst++ = '&'; src += 5; }
        else if (!wcsncmp( src, L"&quot;", 6 )) { *dst++ = '"'; src += 6; }
        else if (!wcsncmp( src, L"&apos;", 6 )) { *dst++ = '\''; src += 6; }
        else if (src[1] == '#')
        {
            WCHAR *num_end;
            unsigned long code = src[2] == 'x' ? wcstoul( src + 3, &num_end, 16 ) : wcstoul( src + 2, &num_end, 10 );
            if (*num_end != ';' || num_end >= end) { *dst++ = *src++; continue; }
            if (code > 0xffff)
            {
                code -= 0x10000;
                *dst++ = 0xd800 | (code >> 10);
                *dst++ = 0xdc00 | (code & 0x3ff);
            }
            else *dst++ = code;
            src = num_end + 1;
        }
        else *dst++ = *src++;
    }
    *dst = 0;
    return out;
}

/* Value of attribute `name` inside the start tag beginning at `tag` (which points at '<'). */
static WCHAR *get_attribute( const WCHAR *tag, const WCHAR *name )
{
    const WCHAR *tag_end = wcschr( tag, '>' ), *p = tag;
    size_t len = wcslen( name );

    if (!tag_end) return NULL;
    while ((p = wcsstr( p, name )) && p < tag_end)
    {
        const WCHAR *q = p + len;
        if (iswspace( p[-1] ) && q[0] == '=' && (q[1] == '"' || q[1] == '\''))
        {
            const WCHAR *value_end = wcschr( q + 2, q[1] );
            if (!value_end || value_end > tag_end) return NULL;
            return unescape( q + 2, value_end );
        }
        p = q;
    }
    return NULL;
}

struct toast_content
{
    WCHAR *summary;
    WCHAR *body;
    WCHAR *image;
    WCHAR *launch;
};

static void free_content( struct toast_content *content )
{
    free( content->summary );
    free( content->body );
    free( content->image );
    free( content->launch );
}

/* First <text> -> summary, the rest joined by newlines -> body (the ToastGeneric layout). Prefer the
 * appLogoOverride image (sender avatar) for the icon. */
static void parse_toast( const WCHAR *xml, struct toast_content *content )
{
    const WCHAR *p;
    size_t body_len = 0;

    memset( content, 0, sizeof(*content) );
    if ((p = wcsstr( xml, L"<toast" ))) content->launch = get_attribute( p, L"launch" );

    for (p = xml; (p = wcsstr( p, L"<text" ));)
    {
        const WCHAR *start = wcschr( p, '>' ), *end;
        WCHAR *text, *joined;

        if (!start) break;
        if (start[-1] == '/') { p = start; continue; } /* <text/> */
        start++;
        if (!(end = wcsstr( start, L"</text>" ))) break;
        p = end;
        if (!(text = unescape( start, end ))) continue;
        if (!content->summary) { content->summary = text; continue; }
        if ((joined = realloc( content->body, (body_len + wcslen( text ) + 2) * sizeof(WCHAR) )))
        {
            if (body_len) joined[body_len++] = '\n';
            wcscpy( joined + body_len, text );
            body_len += wcslen( text );
            content->body = joined;
        }
        free( text );
    }

    for (p = xml; (p = wcsstr( p, L"<image" )); p++)
    {
        WCHAR *placement = get_attribute( p, L"placement" );
        BOOL logo = placement && !wcscmp( placement, L"appLogoOverride" );

        free( placement );
        if (content->image && !logo) continue;
        free( content->image );
        content->image = get_attribute( p, L"src" );
        if (logo) break;
    }
}

/* file:/// URI or DOS path -> Unix path (UTF-8); empty for remote or package (ms-appx) images. */
static char *image_unix_path( const WCHAR *src )
{
    WCHAR *dos, *dst;
    const WCHAR *s;
    char *unix_path, *ret;

    if (!src || !src[0]) return NULL;
    if (!wcsnicmp( src, L"file:///", 8 )) src += 8;
    else if (wcsstr( src, L"://" ) || !wcsnicmp( src, L"ms-app", 6 )) return NULL;
    if (!(dos = malloc( (wcslen( src ) + 1) * sizeof(WCHAR) ))) return NULL;
    for (s = src, dst = dos; *s; s++)
    {
        if (s[0] == '%' && iswxdigit( s[1] ) && iswxdigit( s[2] ))
        {
            WCHAR hex[3] = {s[1], s[2], 0};
            *dst++ = wcstoul( hex, NULL, 16 );
            s += 2;
        }
        else *dst++ = *s == '/' ? '\\' : *s;
    }
    *dst = 0;
    unix_path = wine_get_unix_file_name( dos );
    free( dos );
    if (!unix_path) return NULL;
    ret = strdup( unix_path );
    HeapFree( GetProcessHeap(), 0, unix_path );
    return ret;
}

static char *to_utf8( const WCHAR *str, BOOL escape_markup )
{
    WCHAR *escaped = NULL;
    const WCHAR *s;
    char *ret;
    int len;

    if (!str) str = L"";
    if (escape_markup && wcspbrk( str, L"&<>" ))
    {
        WCHAR *dst;
        if (!(escaped = malloc( (wcslen( str ) * 5 + 1) * sizeof(WCHAR) ))) return NULL;
        for (s = str, dst = escaped; *s; s++)
        {
            if (*s == '&') { wcscpy( dst, L"&amp;" ); dst += 5; }
            else if (*s == '<') { wcscpy( dst, L"&lt;" ); dst += 4; }
            else if (*s == '>') { wcscpy( dst, L"&gt;" ); dst += 4; }
            else *dst++ = *s;
        }
        *dst = 0;
        str = escaped;
    }
    len = WideCharToMultiByte( CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL );
    if ((ret = malloc( len ))) WideCharToMultiByte( CP_UTF8, 0, str, -1, ret, len, NULL, NULL );
    free( escaped );
    return ret;
}

static WCHAR *hstring_dup( HSTRING str )
{
    UINT32 len;
    const WCHAR *buf = WindowsGetStringRawBuffer( str, &len );
    WCHAR *ret;

    if (!str || !len) return NULL;
    if ((ret = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        memcpy( ret, buf, len * sizeof(WCHAR) );
        ret[len] = 0;
    }
    return ret;
}

static BOOL str_equal( const WCHAR *a, const WCHAR *b )
{
    return !wcscmp( a ? a : L"", b ? b : L"" );
}

/* Notification server app name: the part of the AppUserModelID after '!' (package app id, e.g. "LINE"),
 * which is also what the desktop's notification daemon maps to a window class. */
static const WCHAR *app_display_name( const WCHAR *app_id )
{
    const WCHAR *bang;

    if (!app_id || !app_id[0]) return L"";
    return (bang = wcsrchr( app_id, '!' )) ? bang + 1 : app_id;
}

/* Windows hands a desktop app's toast click to the COM class its AUMID registers, from the shortcut property
 * System.AppUserModel.ToastActivatorCLSID or HKCU\Software\Classes\AppUserModelId\<AUMID> CustomActivator.
 * Such apps may ignore the in-process Activated event entirely (LINE does). Only the registry source is read:
 * Wine's shell link does not store shortcut properties. */
static BOOL get_toast_activator( const WCHAR *app_id, CLSID *clsid )
{
    WCHAR key[512], value[40];
    DWORD size = sizeof(value);

    if (!app_id || !app_id[0]) return FALSE;
    swprintf( key, ARRAY_SIZE(key), L"Software\\Classes\\AppUserModelId\\%s", app_id );
    if (RegGetValueW( HKEY_CURRENT_USER, key, L"CustomActivator", RRF_RT_REG_SZ, NULL, value, &size )) return FALSE;
    return SUCCEEDED(CLSIDFromString( value, clsid ));
}

/* Called on the MTA event thread; the activator is a local server, usually in the app's own STA. */
static void invoke_toast_activator( const WCHAR *app_id, const WCHAR *launch )
{
    INotificationActivationCallback *callback;
    CLSID clsid;
    HRESULT hr;

    if (!get_toast_activator( app_id, &clsid )) return;
    hr = CoCreateInstance( &clsid, NULL, CLSCTX_LOCAL_SERVER, &IID_INotificationActivationCallback, (void **)&callback );
    if (FAILED(hr))
    {
        WARN( "activator %s for %s: %#lx\n", debugstr_guid( &clsid ), debugstr_w( app_id ), hr );
        return;
    }
    hr = INotificationActivationCallback_Activate( callback, app_id, launch ? launch : L"", NULL, 0 );
    TRACE( "activator %s, args %s -> %#lx\n", debugstr_guid( &clsid ), debugstr_w( launch ), hr );
    INotificationActivationCallback_Release( callback );
}

/* ---- event thread: server signals -> ToastNotification events ---- */

static DWORD WINAPI event_thread( void *arg )
{
    RoInitialize( RO_INIT_MULTITHREADED );
    for (;;)
    {
        struct wait_event_params params = {0};
        IToastNotification *toast = NULL;
        HSTRING launch = NULL;
        WCHAR *app_id = NULL;
        BOOL dismissed = FALSE;
        struct shown *shown;

        if (WINE_UNIX_CALL( unix_wait_event, &params ) || params.type == NOTIFICATION_EVENT_SHUTDOWN) break;

        EnterCriticalSection( &shown_cs );
        LIST_FOR_EACH_ENTRY( shown, &shown_list, struct shown, entry )
        {
            if (shown->id != params.id) continue;
            if (params.type == NOTIFICATION_EVENT_ACTIVATED)
            {
                shown->activated = TRUE;
                toast = shown->toast;
                IToastNotification_AddRef( toast );
                if (shown->launch) WindowsCreateString( shown->launch, wcslen( shown->launch ), &launch );
                if (shown->app_id) app_id = wcsdup( shown->app_id );
            }
            else
            {
                if (!shown->activated)
                {
                    toast = shown->toast;
                    IToastNotification_AddRef( toast );
                    dismissed = TRUE;
                }
                list_remove( &shown->entry );
                free_shown( shown );
            }
            break;
        }
        LeaveCriticalSection( &shown_cs );

        if (!toast) continue;
        TRACE( "id %u type %u reason %u\n", params.id, params.type, params.reason );
        if (params.type == NOTIFICATION_EVENT_ACTIVATED)
        {
            toast_fire_activated( toast, launch );
            invoke_toast_activator( app_id, WindowsGetStringRawBuffer( launch, NULL ) );
        }
        else if (dismissed)
        {
            /* freedesktop reasons: 1 expired, 2 dismissed by the user, 3 closed by CloseNotification. */
            ToastDismissalReason reason = params.reason == 1 ? ToastDismissalReason_TimedOut :
                                          params.reason == 2 ? ToastDismissalReason_UserCanceled :
                                          ToastDismissalReason_ApplicationHidden;
            toast_fire_dismissed( toast, reason );
        }
        WindowsDeleteString( launch );
        free( app_id );
        IToastNotification_Release( toast );
    }
    WARN( "notification event thread exiting\n" );
    return 0;
}

static BOOL WINAPI init_bridge( INIT_ONCE *once, void *param, void **context )
{
    struct init_params params = {0};
    HANDLE thread;

    if (__wine_init_unix_call() || WINE_UNIX_CALL( unix_init, &params ))
    {
        WARN( "no notification server; toasts are dropped\n" );
        return TRUE;
    }
    body_markup = params.body_markup;
    if ((thread = CreateThread( NULL, 0, event_thread, NULL, 0, NULL ))) CloseHandle( thread );
    bridge_ready = TRUE;
    return TRUE;
}

static BOOL ensure_bridge(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, init_bridge, NULL, NULL );
    return bridge_ready;
}

/* ---- API for toast.c ---- */

HRESULT notification_show( IToastNotification *toast, HSTRING xml, HSTRING app_id )
{
    struct notify_params params = {0};
    struct toast_content content;
    struct shown *shown, *old;
    IToastNotification2 *toast2;
    HSTRING tag = NULL, group = NULL;
    char *summary, *body, *icon, *app;
    NTSTATUS status;

    if (!ensure_bridge()) return S_OK;
    if (!(shown = calloc( 1, sizeof(*shown) ))) return E_OUTOFMEMORY;
    if (SUCCEEDED(IToastNotification_QueryInterface( toast, &IID_IToastNotification2, (void **)&toast2 )))
    {
        IToastNotification2_get_Tag( toast2, &tag );
        IToastNotification2_get_Group( toast2, &group );
        IToastNotification2_Release( toast2 );
    }
    shown->tag = hstring_dup( tag );
    shown->group = hstring_dup( group );
    shown->app_id = hstring_dup( app_id );
    WindowsDeleteString( tag );
    WindowsDeleteString( group );

    parse_toast( WindowsGetStringRawBuffer( xml, NULL ), &content );
    shown->launch = content.launch;
    content.launch = NULL;

    /* Windows replaces a shown toast with the same tag and group. */
    EnterCriticalSection( &shown_cs );
    if (shown->tag)
        LIST_FOR_EACH_ENTRY( old, &shown_list, struct shown, entry )
            if (str_equal( old->tag, shown->tag ) && str_equal( old->group, shown->group ))
            {
                params.replaces_id = old->id;
                list_remove( &old->entry );
                free_shown( old );
                break;
            }
    LeaveCriticalSection( &shown_cs );

    app = to_utf8( app_display_name( shown->app_id ), FALSE );
    summary = to_utf8( content.summary, FALSE );
    body = to_utf8( content.body, body_markup );
    icon = image_unix_path( content.image );
    TRACE( "image %s, launch %s\n", debugstr_w( content.image ), debugstr_w( shown->launch ) );
    params.app_name = app ? app : "";
    params.summary = summary ? summary : "";
    params.body = body ? body : "";
    params.icon = icon ? icon : "";
    status = WINE_UNIX_CALL( unix_notify, &params );
    TRACE( "summary %s, icon %s -> id %u, status %#lx\n", debugstr_a( params.summary ), debugstr_a( params.icon ),
           params.id, status );
    free( app );
    free( summary );
    free( body );
    free( icon );
    free_content( &content );

    if (status)
    {
        free( shown->tag );
        free( shown->group );
        free( shown->launch );
        free( shown->app_id );
        free( shown );
        return S_OK; /* a missing server is not the application's failure */
    }
    shown->id = params.id;
    shown->toast = toast;
    IToastNotification_AddRef( shown->toast );
    EnterCriticalSection( &shown_cs );
    list_add_tail( &shown_list, &shown->entry );
    newest_toast = toast;
    LeaveCriticalSection( &shown_cs );
    return S_OK;
}

static void close_matching( BOOL (*match)( struct shown *, const void * ), const void *arg )
{
    struct shown *shown, *next;
    struct list closing = LIST_INIT( closing );

    EnterCriticalSection( &shown_cs );
    LIST_FOR_EACH_ENTRY_SAFE( shown, next, &shown_list, struct shown, entry )
    {
        if (!match( shown, arg )) continue;
        list_remove( &shown->entry );
        list_add_tail( &closing, &shown->entry );
    }
    LeaveCriticalSection( &shown_cs );

    /* Entries leave the list first, so the resulting NotificationClosed raises no Dismissed: Windows does
     * not raise it for history removal either. */
    LIST_FOR_EACH_ENTRY_SAFE( shown, next, &closing, struct shown, entry )
    {
        struct close_params params = {shown->id};
        WINE_UNIX_CALL( unix_close, &params );
        list_remove( &shown->entry );
        free_shown( shown );
    }
}

struct tag_group { const WCHAR *tag; const WCHAR *group; };

static BOOL match_tag_group( struct shown *shown, const void *arg )
{
    const struct tag_group *filter = arg;
    if (filter->tag && !str_equal( shown->tag, filter->tag )) return FALSE;
    if (filter->group && !str_equal( shown->group, filter->group )) return FALSE;
    return TRUE;
}

static BOOL match_toast( struct shown *shown, const void *arg )
{
    return shown->toast == arg;
}

/* NULL tag or group matches any. */
void notification_remove( HSTRING tag, HSTRING group, BOOL any_tag, BOOL any_group )
{
    WCHAR *tag_str = hstring_dup( tag ), *group_str = hstring_dup( group );
    struct tag_group filter = {any_tag ? NULL : (tag_str ? tag_str : L""), any_group ? NULL : (group_str ? group_str : L"")};

    if (bridge_ready) close_matching( match_tag_group, &filter );
    free( tag_str );
    free( group_str );
}

void notification_hide( IToastNotification *toast )
{
    if (bridge_ready) close_matching( match_toast, toast );
}

/* Snapshot of the toasts still on screen, for ToastNotificationHistory.GetHistory. */
HRESULT notification_snapshot( IToastNotification ***toasts, UINT32 *count )
{
    struct shown *shown;
    UINT32 n = 0;

    EnterCriticalSection( &shown_cs );
    *toasts = calloc( list_count( &shown_list ) + 1, sizeof(**toasts) );
    if (*toasts)
        LIST_FOR_EACH_ENTRY( shown, &shown_list, struct shown, entry )
        {
            if (shown->toast == newest_toast) continue;
            (*toasts)[n] = shown->toast;
            IToastNotification_AddRef( (*toasts)[n] );
            n++;
        }
    LeaveCriticalSection( &shown_cs );
    *count = n;
    return *toasts ? S_OK : E_OUTOFMEMORY;
}
