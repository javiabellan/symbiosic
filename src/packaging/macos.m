#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate> {
  @private
    NSTask *_server;
    NSMutableData *_output;
    BOOL _opened;
}
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSMenu *menu = [NSMenu new];
    NSMenuItem *app = [NSMenuItem new];
    NSMenu *actions = [NSMenu new];
    [actions addItemWithTitle:@"Quit Symbiosic" action:@selector(terminate:) keyEquivalent:@"q"];
    app.submenu = actions;
    [menu addItem:app];
    NSApp.mainMenu = menu;

    // Bundle resources are read-only; all mutable state belongs to the user's data folder.
    NSString *resources = NSBundle.mainBundle.resourcePath;
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    if (!env[@"DATA_DIR"]) {
        env[@"DATA_DIR"] = [NSHomeDirectory()
            stringByAppendingPathComponent:@"Library/Application Support/Symbiosic"];
    }
    _server = [NSTask new];
    _server.executableURL =
        [NSURL fileURLWithPath:[resources stringByAppendingPathComponent:@"bin/symbiosic"]];
    _server.currentDirectoryURL = [NSURL fileURLWithPath:resources];
    _server.environment = env;
    NSPipe *pipe = [NSPipe pipe];
    _server.standardOutput = pipe;
    _output = [NSMutableData new];
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
      NSData *data = handle.availableData;
      if (data.length == 0) {
          handle.readabilityHandler = nil;
          return;
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        [self->_output appendData:data];
        NSString *text = [[NSString alloc] initWithData:self->_output
                                               encoding:NSUTF8StringEncoding];
        if (self->_opened || ![text containsString:@"\n"]) {
            return;
        }
        NSString *line = [text componentsSeparatedByString:@"\n"].firstObject;
        if (![line hasPrefix:@"http://127.0.0.1:"]) {
            return;
        }
        self->_opened = YES;
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:line]];
      });
    };
    _server.terminationHandler = ^(NSTask *task) {
      (void)task;
      dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
      });
    };
    NSError *error = nil;
    if (![_server launchAndReturnError:&error]) {
        NSLog(@"Symbiosic: %@", error.localizedDescription);
        [NSApp terminate:nil];
    }
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    if (_server.running) {
        [_server terminate];
        [_server waitUntilExit];
    }
    return NSTerminateNow;
}
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
