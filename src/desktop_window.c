#include "desktop_window.h"
#include <stddef.h>

static webview_error_t set_size(webview_t window, int width, int height, webview_hint_t hint) {
    webview_error_t status = webview_set_size(window, width, height, hint);
#ifdef __linux__
    /* webview 0.12.0's GTK backend applies valid sizes, then unconditionally
       returns INVALID_ARGUMENT. Both hints below are supported by that backend. */
    if (status == WEBVIEW_ERROR_INVALID_ARGUMENT) {
        return WEBVIEW_ERROR_OK;
    }
#endif
    return status;
}

webview_t desktop_window_create(const char *url) {
#ifdef __APPLE__
    desktop_macos_prepare();
#endif
    webview_t window = webview_create(0, NULL);
    if (!window) {
        return NULL;
    }
#ifdef __APPLE__
    desktop_macos_attach(webview_get_window(window));
#endif
    if (WEBVIEW_FAILED(webview_set_title(window, "Symbiosic")) ||
        WEBVIEW_FAILED(set_size(window, 640, 480, WEBVIEW_HINT_MIN)) ||
        WEBVIEW_FAILED(set_size(window, 1280, 840, WEBVIEW_HINT_NONE)) ||
        WEBVIEW_FAILED(webview_navigate(window, url))) {
        webview_destroy(window);
        return NULL;
    }
    return window;
}
