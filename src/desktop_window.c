#include "desktop_window.h"
#include <stddef.h>

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
        WEBVIEW_FAILED(webview_set_size(window, 640, 480, WEBVIEW_HINT_MIN)) ||
        WEBVIEW_FAILED(webview_set_size(window, 1280, 840, WEBVIEW_HINT_NONE)) ||
        WEBVIEW_FAILED(webview_navigate(window, url))) {
        webview_destroy(window);
        return NULL;
    }
    return window;
}
