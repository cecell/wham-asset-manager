// Restarts Windows Explorer after an update, so the new DLL is the one it
// loads. Restart Manager proved unreliable for the shell (a long wait, then no
// taskbar), so this does what Task Manager's "Restart" does:
//  1. ends every Explorer process of this session at once (the shell, and
//     folder windows in their own process); the setup said beforehand that
//     every File Explorer window closes;
//  2. starts explorer.exe again and waits for the taskbar.
// Other programs that load the DLL (Everything's right-click menu, for one)
// keep their copy until they restart.
#include "restart_explorer.h"

#include <tlhelp32.h>

#include <string>
#include <vector>

namespace pse {
namespace {

bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

// Explorer processes in this user's session.
std::vector<DWORD> ExplorerProcesses() {
    std::vector<DWORD> out;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD session = 0;
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0 && ProcessIdToSessionId(pe.th32ProcessID, &session) && session == mySession)
            out.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return out;
}

bool WaitFor(ULONGLONG ms, bool (*done)()) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < ms) {
        if (done()) return true;
        Sleep(100);
    }
    return done();
}

}  // namespace

HRESULT RestartExplorer() {
    // An elevated setup would start an elevated Explorer: leave it alone, the
    // new version then runs after the next sign-in or Explorer restart.
    if (Elevated()) return S_FALSE;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return S_FALSE;  // no shell running here: nothing to restart

    // The setup told the user every File Explorer window closes: no waiting for a
    // polite exit, which only made them watch the windows hang for seconds.
    for (DWORD pid : ExplorerProcesses()) {
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(p, 0);
            WaitForSingleObject(p, 3000);
            CloseHandle(p);
        }
    }

    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(windows) + L"\\explorer.exe\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, windows, &si, &pi)) return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Back once the taskbar is.
    return WaitFor(15000, [] { return FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr; }) ? S_OK : S_FALSE;
}

}  // namespace pse
