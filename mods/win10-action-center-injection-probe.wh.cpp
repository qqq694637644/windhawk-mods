// ==WindhawkMod==
// @id              win10-action-center-injection-probe
// @name            Win10 Action Center Injection Probe (diagnostics only)
// @description     Diagnoses Windhawk loading in the native Windows 10 Action Center host without installing hooks or changing UI.
// @version         0.1.0
// @author          qqq694637644
// @github          https://github.com/qqq694637644
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows 10 Action Center injection probe

This is diagnostic only. It does not hook APIs, create controls,
modify brightness/power settings, restart Explorer, or change any Windows file.

It is designed for Windows 10 build 19045 and targets
ShellExperienceHost.exe, which hosts the native Windows 10 Action Center.
The mod logs the process path, PID and Windows version upon loading.
For other Windows versions, initialization fails without changing anything.

Build passing does not prove that Windhawk runs safely on a real machine.

Do not load this probe on a machine experiencing unrecoverable freezes.
Testing the runtime requires an isolated Windows 10 VM with a snapshot.
*/
// ==/WindhawkModReadme==

#include <windows.h>

using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);

BOOL Wh_ModInit() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersion_t>(
              GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;

    if (!rtlGetVersion || rtlGetVersion(&version) != 0) {
        Wh_Log(L"Probe: unable to determine Windows version, refusing to load");
        return FALSE;
    }

    Wh_Log(L"Probe: PID=%lu process=%s Windows=%lu.%lu.%lu",
           GetCurrentProcessId(), path, version.dwMajorVersion,
           version.dwMinorVersion, version.dwBuildNumber);

    // This probe deliberately declines initialization on Windows 11 and
    // all Windows 10 builds other than the tested target.
    if (version.dwMajorVersion != 10 || version.dwBuildNumber != 19045) {
        Wh_Log(L"Probe: not Windows 10 build 19045, refusing to load");
        return FALSE;
    }

    Wh_Log(L"Probe: loaded, with zero hooks and zero UI changes");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Probe: unloaded");
}
