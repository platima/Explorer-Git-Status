#pragma once

#include "GitStatus.h"

#include <windows.h>
#include <shlobj.h>

class StatusCache;

// Description of one overlay (one colour). Explorer needs a separate COM class
// per overlay icon, so there is one of these for each status we display.
struct OverlayInfo
{
    const CLSID* clsid;
    const wchar_t* registryName; // Name under ShellIconOverlayIdentifiers.
    const wchar_t* description;
    RepoStatus status;           // Status this overlay is shown for.
    int iconIndex;               // Index of the icon resource in this DLL.
};

extern const OverlayInfo g_overlays[];
extern const size_t g_overlayCount;

extern HMODULE g_module;
extern volatile LONG g_dllRefCount;

// Shared non-blocking status cache (created on first use).
StatusCache& GetStatusCache();

class OverlayHandler final : public IShellIconOverlayIdentifier
{
public:
    explicit OverlayHandler(const OverlayInfo& info);

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IShellIconOverlayIdentifier
    IFACEMETHODIMP IsMemberOf(PCWSTR path, DWORD attributes) override;
    IFACEMETHODIMP GetOverlayInfo(PWSTR iconFile, int cchMax, int* index, DWORD* flags) override;
    IFACEMETHODIMP GetPriority(int* priority) override;

private:
    ~OverlayHandler();

    LONG m_refCount = 1;
    const OverlayInfo& m_info;
};

class OverlayClassFactory final : public IClassFactory
{
public:
    explicit OverlayClassFactory(const OverlayInfo& info);

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override;
    IFACEMETHODIMP LockServer(BOOL lock) override;

private:
    ~OverlayClassFactory();

    LONG m_refCount = 1;
    const OverlayInfo& m_info;
};
