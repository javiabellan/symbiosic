#import <Cocoa/Cocoa.h>
#include "app.h"
#include <stdlib.h>
#include <unistd.h>

bool desktop_macos_bootstrap(char **error) {
    @autoreleasepool {
        NSBundle *bundle = NSBundle.mainBundle;
        if (![bundle.bundlePath hasSuffix:@".app"]) {
            return true;
        }
        if (chdir(bundle.resourcePath.fileSystemRepresentation)) {
            *error = xstrdup("Cannot open the application's bundled resources");
            return false;
        }
        if (!getenv("DATA_DIR")) {
            NSString *data = [NSHomeDirectory()
                stringByAppendingPathComponent:@"Library/Application Support/Symbiosic"];
            setenv("DATA_DIR", data.fileSystemRepresentation, 0);
        }
    }
    return true;
}

/* Route Cmd-Q and Dock Quit through window close so C17 can drain its workers. */
static __weak NSWindow *workspace_window;
@interface SymbiosicApplication : NSApplication
@end
@implementation SymbiosicApplication
- (void)terminate:(id)sender {
    [workspace_window performClose:sender];
}
@end

void desktop_macos_attach(void *window) {
    workspace_window = (__bridge NSWindow *)window;
}

void desktop_macos_prepare(void) {
    [SymbiosicApplication sharedApplication];
    NSMenu *menu = [NSMenu new];
    NSMenuItem *app = [NSMenuItem new];
    NSMenu *actions = [NSMenu new];
    [actions addItemWithTitle:@"About Symbiosic"
                      action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [actions addItem:NSMenuItem.separatorItem];
    [actions addItemWithTitle:@"Quit Symbiosic" action:@selector(terminate:) keyEquivalent:@"q"];
    app.submenu = actions;
    [menu addItem:app];
    NSMenuItem *edit = [NSMenuItem new];
    NSMenu *editing = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editing addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [editing addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
    [editing addItem:NSMenuItem.separatorItem];
    [editing addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editing addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editing addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editing addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    edit.submenu = editing;
    [menu addItem:edit];
    NSApp.mainMenu = menu;
}
