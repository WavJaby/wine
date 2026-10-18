/*
 * Windows.UI.Notifications toast classes
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

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpnapps);

/* Legacy template XML, as documented for ToastNotificationManager.GetTemplateContent. */
static const WCHAR *template_xml[] =
{
    [ToastTemplateType_ToastImageAndText01] = L"<toast><visual><binding template=\"ToastImageAndText01\">"
        L"<image id=\"1\" src=\"\"/><text id=\"1\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastImageAndText02] = L"<toast><visual><binding template=\"ToastImageAndText02\">"
        L"<image id=\"1\" src=\"\"/><text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastImageAndText03] = L"<toast><visual><binding template=\"ToastImageAndText03\">"
        L"<image id=\"1\" src=\"\"/><text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastImageAndText04] = L"<toast><visual><binding template=\"ToastImageAndText04\">"
        L"<image id=\"1\" src=\"\"/><text id=\"1\"></text><text id=\"2\"></text><text id=\"3\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastText01] = L"<toast><visual><binding template=\"ToastText01\">"
        L"<text id=\"1\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastText02] = L"<toast><visual><binding template=\"ToastText02\">"
        L"<text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastText03] = L"<toast><visual><binding template=\"ToastText03\">"
        L"<text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    [ToastTemplateType_ToastText04] = L"<toast><visual><binding template=\"ToastText04\">"
        L"<text id=\"1\"></text><text id=\"2\"></text><text id=\"3\"></text></binding></visual></toast>",
};

/* Event handler registrations; LINE registers one handler per event. */
#define MAX_HANDLERS 8

struct handler_list
{
    struct { INT64 token; IUnknown *handler; } items[MAX_HANDLERS];
};

static CRITICAL_SECTION handlers_cs;
static CRITICAL_SECTION_DEBUG handlers_cs_debug =
{
    0, 0, &handlers_cs,
    { &handlers_cs_debug.ProcessLocksList, &handlers_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": handlers_cs") }
};
static CRITICAL_SECTION handlers_cs = { &handlers_cs_debug, -1, 0, 0, 0, 0 };
static INT64 next_token = 1;

static HRESULT handlers_add( struct handler_list *list, IUnknown *handler, EventRegistrationToken *token )
{
    unsigned int i;
    HRESULT hr = E_OUTOFMEMORY;

    if (!handler) return E_INVALIDARG;
    EnterCriticalSection( &handlers_cs );
    for (i = 0; i < MAX_HANDLERS; i++)
    {
        if (list->items[i].handler) continue;
        list->items[i].handler = handler;
        IUnknown_AddRef( list->items[i].handler );
        token->value = list->items[i].token = next_token++;
        hr = S_OK;
        break;
    }
    LeaveCriticalSection( &handlers_cs );
    return hr;
}

static HRESULT handlers_remove( struct handler_list *list, EventRegistrationToken token )
{
    IUnknown *handler = NULL;
    unsigned int i;

    EnterCriticalSection( &handlers_cs );
    for (i = 0; i < MAX_HANDLERS; i++)
    {
        if (!list->items[i].handler || list->items[i].token != token.value) continue;
        handler = list->items[i].handler;
        list->items[i].handler = NULL;
        break;
    }
    LeaveCriticalSection( &handlers_cs );
    if (handler) IUnknown_Release( handler );
    return S_OK;
}

/* Copies the handlers (referenced) so they can be invoked outside the lock. */
static unsigned int handlers_get( struct handler_list *list, IUnknown **out )
{
    unsigned int i, count = 0;

    EnterCriticalSection( &handlers_cs );
    for (i = 0; i < MAX_HANDLERS; i++)
        if (list->items[i].handler)
        {
            out[count] = list->items[i].handler;
            IUnknown_AddRef( out[count] );
            count++;
        }
    LeaveCriticalSection( &handlers_cs );
    return count;
}

