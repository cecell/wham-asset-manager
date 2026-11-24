#include "search_menu.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <new>
#include <string>

#include "context_menu.h"
#include "search_window.h"

namespace pse {
namespace {

bool IsBsa(const std::wstring& path) { return _wcsicmp(PathFindExtensionW(path.c_str()), L".bsa") == 0; }

bool IsDirectory(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

class SearchMenu final : public IShellExtInit, public IContextMenu {
public:
    SearchMenu() { InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) {
            *ppv = static_cast<IShellExtInit*>(this);
        } else if (riid == IID_IContextMenu) {
            *ppv = static_cast<IContextMenu*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // One folder or drive, the background of a folder (no data object, just its
    // pidl), or exactly one .bsa. Anything else, including folders with no
    // file-system path (inside a browsed .bsa, Control Panel), gets no item.
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE folder, IDataObject* data, HKEY) override {
        path_.clear();
        try {
            if (!data) {
                wchar_t buf[MAX_PATH * 2] = {};
                if (!folder || !SHGetPathFromIDListEx(folder, buf, ARRAYSIZE(buf), GPFIDL_DEFAULT)) return E_FAIL;
                if (!IsDirectory(buf)) return E_FAIL;
                path_ = buf;
                return S_OK;
            }
            FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM stg{};
            if (FAILED(data->GetData(&fe, &stg))) return E_INVALIDARG;
            if (auto drop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
                if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 1) {
                    const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
                    std::wstring path(len + 1, L' ');
                    DragQueryFileW(drop, 0, path.data(), len + 1);
                    path.resize(len);
                    if (IsDirectory(path) || (IsBsa(path) && IsFile(path))) path_ = std::move(path);
                }
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
        } catch (...) {
            path_.clear();
            return E_OUTOFMEMORY;
        }
        return path_.empty() ? E_FAIL : S_OK;
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override {
        if ((flags & CMF_DEFAULTONLY) || path_.empty() || last < first) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first, L"Search text…");
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
    }

    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override {
        if (!info) return E_POINTER;
        if (HIWORD(info->lpVerb)) return E_INVALIDARG;
        if (LOWORD(info->lpVerb) != 0 || path_.empty()) return E_INVALIDARG;
        try {
            ShowSearchWindowAsync(path_);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }

private:
    ~SearchMenu() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
    std::wstring path_;
};

class Factory final : public IClassFactory {
public:
    Factory() { InterlockedIncrement(&g_dllRefs); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* obj = new (std::nothrow) SearchMenu();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_dllRefs);
        else InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }

private:
    ~Factory() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
};

}  // namespace

HRESULT CreateSearchMenuFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse
