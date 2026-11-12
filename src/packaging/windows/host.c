#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../desktop_window.h"

enum { PATH_CAP = 32768, URL_CAP = 128 };
static void set_path(const wchar_t *name, const wchar_t *value) {
    wchar_t path[PATH_CAP];
    if (!value || wcslen(value) + 2 >= PATH_CAP) {
        return;
    }
    /* The POSIX backend uses /c/Users/... for native C:\Users\... paths. */
    if (value[0] && value[1] == L':') {
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
static void stop_window(webview_t window, void *arg) {
    (void)arg;
    webview_terminate(window);
}
typedef struct {
    HANDLE process;
    webview_t window;
} Watcher;
static DWORD WINAPI watch_server(void *arg) {
    Watcher *watcher = arg;
    WaitForSingleObject(watcher->process, INFINITE);
    webview_dispatch(watcher->window, stop_window, NULL);
    return 0;
}
static void show_error(const wchar_t *message) {
    MessageBoxW(NULL, message, L"Symbiosic", MB_OK | MB_ICONERROR);
}
static int run_host(void) {
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
    if (!local || wcslen(local) + 64 >= PATH_CAP) {
        return 1;
    }
    swprintf(data, PATH_CAP, L"%ls\\Symbiosic", local);
    CreateDirectoryW(data, NULL);
    if (!_wgetenv(L"DATA_DIR")) {
        set_path(L"DATA_DIR", data);
    }
    /* WebView2 needs a writable native profile outside the installation folder. */
    swprintf(data, PATH_CAP, L"%ls\\Symbiosic\\WebView2", local);
    if (!_wgetenv(L"WEBVIEW2_USER_DATA_FOLDER")) {
        SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", data);
    }
    if (!_wgetenv(L"SSL_CERT_FILE")) {
        if (wcslen(root) + 32 >= PATH_CAP) {
            return 1;
        }
        swprintf(data, PATH_CAP, L"%ls\\assets\\cacert.pem", root);
        set_path(L"SSL_CERT_FILE", data);
    }

    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE output_read = NULL, output_write = NULL, input_read = NULL, input_write = NULL;
    HANDLE log = INVALID_HANDLE_VALUE, watcher_thread = NULL;
    PROCESS_INFORMATION process = {0};
    webview_t window = NULL;
    DWORD status = 1;
    if (!CreatePipe(&output_read, &output_write, &security, 0) ||
        !CreatePipe(&input_read, &input_write, &security, 0) ||
        !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0)) {
        show_error(L"Cannot create the desktop host pipes.");
        goto cleanup;
    }
    swprintf(data, PATH_CAP, L"%ls\\Symbiosic\\server.log", local);
    log = CreateFileW(data, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        show_error(L"Cannot open %LOCALAPPDATA%\\Symbiosic\\server.log.");
        goto cleanup;
    }
    STARTUPINFOW startup = {.cb = sizeof(startup),
                            .dwFlags = STARTF_USESTDHANDLES,
                            .hStdInput = input_read, .hStdOutput = output_write, .hStdError = log};
    wchar_t command[] = L"bin\\symbiosic.exe --headless --parent-stdin";
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        show_error(L"Cannot start the backend. Keep all of the extracted app files together.");
        goto cleanup;
    }
    CloseHandle(output_write);
    output_write = NULL;
    CloseHandle(input_read);
    input_read = NULL;

    /* The startup URL is emitted only after bind succeeds; bound startup waiting. */
    char line[URL_CAP] = {0};
    size_t used = 0;
    ULONGLONG deadline = GetTickCount64() + 30000;
    BOOL ready = FALSE;
    while (used < sizeof(line) - 1 && GetTickCount64() < deadline) {
        DWORD available = 0, count = 0;
        if (!PeekNamedPipe(output_read, NULL, 0, NULL, &available, NULL)) {
            break;
        }
        if (!available) {
            if (WaitForSingleObject(process.hProcess, 20) != WAIT_TIMEOUT) {
                break;
            }
            continue;
        }
        if (!ReadFile(output_read, &line[used], 1, &count, NULL) || !count) {
            break;
        }
        if (line[used] == '\n') {
            line[used] = 0;
            ready = TRUE;
            break;
        }
        used++;
    }
    line[strcspn(line, "\r\n")] = 0;
    unsigned port = 0;
    char extra;
    if (!ready || sscanf(line, "http://127.0.0.1:%u%c", &port, &extra) != 1 ||
        port < 1 || port > 65535) {
        show_error(L"The backend could not start. See %LOCALAPPDATA%\\Symbiosic\\server.log.");
        goto cleanup;
    }
    window = desktop_window_create(line);
    if (!window) {
        show_error(L"Cannot create the native window. Install Microsoft Edge WebView2 Runtime and retry.");
        goto cleanup;
    }
    Watcher watcher = {.process = process.hProcess, .window = window};
    watcher_thread = CreateThread(NULL, 0, watch_server, &watcher, 0, NULL);
    if (!watcher_thread) {
        show_error(L"Cannot monitor the backend process.");
        goto cleanup;
    }
    status = WEBVIEW_FAILED(webview_run(window)) ? 1 : 0;

cleanup:
    /* EOF requests normal C17 shutdown, including queued jobs and active workers.
       The pipe also closes automatically if the host crashes or is terminated. */
    if (input_write) CloseHandle(input_write);
    if (input_read) CloseHandle(input_read);
    if (output_write) CloseHandle(output_write);
    if (output_read) CloseHandle(output_read);
    if (process.hProcess) {
        if (WaitForSingleObject(process.hProcess, 125000) == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            status = 1;
        }
        DWORD child_status = 1;
        GetExitCodeProcess(process.hProcess, &child_status);
        if (child_status) status = child_status;
    }
    if (watcher_thread) {
        WaitForSingleObject(watcher_thread, INFINITE);
        CloseHandle(watcher_thread);
    }
    if (window) webview_destroy(window);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return (int)status;
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, wchar_t *command, int show) {
    (void)instance;
    (void)previous;
    (void)command;
    (void)show;
    return run_host();
}
