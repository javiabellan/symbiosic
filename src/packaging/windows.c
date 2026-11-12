#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum { PATH_CAP = 32768, URL_CAP = 128, START_DELAY_MS = 1500 };
static void set_path(const wchar_t *name, const wchar_t *value) {
    wchar_t path[PATH_CAP];
    if (!value || wcslen(value) + 2 >= PATH_CAP) {
        return;
    }
    // MSYS uses /c/Users/... for native C:\Users\... paths.
    if (value[1] == L':') {
        swprintf(path, PATH_CAP, L"/%lc%ls", value[0], value + 2);
    } else {
        wcsncpy(path, value, PATH_CAP - 1);
        path[PATH_CAP - 1] = 0;
    }
    for (wchar_t *p = path; *p; p++) {
        if (*p == L'\\') {
            *p = L'/';
        }
    }
    SetEnvironmentVariableW(name, path);
}
int wmain(void) {
    wchar_t root[PATH_CAP], data[PATH_CAP];
    DWORD length = GetModuleFileNameW(NULL, root, PATH_CAP);
    if (!length || length >= PATH_CAP) {
        return 1;
    }
    wchar_t *slash = wcsrchr(root, L'\\');
    if (!slash) {
        return 1;
    }
    *slash = 0;
    if (!SetCurrentDirectoryW(root)) {
        return 1;
    }
    set_path(L"HOME", _wgetenv(L"USERPROFILE"));
    if (!_wgetenv(L"DATA_DIR")) {
        swprintf(data, PATH_CAP, L"%ls\\Symbiosic", _wgetenv(L"LOCALAPPDATA"));
        set_path(L"DATA_DIR", data);
    }
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    wchar_t command[] = L"bin\\symbiosic.exe";
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fwprintf(stderr, L"Cannot start Symbiosic (%lu).\n", GetLastError());
        return 1;
    }
    // The server prints its URL; the console also provides an explicit shutdown action.
    const wchar_t *port = _wgetenv(L"PORT");
    wchar_t url[URL_CAP];
    swprintf(url, URL_CAP, L"http://127.0.0.1:%ls", port ? port : L"4317");
    Sleep(START_DELAY_MS);
    if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        wprintf(L"Close this window to stop Symbiosic.\n");
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)status;
}
