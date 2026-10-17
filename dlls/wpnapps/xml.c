/*
 * Windows.Data.Xml.Dom.XmlDocument
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

/* Trace build: only load/serialize are real; every other DOM slot logs its interface and vtable index so a
 * run shows exactly which methods the application needs. x64-only shortcut: stubs ignore their arguments. */
#define STUB( p, i ) static HRESULT WINAPI p##_stub_##i( void *this ) { FIXME( #p " slot %d not implemented\n", i ); return E_NOTIMPL; }
STUB( doc, 6 )
STUB( doc, 7 )
STUB( doc, 8 )
STUB( doc, 9 )
STUB( doc, 10 )
STUB( doc, 11 )
STUB( doc, 12 )
STUB( doc, 13 )
STUB( doc, 14 )
STUB( doc, 15 )
STUB( doc, 16 )
STUB( doc, 17 )
STUB( doc, 18 )
STUB( doc, 19 )
STUB( doc, 20 )
STUB( doc, 21 )
STUB( doc, 22 )
STUB( doc, 23 )
STUB( doc, 24 )
STUB( doc, 25 )
STUB( doc, 26 )
STUB( doc, 27 )
STUB( doc, 28 )
STUB( node, 6 )
STUB( node, 7 )
STUB( node, 8 )
STUB( node, 9 )
STUB( node, 10 )
STUB( node, 11 )
STUB( node, 12 )
STUB( node, 13 )
STUB( node, 14 )
STUB( node, 15 )
STUB( node, 16 )
STUB( node, 17 )
STUB( node, 18 )
STUB( node, 19 )
STUB( node, 20 )
STUB( node, 21 )
STUB( node, 22 )
STUB( node, 23 )
STUB( node, 24 )
STUB( node, 25 )
STUB( node, 26 )
STUB( node, 27 )
STUB( node, 28 )
STUB( selector, 6 )
STUB( selector, 7 )
STUB( selector, 8 )
STUB( selector, 9 )
STUB( selector, 10 )
STUB( selector, 11 )
STUB( selector, 12 )
STUB( selector, 13 )
STUB( selector, 14 )
STUB( selector, 15 )
STUB( selector, 16 )
STUB( selector, 17 )
STUB( selector, 18 )
STUB( selector, 19 )
STUB( selector, 20 )
STUB( selector, 21 )
STUB( selector, 22 )
STUB( selector, 23 )
STUB( selector, 24 )
STUB( selector, 25 )
STUB( selector, 26 )
STUB( selector, 27 )
STUB( selector, 28 )
STUB( serializer, 6 )
STUB( serializer, 7 )
STUB( serializer, 8 )
STUB( serializer, 9 )
STUB( serializer, 10 )
STUB( serializer, 11 )
STUB( serializer, 12 )
STUB( serializer, 13 )
STUB( serializer, 14 )
STUB( serializer, 15 )
STUB( serializer, 16 )
STUB( serializer, 17 )
STUB( serializer, 18 )
STUB( serializer, 19 )
STUB( serializer, 20 )
STUB( serializer, 21 )
STUB( serializer, 22 )
STUB( serializer, 23 )
STUB( serializer, 24 )
STUB( serializer, 25 )
STUB( serializer, 26 )
STUB( serializer, 27 )
STUB( serializer, 28 )
STUB( io, 6 )
STUB( io, 7 )
STUB( io, 8 )
STUB( io, 9 )
STUB( io, 10 )
STUB( io, 11 )
STUB( io, 12 )
STUB( io, 13 )
STUB( io, 14 )
STUB( io, 15 )
STUB( io, 16 )
STUB( io, 17 )
STUB( io, 18 )
STUB( io, 19 )
STUB( io, 20 )
STUB( io, 21 )
STUB( io, 22 )
STUB( io, 23 )
STUB( io, 24 )
STUB( io, 25 )
STUB( io, 26 )
STUB( io, 27 )
STUB( io, 28 )

#define STUB_TABLE( p ) { \
    p##_stub_6, p##_stub_7, p##_stub_8, p##_stub_9, p##_stub_10, p##_stub_11, p##_stub_12, p##_stub_13, \
    p##_stub_14, p##_stub_15, p##_stub_16, p##_stub_17, p##_stub_18, p##_stub_19, p##_stub_20, p##_stub_21, \
    p##_stub_22, p##_stub_23, p##_stub_24, p##_stub_25, p##_stub_26, p##_stub_27, p##_stub_28 }