static void handlers_clear( struct handler_list *list )
{
    unsigned int i;
    for (i = 0; i < MAX_HANDLERS; i++) if (list->items[i].handler) IUnknown_Release( list->items[i].handler );
}

/* ToastActivatedEventArgs / ToastDismissedEventArgs */

struct event_args
{
    IToastActivatedEventArgs IToastActivatedEventArgs_iface;
    IToastDismissedEventArgs IToastDismissedEventArgs_iface;
    LONG ref;
    HSTRING arguments;
    ToastDismissalReason reason;
};

static inline struct event_args *impl_from_IToastActivatedEventArgs( IToastActivatedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct event_args, IToastActivatedEventArgs_iface );
}

static HRESULT WINAPI activated_args_QueryInterface( IToastActivatedEventArgs *iface, REFIID iid, void **out )
{
    struct event_args *impl = impl_from_IToastActivatedEventArgs( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastActivatedEventArgs ))
        *out = &impl->IToastActivatedEventArgs_iface;
    else if (IsEqualGUID( iid, &IID_IToastDismissedEventArgs ))
        *out = &impl->IToastDismissedEventArgs_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI activated_args_AddRef( IToastActivatedEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_IToastActivatedEventArgs( iface )->ref );
}

static ULONG WINAPI activated_args_Release( IToastActivatedEventArgs *iface )
{
    struct event_args *impl = impl_from_IToastActivatedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->arguments );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI activated_args_GetIids( IToastActivatedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI activated_args_GetRuntimeClassName( IToastActivatedEventArgs *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI activated_args_GetTrustLevel( IToastActivatedEventArgs *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activated_args_get_Arguments( IToastActivatedEventArgs *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl_from_IToastActivatedEventArgs( iface )->arguments, value );
}

static const struct IToastActivatedEventArgsVtbl activated_args_vtbl =
{
    activated_args_QueryInterface,
    activated_args_AddRef,
    activated_args_Release,
    /* IInspectable methods */
    activated_args_GetIids,
    activated_args_GetRuntimeClassName,
    activated_args_GetTrustLevel,
    /* IToastActivatedEventArgs methods */
    activated_args_get_Arguments,
};

DEFINE_IINSPECTABLE( dismissed_args, IToastDismissedEventArgs, struct event_args, IToastActivatedEventArgs_iface )

static HRESULT WINAPI dismissed_args_get_Reason( IToastDismissedEventArgs *iface, ToastDismissalReason *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl_from_IToastDismissedEventArgs( iface )->reason;
    return S_OK;
}

static const struct IToastDismissedEventArgsVtbl dismissed_args_vtbl =
{
    dismissed_args_QueryInterface,
    dismissed_args_AddRef,
    dismissed_args_Release,
    /* IInspectable methods */
    dismissed_args_GetIids,
    dismissed_args_GetRuntimeClassName,
    dismissed_args_GetTrustLevel,
    /* IToastDismissedEventArgs methods */
    dismissed_args_get_Reason,
};

static struct event_args *event_args_create( HSTRING arguments, ToastDismissalReason reason )
{
    struct event_args *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IToastActivatedEventArgs_iface.lpVtbl = &activated_args_vtbl;
    impl->IToastDismissedEventArgs_iface.lpVtbl = &dismissed_args_vtbl;
    impl->ref = 1;
    impl->reason = reason;
    if (arguments) WindowsDuplicateString( arguments, &impl->arguments );
    return impl;
}

/* ToastNotification */

struct toast
{
    IToastNotification IToastNotification_iface;
    IToastNotification2 IToastNotification2_iface;
    LONG ref;
    IXmlDocument *content;
    HSTRING tag;
    HSTRING group;
    boolean suppress_popup;
    struct handler_list activated;
    struct handler_list dismissed;
    struct handler_list failed;
};

static inline struct toast *impl_from_IToastNotification( IToastNotification *iface )
{
    return CONTAINING_RECORD( iface, struct toast, IToastNotification_iface );
}

static HRESULT WINAPI toast_QueryInterface( IToastNotification *iface, REFIID iid, void **out )
{
    struct toast *impl = impl_from_IToastNotification( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastNotification ))
    {
        *out = &impl->IToastNotification_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IToastNotification2 ))
    {
        *out = &impl->IToastNotification2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI toast_AddRef( IToastNotification *iface )
{
    return InterlockedIncrement( &impl_from_IToastNotification( iface )->ref );
}

static ULONG WINAPI toast_Release( IToastNotification *iface )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IXmlDocument_Release( impl->content );
        WindowsDeleteString( impl->tag );
        WindowsDeleteString( impl->group );
        handlers_clear( &impl->activated );
        handlers_clear( &impl->dismissed );
        handlers_clear( &impl->failed );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI toast_GetIids( IToastNotification *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI toast_GetRuntimeClassName( IToastNotification *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Notifications_ToastNotification,
                                wcslen( RuntimeClass_Windows_UI_Notifications_ToastNotification ), class_name );
}

static HRESULT WINAPI toast_GetTrustLevel( IToastNotification *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI toast_get_Content( IToastNotification *iface, IXmlDocument **value )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->content;
    IXmlDocument_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI toast_put_ExpirationTime( IToastNotification *iface, IReference_DateTime *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI toast_get_ExpirationTime( IToastNotification *iface, IReference_DateTime **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI toast_add_Dismissed( IToastNotification *iface, ITypedEventHandler_ToastNotification_ToastDismissedEventArgs *handler,
                                           EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return handlers_add( &impl_from_IToastNotification( iface )->dismissed, (IUnknown *)handler, token );
}

static HRESULT WINAPI toast_remove_Dismissed( IToastNotification *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return handlers_remove( &impl_from_IToastNotification( iface )->dismissed, token );
}

static HRESULT WINAPI toast_add_Activated( IToastNotification *iface, ITypedEventHandler_ToastNotification_IInspectable *handler,
                                           EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return handlers_add( &impl_from_IToastNotification( iface )->activated, (IUnknown *)handler, token );
}

static HRESULT WINAPI toast_remove_Activated( IToastNotification *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return handlers_remove( &impl_from_IToastNotification( iface )->activated, token );
}

static HRESULT WINAPI toast_add_Failed( IToastNotification *iface, ITypedEventHandler_ToastNotification_ToastFailedEventArgs *handler,
                                           EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return handlers_add( &impl_from_IToastNotification( iface )->failed, (IUnknown *)handler, token );
}

static HRESULT WINAPI toast_remove_Failed( IToastNotification *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return handlers_remove( &impl_from_IToastNotification( iface )->failed, token );
}

static const struct IToastNotificationVtbl toast_vtbl =
{
    toast_QueryInterface,
    toast_AddRef,
    toast_Release,
    /* IInspectable methods */
    toast_GetIids,
    toast_GetRuntimeClassName,
    toast_GetTrustLevel,
    /* IToastNotification methods */
    toast_get_Content,
    toast_put_ExpirationTime,
    toast_get_ExpirationTime,
    toast_add_Dismissed,
    toast_remove_Dismissed,
    toast_add_Activated,
    toast_remove_Activated,
    toast_add_Failed,
    toast_remove_Failed,
};

void toast_fire_activated( IToastNotification *iface, HSTRING arguments )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    IUnknown *handlers[MAX_HANDLERS];
    unsigned int i, count = handlers_get( &impl->activated, handlers );
    struct event_args *args = event_args_create( arguments, 0 );

    TRACE( "toast %p, arguments %s, %u handlers.\n", iface, debugstr_hstring( arguments ), count );
    for (i = 0; i < count; i++)
    {
        if (args) ITypedEventHandler_ToastNotification_IInspectable_Invoke( (ITypedEventHandler_ToastNotification_IInspectable *)handlers[i],
                                                                             iface, (IInspectable *)&args->IToastActivatedEventArgs_iface );
        IUnknown_Release( handlers[i] );
    }
    if (args) IToastActivatedEventArgs_Release( &args->IToastActivatedEventArgs_iface );
}

void toast_fire_dismissed( IToastNotification *iface, ToastDismissalReason reason )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    IUnknown *handlers[MAX_HANDLERS];
    unsigned int i, count = handlers_get( &impl->dismissed, handlers );
    struct event_args *args = event_args_create( NULL, reason );

    TRACE( "toast %p, reason %d, %u handlers.\n", iface, reason, count );
    for (i = 0; i < count; i++)
    {
        if (args) ITypedEventHandler_ToastNotification_ToastDismissedEventArgs_Invoke(
                (ITypedEventHandler_ToastNotification_ToastDismissedEventArgs *)handlers[i], iface,
                &args->IToastDismissedEventArgs_iface );
        IUnknown_Release( handlers[i] );
    }
    if (args) IToastActivatedEventArgs_Release( &args->IToastActivatedEventArgs_iface );
}

DEFINE_IINSPECTABLE( toast2, IToastNotification2, struct toast, IToastNotification_iface )

static HRESULT replace_string( HSTRING *dst, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *dst );
    *dst = copy;
    return S_OK;
}

static HRESULT WINAPI toast2_put_Tag( IToastNotification2 *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return replace_string( &impl_from_IToastNotification2( iface )->tag, value );
}

static HRESULT WINAPI toast2_get_Tag( IToastNotification2 *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl_from_IToastNotification2( iface )->tag, value );
}

static HRESULT WINAPI toast2_put_Group( IToastNotification2 *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return replace_string( &impl_from_IToastNotification2( iface )->group, value );
}

static HRESULT WINAPI toast2_get_Group( IToastNotification2 *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl_from_IToastNotification2( iface )->group, value );
}

