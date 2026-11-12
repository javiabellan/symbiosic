#include "app.h"
#include "desktop_window.h"
#include <stdio.h>

typedef struct {
    App *app;
    webview_t window;
    uv_async_t stop;
} Desktop;

static void stop_server(uv_async_t *handle) {
    Desktop *desktop = handle->data;
    app_shutdown(desktop->app);
    uv_close((uv_handle_t *)handle, NULL);
}
static void stop_window(webview_t window, void *arg) {
    (void)arg;
    webview_terminate(window);
}
static void server_thread(void *arg) {
    Desktop *desktop = arg;
    uv_run(&desktop->app->loop, UV_RUN_DEFAULT);
    /* Dispatch also keeps termination on the UI thread on every backend. */
    webview_dispatch(desktop->window, stop_window, NULL);
}
bool desktop_run(App *app, char **error) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", app->cfg.port);
    Desktop desktop = {.app = app, .window = desktop_window_create(url)};
    if (!desktop.window) {
        *error = xstrdup("Cannot create the native webview. Check the desktop display and "
                         "system webview runtime, or run with --headless.");
        return false;
    }
    int rc = uv_async_init(&app->loop, &desktop.stop, stop_server);
    if (rc) {
        webview_destroy(desktop.window);
        *error = fmt("Cannot initialize desktop shutdown: %s", uv_strerror(rc));
        return false;
    }
    desktop.stop.data = &desktop;
    /* Signals can finish the server even while the window is still open. */
    uv_unref((uv_handle_t *)&desktop.stop);
    uv_thread_t thread;
    rc = uv_thread_create(&thread, server_thread, &desktop);
    if (rc) {
        uv_close((uv_handle_t *)&desktop.stop, NULL);
        uv_run(&app->loop, UV_RUN_NOWAIT);
        webview_destroy(desktop.window);
        *error = fmt("Cannot start server thread: %s", uv_strerror(rc));
        return false;
    }
    webview_error_t result = webview_run(desktop.window);
    /* Only the server thread mutates App or libuv handles. */
    uv_async_send(&desktop.stop);
    uv_thread_join(&thread);
    if (!uv_is_closing((uv_handle_t *)&desktop.stop)) {
        uv_close((uv_handle_t *)&desktop.stop, NULL);
    }
    uv_run(&app->loop, UV_RUN_DEFAULT);
    webview_destroy(desktop.window);
    if (WEBVIEW_FAILED(result)) {
        *error = fmt("Native webview loop failed (%d)", result);
        return false;
    }
    return true;
}
