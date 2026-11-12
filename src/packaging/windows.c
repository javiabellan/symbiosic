#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { PATH_CAP = 32768, URL_CAP = 128 };
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
    const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
    if (!local) {
        return 1;
    }
    if (!_wgetenv(L"DATA_DIR")) {
        swprintf(data, PATH_CAP, L"%ls\\Symbiosic", local);
        set_path(L"DATA_DIR", data);
    }
    if (!_wgetenv(L"SSL_CERT_FILE")) {
        swprintf(data, PATH_CAP, L"%ls\\assets\\cacert.pem", root);
        set_path(L"SSL_CERT_FILE", data);
    }

    // Read the server's startup URL so browser launch follows successful initialization.
    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE reader, writer;
    if (!CreatePipe(&reader, &writer, &security, 0)) {
        return 1;
    }
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {.cb = sizeof(startup),
                            .dwFlags = STARTF_USESTDHANDLES,
                            .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                            .hStdOutput = writer,
                            .hStdError = GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process = {0};
    wchar_t command[] = L"bin\\symbiosic.exe";
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fwprintf(stderr, L"Cannot start Symbiosic (%lu).\n", GetLastError());
        CloseHandle(reader);
        CloseHandle(writer);
        return 1;
    }
    CloseHandle(writer);
    char line[URL_CAP] = {0};
    DWORD count;
    for (size_t i = 0; i < URL_CAP - 1; i++) {
        if (!ReadFile(reader, &line[i], 1, &count, NULL) || !count || line[i] == '\n') {
            break;
        }
    }
    CloseHandle(reader);
    if (strncmp(line, "http://127.0.0.1:", sizeof("http://127.0.0.1:") - 1) == 0) {
        wchar_t url[URL_CAP];
        MultiByteToWideChar(CP_UTF8, 0, line, -1, url, URL_CAP);
        url[wcscspn(url, L"\r\n")] = 0;
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
