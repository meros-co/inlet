// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// The COM plumbing an ASIO host expects of a driver DLL: a class factory for
// the driver's CLSID, and self-registration under HKLM\SOFTWARE\ASIO, which is
// where hosts list drivers from. The installer calls DllRegisterServer.

#include "InletAsio.h"

#include <atomic>
#include <new>
#include <string>

#include <olectl.h>

namespace
{

HMODULE gModule = nullptr;
std::atomic<long> gLocks { 0 };
std::atomic<long> gObjects { 0 };

const wchar_t* const kDriverKey = L"SOFTWARE\\ASIO\\Inlet";
const wchar_t* const kDescription = L"Inlet";

std::wstring clsidString()
{
    wchar_t buf[64] = {};
    StringFromGUID2(inlet::kInletAsioClsid, buf, 64);
    return buf;
}

class Factory : public IClassFactory
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // static lifetime
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (outer != nullptr)
            return CLASS_E_NOAGGREGATION;
        auto* driver = new (std::nothrow) inlet::InletAsio();
        if (driver == nullptr)
            return E_OUTOFMEMORY;
        ++gObjects;
        const HRESULT hr = driver->QueryInterface(riid, ppv);
        driver->Release();   // the caller's reference, if any, keeps it
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        lock ? ++gLocks : --gLocks;
        return S_OK;
    }
};

Factory gFactory;

LONG setString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          (DWORD) ((value.size() + 1) * sizeof(wchar_t)));
}

} // namespace

// Counted so DllCanUnloadNow can answer; the driver object decrements on delete.
void inletAsioObjectGone()
{
    --gObjects;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        gModule = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (clsid != inlet::kInletAsioClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    return gFactory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return gLocks.load() == 0 && gObjects.load() <= 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(gModule, path, MAX_PATH);
    const std::wstring clsid = clsidString();

    HKEY key = nullptr;
    const std::wstring server = L"CLSID\\" + clsid + L"\\InprocServer32";
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, server.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return SELFREG_E_CLASS;
    setString(key, nullptr, path);
    setString(key, L"ThreadingModel", L"Apartment");
    RegCloseKey(key);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        setString(key, nullptr, kDescription);
        RegCloseKey(key);
    }

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kDriverKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return SELFREG_E_CLASS;
    setString(key, L"CLSID", clsid);
    setString(key, L"Description", kDescription);
    RegCloseKey(key);
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    const std::wstring clsid = clsidString();
    RegDeleteTreeW(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kDriverKey);
    return S_OK;
}
