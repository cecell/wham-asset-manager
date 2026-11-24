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

// This runs on Explorer's own thread for every right-click on a folder, drive or
// .bsa: nothing here may touch the disk (GetFileAttributesW on a dropped network
// drive waits for its timeout). The shell item already knows: it was built from the
// folder's listing.
// A real folder: SFGAO_FOLDER on the file system, and not a stream (a .zip is both).
bool ItemIsFileFolder(IDataObject* data) {
    IShellItemArray* items = nullptr;
    if (FAILED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) || !items) return false;
    bool folder = false;
    DWORD count = 0;
    IShellItem* item = nullptr;
    if (SUCCEEDED(items->GetCount(&count)) && count == 1 && SUCCEEDED(items->GetItemAt(0, &item)) && item) {
        SFGAOF attrs = 0;
        item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STREAM, &attrs);  // S_FALSE when not all are set: read `attrs`
        folder = (attrs & SFGAO_FOLDER) && (attrs & SFGAO_FILESYSTEM) && !(attrs & SFGAO_STREAM);
        item->Release();
    }
    items->Release();
    return folder;
}

// A locked and released CF_HDROP, even when something throws.
struct DropMedium {
    STGMEDIUM stg{};
    HDROP drop = nullptr;
    ~DropMedium() {
        if (drop) GlobalUnlock(stg.hGlobal);
        ReleaseStgMedium(&stg);
    }
};

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
                // The handler is registered under Directory\Background, so this is a
                // folder; a pidl with no file-system path (inside a .bsa) fails here.
                if (!folder || !SHGetPathFromIDListEx(folder, buf, ARRAYSIZE(buf), GPFIDL_DEFAULT)) return E_FAIL;
                path_ = buf;
                return S_OK;
            }
            FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            DropMedium m;
            if (FAILED(data->GetData(&fe, &m.stg))) return E_INVALIDARG;
            m.drop = static_cast<HDROP>(GlobalLock(m.stg.hGlobal));
            if (m.drop && DragQueryFileW(m.drop, 0xFFFFFFFF, nullptr, 0) == 1) {
                const UINT len = DragQueryFileW(m.drop, 0, nullptr, 0);
                std::wstring path(len + 1, L' ');
                DragQueryFileW(m.drop, 0, path.data(), len + 1);
                path.resize(len);
                // A .bsa is taken by its name; anything else only if the shell says it is a folder.
                if (IsBsa(path) || ItemIsFileFolder(data)) path_ = std::move(path);
            }
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
        if (!IS_INTRESOURCE(info->lpVerb)) return E_INVALIDARG;
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
