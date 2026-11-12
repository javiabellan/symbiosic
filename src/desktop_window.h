#ifndef SYMBIOSIC_DESKTOP_WINDOW_H
#define SYMBIOSIC_DESKTOP_WINDOW_H
#include <webview/webview.h>

/* Call on the main thread; all UI uses the operating system's webview. */
webview_t desktop_window_create(const char *url);
#ifdef __APPLE__
void desktop_macos_prepare(void);
void desktop_macos_attach(void *window);
#endif
#endif