static HRESULT WINAPI toast2_put_SuppressPopup( IToastNotification2 *iface, boolean value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    impl_from_IToastNotification2( iface )->suppress_popup = value;
    return S_OK;
}

static HRESULT WINAPI toast2_get_SuppressPopup( IToastNotification2 *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl_from_IToastNotification2( iface )->suppress_popup;
    return S_OK;
}

static const struct IToastNotification2Vtbl toast2_vtbl =
{
    toast2_QueryInterface,
    toast2_AddRef,
    toast2_Release,
    /* IInspectable methods */
    toast2_GetIids,
    toast2_GetRuntimeClassName,
    toast2_GetTrustLevel,
    /* IToastNotification2 methods */
    toast2_put_Tag,
    toast2_get_Tag,
    toast2_put_Group,
    toast2_get_Group,
    toast2_put_SuppressPopup,
    toast2_get_SuppressPopup,
};

/* ToastNotifier */

struct notifier
{
    IToastNotifier IToastNotifier_iface;
    LONG ref;
    HSTRING app_id;
};

static inline struct notifier *impl_from_IToastNotifier( IToastNotifier *iface )
{
    return CONTAINING_RECORD( iface, struct notifier, IToastNotifier_iface );
}

static HRESULT WINAPI notifier_QueryInterface( IToastNotifier *iface, REFIID iid, void **out )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IToastNotifier ))
    {
        *out = &impl->IToastNotifier_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI notifier_AddRef( IToastNotifier *iface )
{
    return InterlockedIncrement( &impl_from_IToastNotifier( iface )->ref );
}

static ULONG WINAPI notifier_Release( IToastNotifier *iface )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->app_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI notifier_GetIids( IToastNotifier *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_GetRuntimeClassName( IToastNotifier *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_GetTrustLevel( IToastNotifier *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI notifier_Show( IToastNotifier *iface, IToastNotification *notification )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );
    IToastNotification2 *toast2;
    IXmlDocument *content;
    boolean suppress = FALSE;
    HSTRING xml = NULL;
    HRESULT hr;

    TRACE( "iface %p, notification %p.\n", iface, notification );

    if (SUCCEEDED(IToastNotification_QueryInterface( notification, &IID_IToastNotification2, (void **)&toast2 )))
    {
        IToastNotification2_get_SuppressPopup( toast2, &suppress );
        IToastNotification2_Release( toast2 );
    }
    if (suppress) return S_OK; /* would only land in the action center, which we do not have */

    if (FAILED(hr = IToastNotification_get_Content( notification, &content ))) return hr;
    hr = xml_document_get_xml( content, &xml );
    IXmlDocument_Release( content );
    if (FAILED(hr)) return hr;
    hr = notification_show( notification, xml, impl->app_id );
    WindowsDeleteString( xml );
    return hr;
}

static HRESULT WINAPI notifier_Hide( IToastNotifier *iface, IToastNotification *notification )
{
    TRACE( "iface %p, notification %p.\n", iface, notification );
    notification_hide( notification );
    return S_OK;
}

static HRESULT WINAPI notifier_get_Setting( IToastNotifier *iface, NotificationSetting *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = NotificationSetting_Enabled;
    return S_OK;
}

static HRESULT WINAPI notifier_AddToSchedule( IToastNotifier *iface, IScheduledToastNotification *toast )
{
    FIXME( "iface %p, toast %p stub!\n", iface, toast );
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_RemoveFromSchedule( IToastNotifier *iface, IScheduledToastNotification *toast )
{
    FIXME( "iface %p, toast %p stub!\n", iface, toast );
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_GetScheduledToastNotifications( IToastNotifier *iface, IVectorView_ScheduledToastNotification **result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static const struct IToastNotifierVtbl notifier_vtbl =
{
    notifier_QueryInterface,
    notifier_AddRef,
    notifier_Release,
    /* IInspectable methods */
    notifier_GetIids,
    notifier_GetRuntimeClassName,
    notifier_GetTrustLevel,
    /* IToastNotifier methods */
    notifier_Show,
    notifier_Hide,
    notifier_get_Setting,
    notifier_AddToSchedule,
    notifier_RemoveFromSchedule,
    notifier_GetScheduledToastNotifications,
};

static HRESULT notifier_create( HSTRING app_id, IToastNotifier **out )
{
    struct notifier *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IToastNotifier_iface.lpVtbl = &notifier_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( app_id, &impl->app_id )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IToastNotifier_iface;
    return S_OK;
}

/* ToastNotificationHistory: a process-wide singleton over the toasts currently shown. */

static IToastNotificationHistory2 history2;

static HRESULT WINAPI history_QueryInterface( IToastNotificationHistory *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastNotificationHistory ))
    {
        *out = iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IToastNotificationHistory2 ))
    {
        *out = &history2;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI history_AddRef( IToastNotificationHistory *iface ) { return 2; }
static ULONG WINAPI history_Release( IToastNotificationHistory *iface ) { return 1; }

static HRESULT WINAPI history_GetIids( IToastNotificationHistory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI history_GetRuntimeClassName( IToastNotificationHistory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI history_GetTrustLevel( IToastNotificationHistory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* Only this process's toasts are tracked, so the application id arguments need no filtering. */
static HRESULT WINAPI history_RemoveGroup( IToastNotificationHistory *iface, HSTRING group )
{
    TRACE( "group %s.\n", debugstr_hstring( group ) );
    notification_remove( NULL, group, TRUE, FALSE );
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroupWithId( IToastNotificationHistory *iface, HSTRING group, HSTRING app_id )
{
    TRACE( "group %s, app %s.\n", debugstr_hstring( group ), debugstr_hstring( app_id ) );
    notification_remove( NULL, group, TRUE, FALSE );
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroupedTagWithId( IToastNotificationHistory *iface, HSTRING tag, HSTRING group, HSTRING app_id )
{
    TRACE( "tag %s, group %s, app %s.\n", debugstr_hstring( tag ), debugstr_hstring( group ), debugstr_hstring( app_id ) );
    notification_remove( tag, group, FALSE, FALSE );
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroupedTag( IToastNotificationHistory *iface, HSTRING tag, HSTRING group )
{
    TRACE( "tag %s, group %s.\n", debugstr_hstring( tag ), debugstr_hstring( group ) );
    notification_remove( tag, group, FALSE, FALSE );
    return S_OK;
}

static HRESULT WINAPI history_Remove( IToastNotificationHistory *iface, HSTRING tag )
{
    TRACE( "tag %s.\n", debugstr_hstring( tag ) );
    notification_remove( tag, NULL, FALSE, FALSE ); /* ungrouped: the default (empty) group */
    return S_OK;
}

static HRESULT WINAPI history_Clear( IToastNotificationHistory *iface )
{
    TRACE( "\n" );
    notification_remove( NULL, NULL, TRUE, TRUE );
    return S_OK;
}

static HRESULT WINAPI history_ClearWithId( IToastNotificationHistory *iface, HSTRING app_id )
{
    TRACE( "app %s.\n", debugstr_hstring( app_id ) );
    notification_remove( NULL, NULL, TRUE, TRUE );
    return S_OK;
}

static const struct IToastNotificationHistoryVtbl history_vtbl =
{
    history_QueryInterface,
    history_AddRef,
    history_Release,
    /* IInspectable methods */
    history_GetIids,
    history_GetRuntimeClassName,
    history_GetTrustLevel,
    /* IToastNotificationHistory methods */
    history_RemoveGroup,
    history_RemoveGroupWithId,
    history_RemoveGroupedTagWithId,
    history_RemoveGroupedTag,
    history_Remove,
    history_Clear,
    history_ClearWithId,
};

static IToastNotificationHistory history = {&history_vtbl};

/* Read-only snapshot vector returned by GetHistory. */

struct toast_vector
{
    IVectorView_ToastNotification IVectorView_ToastNotification_iface;
    LONG ref;
    IToastNotification **toasts;
    UINT32 count;
};

static inline struct toast_vector *impl_from_IVectorView_ToastNotification( IVectorView_ToastNotification *iface )
{
    return CONTAINING_RECORD( iface, struct toast_vector, IVectorView_ToastNotification_iface );
}

static HRESULT WINAPI vector_QueryInterface( IVectorView_ToastNotification *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVectorView_ToastNotification ))
    {
        *out = iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI vector_AddRef( IVectorView_ToastNotification *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_ToastNotification( iface )->ref );
}

static ULONG WINAPI vector_Release( IVectorView_ToastNotification *iface )
{
    struct toast_vector *impl = impl_from_IVectorView_ToastNotification( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IToastNotification_Release( impl->toasts[i] );
        free( impl->toasts );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI vector_GetIids( IVectorView_ToastNotification *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI vector_GetRuntimeClassName( IVectorView_ToastNotification *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI vector_GetTrustLevel( IVectorView_ToastNotification *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI vector_GetAt( IVectorView_ToastNotification *iface, UINT32 index, IToastNotification **value )
{
    struct toast_vector *impl = impl_from_IVectorView_ToastNotification( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    *value = impl->toasts[index];
    IToastNotification_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI vector_get_Size( IVectorView_ToastNotification *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl_from_IVectorView_ToastNotification( iface )->count;
    return S_OK;
}

static HRESULT WINAPI vector_IndexOf( IVectorView_ToastNotification *iface, IToastNotification *element,
                                      UINT32 *index, boolean *found )
{
    struct toast_vector *impl = impl_from_IVectorView_ToastNotification( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );
    for (i = 0; i < impl->count; i++) if (impl->toasts[i] == element) break;
    *found = i < impl->count;
    *index = *found ? i : 0;
    return S_OK;
}

static HRESULT WINAPI vector_GetMany( IVectorView_ToastNotification *iface, UINT32 start, UINT32 size,
                                      IToastNotification **items, UINT32 *count )
{
    struct toast_vector *impl = impl_from_IVectorView_ToastNotification( iface );
    UINT32 i;

    TRACE( "iface %p, start %u, size %u, items %p, count %p.\n", iface, start, size, items, count );
    for (i = start; i < impl->count && i - start < size; i++)
        items[i - start] = impl->toasts[i];
        IToastNotification_AddRef( items[i - start] );
    *count = i > start ? i - start : 0;
    return S_OK;
}

static const struct IVectorView_ToastNotificationVtbl vector_vtbl =
{
    vector_QueryInterface,
    vector_AddRef,
    vector_Release,
    /* IInspectable methods */
    vector_GetIids,
    vector_GetRuntimeClassName,
    vector_GetTrustLevel,
    /* IVectorView<ToastNotification> methods */
    vector_GetAt,
    vector_get_Size,
    vector_IndexOf,
    vector_GetMany,
};

static HRESULT WINAPI history2_QueryInterface( IToastNotificationHistory2 *iface, REFIID iid, void **out )
{
    return history_QueryInterface( &history, iid, out );
}

static ULONG WINAPI history2_AddRef( IToastNotificationHistory2 *iface ) { return 2; }
static ULONG WINAPI history2_Release( IToastNotificationHistory2 *iface ) { return 1; }

static HRESULT WINAPI history2_GetIids( IToastNotificationHistory2 *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI history2_GetRuntimeClassName( IToastNotificationHistory2 *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI history2_GetTrustLevel( IToastNotificationHistory2 *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI history2_GetHistory( IToastNotificationHistory2 *iface, IVectorView_ToastNotification **result )
{
    struct toast_vector *impl;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = notification_snapshot( &impl->toasts, &impl->count )))
    {
        free( impl );
        return hr;
    }
    impl->IVectorView_ToastNotification_iface.lpVtbl = &vector_vtbl;
    impl->ref = 1;
    *result = &impl->IVectorView_ToastNotification_iface;
    return S_OK;
}

static HRESULT WINAPI history2_GetHistoryWithId( IToastNotificationHistory2 *iface, HSTRING app_id,
                                                 IVectorView_ToastNotification **result )
{
    TRACE( "iface %p, app_id %s, result %p.\n", iface, debugstr_hstring( app_id ), result );
    return history2_GetHistory( iface, result );
}

static const struct IToastNotificationHistory2Vtbl history2_vtbl =
{
    history2_QueryInterface,
    history2_AddRef,
    history2_Release,
    /* IInspectable methods */
    history2_GetIids,
    history2_GetRuntimeClassName,
    history2_GetTrustLevel,
    /* IToastNotificationHistory2 methods */
    history2_GetHistory,
    history2_GetHistoryWithId,
};

static IToastNotificationHistory2 history2 = {&history2_vtbl};

/* Activation factories: ToastNotificationManager (statics) and ToastNotification (IToastNotificationFactory). */

struct factory
{
    IActivationFactory IActivationFactory_iface;
    IToastNotificationManagerStatics IToastNotificationManagerStatics_iface;
    IToastNotificationManagerStatics2 IToastNotificationManagerStatics2_iface;
    IToastNotificationFactory IToastNotificationFactory_iface;
    const WCHAR *class_name;
    LONG ref;
};

static inline struct factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "class %s, iid %s.\n", debugstr_w( impl->class_name ), debugstr_guid( iid ) );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationManagerStatics ) && impl->IToastNotificationManagerStatics_iface.lpVtbl)
        *out = &impl->IToastNotificationManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationManagerStatics2 ) && impl->IToastNotificationManagerStatics2_iface.lpVtbl)
        *out = &impl->IToastNotificationManagerStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationFactory ) && impl->IToastNotificationFactory_iface.lpVtbl)
        *out = &impl->IToastNotificationFactory_iface;

    if (!*out)
    {
        FIXME( "class %s: %s not implemented, returning E_NOINTERFACE.\n", debugstr_w( impl->class_name ), debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( statics, IToastNotificationManagerStatics, struct factory, IActivationFactory_iface )

static HRESULT WINAPI statics_CreateToastNotifier( IToastNotificationManagerStatics *iface, IToastNotifier **result )
{
    FIXME( "iface %p, result %p: no application id, using process default.\n", iface, result );
    return notifier_create( NULL, result );
}

static HRESULT WINAPI statics_CreateToastNotifierWithId( IToastNotificationManagerStatics *iface, HSTRING id, IToastNotifier **result )
{
    TRACE( "iface %p, id %s, result %p.\n", iface, debugstr_hstring( id ), result );
    return notifier_create( id, result );
}

static HRESULT WINAPI statics_GetTemplateContent( IToastNotificationManagerStatics *iface, ToastTemplateType type, IXmlDocument **result )
{
    TRACE( "iface %p, type %d, result %p.\n", iface, type, result );
    if ((UINT)type >= ARRAY_SIZE(template_xml)) return E_INVALIDARG;
    return xml_document_create( template_xml[type], result );
}

static const struct IToastNotificationManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IToastNotificationManagerStatics methods */
    statics_CreateToastNotifier,
    statics_CreateToastNotifierWithId,
    statics_GetTemplateContent,
};

DEFINE_IINSPECTABLE( statics2, IToastNotificationManagerStatics2, struct factory, IActivationFactory_iface )

static HRESULT WINAPI statics2_get_History( IToastNotificationManagerStatics2 *iface, IToastNotificationHistory **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = &history;
    return S_OK;
}

static const struct IToastNotificationManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IToastNotificationManagerStatics2 methods */
    statics2_get_History,
};

DEFINE_IINSPECTABLE( toast_factory, IToastNotificationFactory, struct factory, IActivationFactory_iface )

static HRESULT WINAPI toast_factory_CreateToastNotification( IToastNotificationFactory *iface, IXmlDocument *content, IToastNotification **value )
{
    struct toast *impl;

    TRACE( "iface %p, content %p, value %p.\n", iface, content, value );

    if (!content) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IToastNotification_iface.lpVtbl = &toast_vtbl;
    impl->IToastNotification2_iface.lpVtbl = &toast2_vtbl;
    impl->ref = 1;
    impl->content = content;
    IXmlDocument_AddRef( impl->content );
    *value = &impl->IToastNotification_iface;
    return S_OK;
}

static const struct IToastNotificationFactoryVtbl toast_factory_vtbl =
{
    toast_factory_QueryInterface,
    toast_factory_AddRef,
    toast_factory_Release,
    /* IInspectable methods */
    toast_factory_GetIids,
    toast_factory_GetRuntimeClassName,
    toast_factory_GetTrustLevel,
    /* IToastNotificationFactory methods */
    toast_factory_CreateToastNotification,
};

static struct factory manager_factory =
{
    .IActivationFactory_iface = {&factory_vtbl},
    .IToastNotificationManagerStatics_iface = {&statics_vtbl},
    .IToastNotificationManagerStatics2_iface = {&statics2_vtbl},
    .class_name = RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
    .ref = 1,
};

static struct factory notification_factory =
{
    .IActivationFactory_iface = {&factory_vtbl},
    .IToastNotificationFactory_iface = {&toast_factory_vtbl},
    .class_name = RuntimeClass_Windows_UI_Notifications_ToastNotification,
    .ref = 1,
};

IActivationFactory *toast_manager_factory = &manager_factory.IActivationFactory_iface;
IActivationFactory *toast_notification_factory = &notification_factory.IActivationFactory_iface;
