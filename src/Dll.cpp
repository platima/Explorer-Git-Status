#include "OverlayHandler.h"
#include "StatusCache.h"
#include "GitQuery.h"

#include <new>
#include <string>

#include <initguid.h>

// {EF9CC9E5-741C-4C53-8FB4-D0C317C3464B}
DEFINE_GUID(CLSID_GitStatusClean, 0xef9cc9e5, 0x741c, 0x4c53, 0x8f, 0xb4, 0xd0, 0xc3, 0x17, 0xc3, 0x46, 0x4b);
// {D5935FBA-49FE-4239-8B45-CD55A1F2ECBC}
DEFINE_GUID(CLSID_GitStatusModified, 0xd5935fba, 0x49fe, 0x4239, 0x8b, 0x45, 0xcd, 0x55, 0xa1, 0xf2, 0xec, 0xbc);
// {76A9C9D1-B37A-4981-8087-F6B516EEE41C}
DEFINE_GUID(CLSID_GitStatusAheadBehind, 0x76a9c9d1, 0xb37a, 0x4981, 0x80, 0x87, 0xf6, 0xb5, 0x16, 0xee, 0xe4, 0x1c);

// Explorer only uses the first ~15 overlay identifiers, sorted by name, so the
// names start with spaces (as OneDrive, Dropbox and TortoiseGit also do).
const OverlayInfo g_overlays[] = {
    { &CLSID_GitStatusClean, L"   GitStatus1Clean", L"Explorer Git Status - Clean", RepoStatus::Clean, 0 },
    { &CLSID_GitStatusModified, L"   GitStatus2Modified", L"Explorer Git Status - Modified", RepoStatus::Modified, 1 },
    { &CLSID_GitStatusAheadBehind, L"   GitStatus3AheadBehind", L"Explorer Git Status - Ahead/Behind",
      RepoStatus::AheadBehind, 2 },
};
const size_t g_overlayCount = ARRAYSIZE(g_overlays);

HMODULE g_module = nullptr;
volatile LONG g_dllRefCount = 0;

namespace
{
// How long a cached status is trusted before being re-checked in the
// background. The stale value keeps being shown until the new one arrives.
constexpr auto kCacheTtl = std::chrono::seconds(30);

StatusCache* g_cache = nullptr;
INIT_ONCE g_cacheInit = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK CreateCache(PINIT_ONCE, PVOID, PVOID*)
{
    g_cache = new (std::nothrow) StatusCache(
        &QueryRepoStatus,
        [](const std::wstring& path) {
            // Ask Explorer to redraw the folder so it picks up the new overlay.
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
        },
        kCacheTtl);
    return g_cache != nullptr;
}

std::wstring GuidToString(const CLSID& clsid)
{
    wchar_t buffer[64] = {};
    StringFromGUID2(clsid, buffer, ARRAYSIZE(buffer));
    return buffer;
}

HRESULT SetRegString(HKEY root, const std::wstring& subKey, const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                              nullptr);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(rc);
}

const wchar_t kOverlayKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
} // namespace

StatusCache& GetStatusCache()
{
    // Intentionally never freed: the worker thread must not be joined from
    // DllMain (loader lock), and DllCanUnloadNow keeps the DLL loaded once the
    // cache exists.
    if (!InitOnceExecuteOnce(&g_cacheInit, CreateCache, nullptr, nullptr))
        throw std::bad_alloc();
    return *g_cache;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    for (size_t i = 0; i < g_overlayCount; ++i)
    {
        if (IsEqualCLSID(clsid, *g_overlays[i].clsid))
        {
            OverlayClassFactory* factory = new (std::nothrow) OverlayClassFactory(g_overlays[i]);
            if (!factory)
                return E_OUTOFMEMORY;
            HRESULT hr = factory->QueryInterface(riid, ppv);
            factory->Release();
            return hr;
        }
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    // Once the cache (and its worker thread) exists the DLL must stay loaded.
    return (g_dllRefCount == 0 && g_cache == nullptr) ? S_OK : S_FALSE;
}

STDAPI DllUnregisterServer();

STDAPI DllRegisterServer()
{
    wchar_t modulePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_module, modulePath, ARRAYSIZE(modulePath));
    if (len == 0 || len >= ARRAYSIZE(modulePath))
        return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr = S_OK;
    for (size_t i = 0; i < g_overlayCount && SUCCEEDED(hr); ++i)
    {
        const OverlayInfo& info = g_overlays[i];
        const std::wstring clsid = GuidToString(*info.clsid);
        const std::wstring clsidKey = L"Software\\Classes\\CLSID\\" + clsid;

        hr = SetRegString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, info.description);
        if (SUCCEEDED(hr))
            hr = SetRegString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, modulePath);
        if (SUCCEEDED(hr))
            hr = SetRegString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
        if (SUCCEEDED(hr))
            hr = SetRegString(HKEY_LOCAL_MACHINE, std::wstring(kOverlayKey) + info.registryName, nullptr, clsid);
        if (SUCCEEDED(hr))
            hr = SetRegString(HKEY_LOCAL_MACHINE, kApprovedKey, clsid.c_str(), info.description);
    }

    if (FAILED(hr))
        DllUnregisterServer();
    return hr;
}

STDAPI DllUnregisterServer()
{
    for (size_t i = 0; i < g_overlayCount; ++i)
    {
        const OverlayInfo& info = g_overlays[i];
        const std::wstring clsid = GuidToString(*info.clsid);

        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kOverlayKey) + info.registryName).c_str());
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (L"Software\\Classes\\CLSID\\" + clsid).c_str());
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kApprovedKey, clsid.c_str());
    }
    return S_OK;
}