struct xml_document
{
    IXmlDocument IXmlDocument_iface;
    IXmlNode IXmlNode_iface;
    IXmlNodeSelector IXmlNodeSelector_iface;
    IXmlNodeSerializer IXmlNodeSerializer_iface;
    IXmlDocumentIO IXmlDocumentIO_iface;
    LONG ref;
    HSTRING xml;
};

static inline struct xml_document *impl_from_IXmlDocument( IXmlDocument *iface )
{
    return CONTAINING_RECORD( iface, struct xml_document, IXmlDocument_iface );
}

static HRESULT WINAPI doc_QueryInterface( IXmlDocument *iface, REFIID iid, void **out )
{
    struct xml_document *impl = impl_from_IXmlDocument( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IXmlDocument ))
        *out = &impl->IXmlDocument_iface;
    else if (IsEqualGUID( iid, &IID_IXmlNode )) *out = &impl->IXmlNode_iface;
    else if (IsEqualGUID( iid, &IID_IXmlNodeSelector )) *out = &impl->IXmlNodeSelector_iface;
    else if (IsEqualGUID( iid, &IID_IXmlNodeSerializer )) *out = &impl->IXmlNodeSerializer_iface;
    else if (IsEqualGUID( iid, &IID_IXmlDocumentIO )) *out = &impl->IXmlDocumentIO_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI doc_AddRef( IXmlDocument *iface )
{
    struct xml_document *impl = impl_from_IXmlDocument( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI doc_Release( IXmlDocument *iface )
{
    struct xml_document *impl = impl_from_IXmlDocument( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->xml );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI doc_GetIids( IXmlDocument *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI doc_GetRuntimeClassName( IXmlDocument *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Data_Xml_Dom_XmlDocument,
                                wcslen( RuntimeClass_Windows_Data_Xml_Dom_XmlDocument ), class_name );
}

static HRESULT WINAPI doc_GetTrustLevel( IXmlDocument *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

#define DEFINE_FORWARD( pfx, iface_type )                                                          \
    static inline struct xml_document *impl_from_##iface_type( iface_type *iface )                 \
    {                                                                                              \
        return CONTAINING_RECORD( iface, struct xml_document, iface_type##_iface );                \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        return doc_QueryInterface( &impl_from_##iface_type( iface )->IXmlDocument_iface, iid, out ); \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        return doc_AddRef( &impl_from_##iface_type( iface )->IXmlDocument_iface );                 \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        return doc_Release( &impl_from_##iface_type( iface )->IXmlDocument_iface );                \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *count, IID **iids )             \
    {                                                                                              \
        return doc_GetIids( &impl_from_##iface_type( iface )->IXmlDocument_iface, count, iids );   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *name )           \
    {                                                                                              \
        return doc_GetRuntimeClassName( &impl_from_##iface_type( iface )->IXmlDocument_iface, name ); \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *level )              \
    {                                                                                              \
        return doc_GetTrustLevel( &impl_from_##iface_type( iface )->IXmlDocument_iface, level );   \
    }

DEFINE_FORWARD( node, IXmlNode )
DEFINE_FORWARD( selector, IXmlNodeSelector )
DEFINE_FORWARD( serializer, IXmlNodeSerializer )
DEFINE_FORWARD( io, IXmlDocumentIO )

static HRESULT WINAPI serializer_GetXml( IXmlNodeSerializer *iface, HSTRING *xml )
{
    struct xml_document *impl = impl_from_IXmlNodeSerializer( iface );
    TRACE( "iface %p, xml %p.\n", iface, xml );
    return WindowsDuplicateString( impl->xml, xml );
}

static HRESULT WINAPI io_LoadXml( IXmlDocumentIO *iface, HSTRING xml )
{
    struct xml_document *impl = impl_from_IXmlDocumentIO( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, xml %s.\n", iface, debugstr_hstring( xml ) );
    if (FAILED(hr = WindowsDuplicateString( xml, &copy ))) return hr;
    WindowsDeleteString( impl->xml );
    impl->xml = copy;
    return S_OK;
}

static HRESULT WINAPI io_LoadXmlWithSettings( IXmlDocumentIO *iface, HSTRING xml, IXmlLoadSettings *settings )
{
    TRACE( "iface %p, xml %s, settings %p.\n", iface, debugstr_hstring( xml ), settings );
    return io_LoadXml( iface, xml );
}

#define IINSPECTABLE_SLOTS( pfx ) pfx##_QueryInterface, pfx##_AddRef, pfx##_Release, pfx##_GetIids, \
    pfx##_GetRuntimeClassName, pfx##_GetTrustLevel

static void *doc_slots[] = { IINSPECTABLE_SLOTS( doc ) };
static void *node_slots[] = { IINSPECTABLE_SLOTS( node ) };
static void *selector_slots[] = { IINSPECTABLE_SLOTS( selector ) };
static void *serializer_slots[] = { IINSPECTABLE_SLOTS( serializer ) };
static void *io_slots[] = { IINSPECTABLE_SLOTS( io ) };
static void *doc_stubs[] = STUB_TABLE( doc );
static void *node_stubs[] = STUB_TABLE( node );
static void *selector_stubs[] = STUB_TABLE( selector );
static void *serializer_stubs[] = STUB_TABLE( serializer );
static void *io_stubs[] = STUB_TABLE( io );

static void *doc_vtbl[29], *node_vtbl[29], *selector_vtbl[29], *serializer_vtbl[29], *io_vtbl[29];

static void build_vtbl( void **vtbl, void **inspectable, void **stubs )
{
    memcpy( vtbl, inspectable, 6 * sizeof(void *) );
    memcpy( vtbl + 6, stubs, 23 * sizeof(void *) );
}

static INIT_ONCE vtbl_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_vtbls( INIT_ONCE *once, void *param, void **context )
{
    build_vtbl( doc_vtbl, doc_slots, doc_stubs );
    build_vtbl( node_vtbl, node_slots, node_stubs );
    build_vtbl( selector_vtbl, selector_slots, selector_stubs );
    build_vtbl( serializer_vtbl, serializer_slots, serializer_stubs );
    build_vtbl( io_vtbl, io_slots, io_stubs );
    serializer_vtbl[6] = serializer_GetXml;
    io_vtbl[6] = io_LoadXml;
    io_vtbl[7] = io_LoadXmlWithSettings;
    return TRUE;
}

HRESULT xml_document_create( const WCHAR *xml, IXmlDocument **out )
{
    struct xml_document *impl;
    HRESULT hr;

    InitOnceExecuteOnce( &vtbl_once, init_vtbls, NULL, NULL );
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IXmlDocument_iface.lpVtbl = (void *)doc_vtbl;
    impl->IXmlNode_iface.lpVtbl = (void *)node_vtbl;
    impl->IXmlNodeSelector_iface.lpVtbl = (void *)selector_vtbl;
    impl->IXmlNodeSerializer_iface.lpVtbl = (void *)serializer_vtbl;
    impl->IXmlDocumentIO_iface.lpVtbl = (void *)io_vtbl;
    impl->ref = 1;
    if (xml && FAILED(hr = WindowsCreateString( xml, wcslen( xml ), &impl->xml )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IXmlDocument_iface;
    return S_OK;
}

HRESULT xml_document_get_xml( IXmlDocument *doc, HSTRING *xml )
{
    IXmlNodeSerializer *serializer;
    HRESULT hr;

    if (FAILED(hr = IXmlDocument_QueryInterface( doc, &IID_IXmlNodeSerializer, (void **)&serializer ))) return hr;
    hr = IXmlNodeSerializer_GetXml( serializer, xml );
    IXmlNodeSerializer_Release( serializer );
    return hr;
}

struct xml_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct xml_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct xml_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct xml_factory *impl = impl_from_IActivationFactory( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
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
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return xml_document_create( NULL, (IXmlDocument **)instance );
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

static struct xml_factory xml_factory =
{
    {&factory_vtbl},
    1,
};

IActivationFactory *xml_document_factory = &xml_factory.IActivationFactory_iface;
