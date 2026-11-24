#include "window_common.h"

extern HINSTANCE g_hInst;

namespace pse {

int Scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }
HMENU CtrlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

Theme CurrentTheme() {
    Theme t;
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    wchar_t forced[16] = {};
    bool dark = light == 0;
    if (GetEnvironmentVariableW(L"PSE_THEME", forced, ARRAYSIZE(forced))) dark = _wcsicmp(forced, L"dark") == 0;
    if (contrast) {
        t.window = GetSysColor(COLOR_3DFACE);
        t.text = t.faint = GetSysColor(COLOR_WINDOWTEXT);
        t.border = GetSysColor(COLOR_WINDOWFRAME);
        t.hover = t.copied = t.matchBack = GetSysColor(COLOR_HIGHLIGHT);
        t.matchLine = GetSysColor(COLOR_HOTLIGHT);
        t.badLine = GetSysColor(COLOR_WINDOWTEXT);
        t.inputBack = GetSysColor(COLOR_WINDOW);
    } else if (dark) {
        t.dark = true;
        t.window = RGB(32, 32, 32);
        t.text = RGB(255, 255, 255);
        t.faint = RGB(170, 170, 170);
        t.border = RGB(86, 86, 86);
        t.hover = RGB(52, 52, 52);
        t.copied = RGB(36, 72, 118);
        t.matchBack = RGB(28, 86, 46);
        t.matchLine = RGB(76, 194, 112);
        t.badLine = RGB(232, 72, 72);
        t.inputBack = RGB(25, 25, 25);
    } else {
        t.window = GetSysColor(COLOR_3DFACE);
        t.text = GetSysColor(COLOR_WINDOWTEXT);
        t.faint = RGB(96, 96, 96);
        t.border = RGB(130, 135, 144);
        t.hover = RGB(229, 236, 247);
        t.copied = RGB(204, 228, 247);
        t.matchBack = RGB(198, 239, 206);
        t.matchLine = RGB(16, 124, 16);
        t.badLine = RGB(209, 17, 17);
        t.inputBack = GetSysColor(COLOR_WINDOW);
    }
    return t;
}

ActivationContext::ActivationContext() {
    ACTCTXW act{sizeof(act)};
    act.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    act.hModule = g_hInst;
    act.lpResourceName = MAKEINTRESOURCEW(2);
    ctx_ = CreateActCtxW(&act);
    if (ctx_ != INVALID_HANDLE_VALUE && !ActivateActCtx(ctx_, &cookie_)) cookie_ = 0;
}

ActivationContext::~ActivationContext() {
    if (cookie_) DeactivateActCtx(0, cookie_);
    if (ctx_ != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx_);
}

HMODULE AddModuleRef() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&AddModuleRef), &module);
    return module;
}

HFONT MessageFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

void FillBox(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

int RunLoop(HWND hwnd) {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

void CenterOnCursorMonitor(HWND hwnd, int width, int height) {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(hwnd, nullptr, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2, mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace pse
