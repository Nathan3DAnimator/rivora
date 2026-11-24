// (2026-11-23, the rename to Rivora Engine) realengine.exe: the name the 0.49 and older installs know. Their shortcuts
// open it and their updater checks the package has it and starts it after installing; this small program only starts
// rivora.exe beside it with the same arguments and working folder, and goes. Shipped by make_dist.ps1 for some
// releases, until every install has moved to the new name.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)inst, (void)prev, (void)show;
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return 1;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return 1;
    wcscpy_s(slash + 1, (size_t)(exe + MAX_PATH * 2 - (slash + 1)), L"rivora.exe");
    // the command line: "<rivora.exe>" and the arguments this one got
    static wchar_t cmd[32768];
    _snwprintf_s(cmd, 32768, _TRUNCATE, L"\"%ls\"%ls%ls", exe, args && args[0] ? L" " : L"", args ? args : L"");
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si), ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"rivora.exe was not found next to realengine.exe. Real Engine is now Rivora Engine: download it again.",
                    L"Rivora Engine", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return 0;
}
