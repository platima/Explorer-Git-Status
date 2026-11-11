#include "OverlayHandler.h"

#include "StatusCache.h"

#include <new>
#include <string>

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

namespace
{
std::wstring NormalisePath(PCWSTR path)
{
    std::wstring key(path);
    // Keep "C:\" intact, but strip trailing separators elsewhere.
    while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/'))
        key.pop_back();
    if (!key.empty())
        CharLowerBuffW(&key[0], static_cast<DWORD>(key.size()));
    return key;
}
} // namespace

OverlayHandler::OverlayHandler(const OverlayInfo& info) : m_info(info)
{
    InterlockedIncrement(&g_dllRefCount);
}

OverlayHandler::~OverlayHandler()
{
    InterlockedDecrement(&g_dllRefCount);
}

IFACEMETHODIMP OverlayHandler::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IShellIconOverlayIdentifier))
    {
        *ppv = static_cast<IShellIconOverlayIdentifier*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) OverlayHandler::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

IFACEMETHODIMP_(ULONG) OverlayHandler::Release()
{
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0)
        delete this;
    return count;
}

IFACEMETHODIMP OverlayHandler::IsMemberOf(PCWSTR path, DWORD attributes)
{
    if (!path || !*path)
        return S_FALSE;

    // Only folders can be repositories. Explorer may pass 0 when it does not
    // know the attributes; the background check copes with files anyway.
    if (attributes != 0 && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return S_FALSE;

    // Never touch cloud placeholders: probing them would trigger a download.
    if (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
        return S_FALSE;

    try
    {
        // Non-blocking: returns the cached value and schedules a background
        // refresh if needed, so Explorer never waits on git.
        RepoStatus status = GetStatusCache().Get(NormalisePath(path));
        return status == m_info.status ? S_OK : S_FALSE;
    }
    catch (...)
    {
        return S_FALSE;
    }
}

IFACEMETHODIMP OverlayHandler::GetOverlayInfo(PWSTR iconFile, int cchMax, int* index, DWORD* flags)
{
    if (!iconFile || !index || !flags || cchMax <= 0)
        return E_POINTER;

    DWORD len = GetModuleFileNameW(g_module, iconFile, static_cast<DWORD>(cchMax));
    if (len == 0 || len >= static_cast<DWORD>(cchMax))
        return E_FAIL;

    *index = m_info.iconIndex;
    *flags = ISIOI_ICONFILE | ISIOI_ICONINDEX;
    return S_OK;
}

IFACEMETHODIMP OverlayHandler::GetPriority(int* priority)
{
    if (!priority)
        return E_POINTER;
    // Only one of our overlays ever matches a given folder, so a fixed
    // priority is fine; 0 is the highest.
    *priority = 0;
    return S_OK;
}

OverlayClassFactory::OverlayClassFactory(const OverlayInfo& info) : m_info(info)
{
    InterlockedIncrement(&g_dllRefCount);
}

OverlayClassFactory::~OverlayClassFactory()
{
    InterlockedDecrement(&g_dllRefCount);
}

IFACEMETHODIMP OverlayClassFactory::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
    {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) OverlayClassFactory::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

IFACEMETHODIMP_(ULONG) OverlayClassFactory::Release()
{
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0)
        delete this;
    return count;
}

IFACEMETHODIMP OverlayClassFactory::CreateInstance(IUnknown* outer, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (outer)
        return CLASS_E_NOAGGREGATION;

    OverlayHandler* handler = new (std::nothrow) OverlayHandler(m_info);
    if (!handler)
        return E_OUTOFMEMORY;
    HRESULT hr = handler->QueryInterface(riid, ppv);
    handler->Release();
    return hr;
}

IFACEMETHODIMP OverlayClassFactory::LockServer(BOOL lock)
{
    if (lock)
        InterlockedIncrement(&g_dllRefCount);
    else
        InterlockedDecrement(&g_dllRefCount);
    return S_OK;
}
