// What WHAM's tool windows (tool_windows.cpp, search_window.cpp) share: colours
// that follow dark mode and high contrast, the manifest activation, a
// reference that keeps the DLL loaded while a window thread runs, and a few
// small drawing and layout helpers.
#pragma once

#include <windows.h>

namespace pse {

constexpr DWORD kDwmUseImmersiveDarkMode = 20;

int Scale(int v, UINT dpi);
HMENU CtrlId(int id);

struct Theme {
    bool dark = false;
    COLORREF window, text, faint, border, hover, copied, matchBack, matchLine, badLine, inputBack;
};
Theme CurrentTheme();

// Activates the DLL's own manifest (comctl32 v6) for the current thread.
class ActivationContext {
public:
    ActivationContext();
    ~ActivationContext();
    ActivationContext(const ActivationContext&) = delete;
    ActivationContext& operator=(const ActivationContext&) = delete;

private:
    HANDLE ctx_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
};

// Per-monitor DPI awareness for the windows made on this thread.
struct DpiScope {
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

// Takes a reference on this DLL (null on failure). A window thread passes it to
// FreeLibraryAndExitThread, so the DLL stays loaded until the thread is gone.
HMODULE AddModuleRef();

HFONT MessageFont(UINT dpi);
void FillBox(HDC dc, const RECT& r, COLORREF c);
int RunLoop(HWND hwnd);
void CenterOnCursorMonitor(HWND hwnd, int width, int height);

}  // namespace pse
