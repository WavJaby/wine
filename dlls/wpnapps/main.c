/*
 * wpnapps.dll: Windows.UI.Notifications toast runtime classes
 *
 * Copyright 2026 Vibhav Pant for CodeWeavers
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

#include "initguid.h"
#include "private.h"
#include "notificationactivationcallback.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpnapps);

HRESULT WINAPI DllGetClassObject( const CLSID *clsid, const IID *riid, void **out )
{
    FIXME( "(%s, %s, %p): stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( classid, NULL );
    IActivationFactory *impl = NULL;

    TRACE( "class %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (!wcscmp( buffer, RuntimeClass_Windows_UI_Notifications_ToastNotificationManager )) impl = toast_manager_factory;
    else if (!wcscmp( buffer, RuntimeClass_Windows_UI_Notifications_ToastNotification )) impl = toast_notification_factory;
    else if (!wcscmp( buffer, RuntimeClass_Windows_Data_Xml_Dom_XmlDocument )) impl = xml_document_factory;

    if (!impl)
    {
        FIXME( "class %s not implemented\n", debugstr_hstring( classid ) );
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return IActivationFactory_QueryInterface( impl, &IID_IActivationFactory, (void **)factory );
}
