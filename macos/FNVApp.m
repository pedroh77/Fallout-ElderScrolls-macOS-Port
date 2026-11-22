#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <CommonCrypto/CommonDigest.h>
#import "FNVSurface.h"
#import "FNVPad.h"
#include "FNVDisplay.h"
#ifndef FNV_PROBE
#include "FNVNativeMedia.h"
#endif
#include "input.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef FNV_GAME_FALLOUT3
#define GAME_TITLE @"Fallout 3"
#define GAME_EXE   @"Fallout3.exe"
#define GAME_ESM   @"Data/Fallout3.esm"
#define GAME_DIR   @"Fallout3"
#define GAME_LOG   @"fallout3.log"
#define GAME_ARGV0 "f3-macos"
#define GAME_EXE_C "Fallout3.exe"
#else
#define GAME_TITLE @"Fallout: New Vegas"
#define GAME_EXE   @"FalloutNV.exe"
#define GAME_ESM   @"Data/FalloutNV.esm"
#define GAME_DIR   @"FalloutNV"
#define GAME_LOG   @"fnv.log"
#define GAME_ARGV0 "fnv-macos"
#define GAME_EXE_C "FalloutNV.exe"
#endif

@interface FNVMetalView : NSView
@end
@implementation FNVMetalView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
@end

static CAMetalLayer *screenLayer;
static NSWindow *mainWindow;
static pthread_mutex_t activeLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t activeChanged = PTHREAD_COND_INITIALIZER;
static BOOL active = YES;

static void setActive(BOOL value) {
    pthread_mutex_lock(&activeLock);
    active = value;
    pthread_cond_broadcast(&activeChanged);
    pthread_mutex_unlock(&activeLock);
}

void fnv_wait_active(void) {
    pthread_mutex_lock(&activeLock);
    while (!active) pthread_cond_wait(&activeChanged, &activeLock);
    pthread_mutex_unlock(&activeLock);
}

static void onMain(dispatch_block_t work) {
    if (NSThread.isMainThread) work();
    else dispatch_sync(dispatch_get_main_queue(), work);
}

CAMetalLayer *fnv_surface(id<MTLDevice> device, CGSize size) {
    onMain(^{
        screenLayer.device = device;
        screenLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        screenLayer.framebufferOnly = NO;
        screenLayer.drawableSize = size;
        screenLayer.contentsGravity = kCAGravityResizeAspect;
        screenLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        screenLayer.displaySyncEnabled = getenv("FNV_NOVSYNC") ? NO : YES;
        screenLayer.maximumDrawableCount = 3;
    });
    return screenLayer;
}

void fnv_resize_surface(CGSize size) {
    onMain(^{
        screenLayer.drawableSize = size;
    });
}

// macOS virtual key code -> DirectInput scancode (DIK_*)
static const uint8_t mac2dik[128] = {
    [0x00] = 0x1E, [0x01] = 0x1F, [0x02] = 0x20, [0x03] = 0x21, [0x04] = 0x23, [0x05] = 0x22, [0x06] = 0x2C, [0x07] = 0x2D,
    [0x08] = 0x2E, [0x09] = 0x2F, [0x0B] = 0x30, [0x0C] = 0x10, [0x0D] = 0x11, [0x0E] = 0x12, [0x0F] = 0x13, [0x10] = 0x15,
    [0x11] = 0x14, [0x12] = 0x02, [0x13] = 0x03, [0x14] = 0x04, [0x15] = 0x05, [0x16] = 0x07, [0x17] = 0x06, [0x18] = 0x0D,
    [0x19] = 0x0A, [0x1A] = 0x08, [0x1B] = 0x0C, [0x1C] = 0x09, [0x1D] = 0x0B, [0x1E] = 0x1B, [0x1F] = 0x18, [0x20] = 0x16,
    [0x21] = 0x1A, [0x22] = 0x17, [0x23] = 0x19, [0x24] = 0x1C, [0x25] = 0x26, [0x26] = 0x24, [0x27] = 0x28, [0x28] = 0x25,
    [0x29] = 0x27, [0x2A] = 0x2B, [0x2B] = 0x33, [0x2C] = 0x35, [0x2D] = 0x31, [0x2E] = 0x32, [0x2F] = 0x34, [0x30] = 0x0F,
    [0x31] = 0x39, [0x32] = 0x29, [0x33] = 0x0E, [0x35] = 0x01, [0x37] = 0xDB, [0x38] = 0x2A, [0x39] = 0x3A, [0x3A] = 0x38,
    [0x3B] = 0x1D, [0x3C] = 0x36, [0x3D] = 0xB8, [0x3E] = 0x9D, [0x41] = 0x53, [0x43] = 0x37, [0x45] = 0x4E, [0x4B] = 0xB5,
    [0x4C] = 0x9C, [0x4E] = 0x4A, [0x52] = 0x52, [0x53] = 0x4F, [0x54] = 0x50, [0x55] = 0x51, [0x56] = 0x4B, [0x57] = 0x4C,
    [0x58] = 0x4D, [0x59] = 0x47, [0x5B] = 0x48, [0x5C] = 0x49, [0x60] = 0x3F, [0x61] = 0x40, [0x62] = 0x41, [0x63] = 0x3D,
    [0x64] = 0x42, [0x65] = 0x43, [0x67] = 0x57, [0x6D] = 0x44, [0x6F] = 0x58, [0x72] = 0xD2, [0x73] = 0xC7, [0x74] = 0xC9,
    [0x75] = 0xD3, [0x76] = 0x3E, [0x77] = 0xCF, [0x78] = 0x3C, [0x79] = 0xD1, [0x7A] = 0x3B, [0x7B] = 0xCB, [0x7C] = 0xCD,
    [0x7D] = 0xD0, [0x7E] = 0xC8,
};

#ifndef FNV_PROBE
static NSString *exeHash(NSURL *url) {
    NSInputStream *stream = [NSInputStream inputStreamWithURL:url];
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    [stream open];
    uint8_t buffer[16384];
    NSInteger n;
    while ((n = [stream read:buffer maxLength:sizeof buffer]) > 0)
        CC_SHA256_Update(&hash, buffer, (CC_LONG)n);
    [stream close];
    if (n < 0) return nil;
    uint8_t bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(bytes, &hash);
    NSMutableString *result = [NSMutableString string];
    for (size_t i = 0; i < sizeof bytes; ++i) [result appendFormat:@"%02x", bytes[i]];
    return result;
}
#endif

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) FNVMetalView *metalView;
@property (nonatomic, strong) NSTextField *statusLabel;
@property (nonatomic, strong) NSTimer *padTimer;
@property (nonatomic) BOOL cursorCaptured;
@property (nonatomic) id eventMonitor;
@end

@implementation AppDelegate

- (void)setupMenus {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appMenuItem = [NSMenuItem new];
    [bar addItem:appMenuItem];

    NSMenu *appMenu = [NSMenu new];
    NSString *title = GAME_TITLE;
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:title]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:title]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    NSMenuItem *quitItem = [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:title]
                                              action:@selector(terminate:)
                                       keyEquivalent:@"q"];
    quitItem.target = NSApp;
    appMenuItem.submenu = appMenu;

    NSMenuItem *viewMenuItem = [NSMenuItem new];
    [bar addItem:viewMenuItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fsItem = [viewMenu addItemWithTitle:@"Toggle Full Screen"
                                             action:@selector(toggleFullScreen:)
                                      keyEquivalent:@"f"];
    fsItem.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    viewMenuItem.submenu = viewMenu;

    NSApp.mainMenu = bar;
}

- (void)setCursorCapture:(BOOL)capture {
    if (_cursorCaptured == capture) return;
    _cursorCaptured = capture;
    CGAssociateMouseAndMouseCursorPosition(!capture);
    if (capture) {
        [NSCursor hide];
    } else {
        [NSCursor unhide];
    }
}

- (void)releaseAllInputs {
    for (int i = 0; i < 8; ++i) input_mouse_button(i, 0);
    for (int dik = 1; dik < 256; ++dik) input_key(dik, 0);
    fnv_pad_reset();
}

- (void)handleEvent:(NSEvent *)e {
    switch (e.type) {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp: {
        if (e.modifierFlags & NSEventModifierFlagCommand) return;
        if (e.keyCode < 128 && mac2dik[e.keyCode]) {
            input_key(mac2dik[e.keyCode], e.type == NSEventTypeKeyDown);
        }
        return;
    }
    case NSEventTypeFlagsChanged: {
        NSEventModifierFlags f = e.modifierFlags;
        input_key(0x2A, (f & NSEventModifierFlagShift) != 0);
        input_key(0x1D, (f & NSEventModifierFlagControl) != 0);
        input_key(0x38, (f & NSEventModifierFlagOption) != 0);
        input_key(0x3A, (f & NSEventModifierFlagCapsLock) != 0);
        return;
    }
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        if (_cursorCaptured) {
            input_mouse_move((int)lround(e.deltaX), (int)lround(e.deltaY));
        }
        return;
    case NSEventTypeLeftMouseDown:
        if (!_cursorCaptured && self.window.isKeyWindow) [self setCursorCapture:YES];
        input_mouse_button(0, 1);
        return;
    case NSEventTypeLeftMouseUp:
        input_mouse_button(0, 0);
        return;
    case NSEventTypeRightMouseDown:
        input_mouse_button(1, 1);
        return;
    case NSEventTypeRightMouseUp:
        input_mouse_button(1, 0);
        return;
    case NSEventTypeOtherMouseDown:
        input_mouse_button((int)e.buttonNumber, 1);
        return;
    case NSEventTypeOtherMouseUp:
        input_mouse_button((int)e.buttonNumber, 0);
        return;
    case NSEventTypeScrollWheel: {
        CGFloat dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY * 10.0 : e.scrollingDeltaY * 120.0;
        input_mouse_wheel((int)lround(dy));
        return;
    }
    default:
        return;
    }
}

- (void)pollControllers {
    GCExtendedGamepad *pad = nil;
    for (GCController *controller in GCController.controllers) {
        if (controller.extendedGamepad) {
            pad = controller.extendedGamepad;
            break;
        }
    }
    FNVPadState state = {0};
    if (pad) {
        state.lx = (int16_t)lround(pad.leftThumbstick.xAxis.value * 32767.0);
        state.ly = (int16_t)lround(pad.leftThumbstick.yAxis.value * 32767.0);
        state.rx = (int16_t)lround(pad.rightThumbstick.xAxis.value * 32767.0);
        state.ry = (int16_t)lround(pad.rightThumbstick.yAxis.value * 32767.0);
        state.left_trigger = (uint8_t)lround(pad.leftTrigger.value * 255.0);
        state.right_trigger = (uint8_t)lround(pad.rightTrigger.value * 255.0);
        if (pad.dpad.up.isPressed) state.buttons |= 0x0001;
        if (pad.dpad.down.isPressed) state.buttons |= 0x0002;
        if (pad.dpad.left.isPressed) state.buttons |= 0x0004;
        if (pad.dpad.right.isPressed) state.buttons |= 0x0008;
        if (pad.buttonMenu.isPressed) state.buttons |= 0x0010;
        if (pad.buttonOptions.isPressed) state.buttons |= 0x0020;
        if (pad.leftThumbstickButton.isPressed) state.buttons |= 0x0040;
        if (pad.rightThumbstickButton.isPressed) state.buttons |= 0x0080;
        if (pad.leftShoulder.isPressed) state.buttons |= 0x0100;
        if (pad.rightShoulder.isPressed) state.buttons |= 0x0200;
        if (pad.buttonA.isPressed) state.buttons |= 0x1000;
        if (pad.buttonB.isPressed) state.buttons |= 0x2000;
        if (pad.buttonX.isPressed) state.buttons |= 0x4000;
        if (pad.buttonY.isPressed) state.buttons |= 0x8000;
    }
    fnv_pad_physical_state(&state, pad != nil);
}

- (void)showStatus:(NSString *)message {
    self.statusLabel.stringValue = message ?: @"";
    self.statusLabel.hidden = (message == nil || message.length == 0);
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    [self setupMenus];

    NSScreen *screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    CGFloat scale = screen ? screen.backingScaleFactor : 2.0;
    NSRect screenFrame = screen ? screen.frame : NSMakeRect(0, 0, 1280, 720);
    unsigned nativeW = (unsigned)llround(screenFrame.size.width * scale);
    unsigned nativeH = (unsigned)llround(screenFrame.size.height * scale);
    fnv_display_configure(nativeW, nativeH);

    NSRect vis = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1280, 720);
    CGFloat w = fnv_display_width() / scale;
    CGFloat h = fnv_display_height() / scale;
    CGFloat fit = MIN(0.88, MIN(vis.size.width / w, (vis.size.height - 28.0) / h));
    NSRect winRect = NSMakeRect(0, 0, floor(w * fit), floor(h * fit));

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:winRect
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = GAME_TITLE;
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.backgroundColor = NSColor.blackColor;

    self.metalView = [[FNVMetalView alloc] initWithFrame:winRect];
    self.metalView.wantsLayer = YES;
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.window.contentView = self.metalView;
    screenLayer = (CAMetalLayer *)self.metalView.layer;
    screenLayer.contentsScale = scale;

    self.statusLabel = [NSTextField labelWithString:@""];
    self.statusLabel.textColor = NSColor.whiteColor;
    self.statusLabel.backgroundColor = [NSColor.blackColor colorWithAlphaComponent:0.7];
    self.statusLabel.drawsBackground = YES;
    self.statusLabel.font = [NSFont systemFontOfSize:14];
    self.statusLabel.lineBreakMode = NSLineBreakByWordWrapping;
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.hidden = YES;
    [self.metalView addSubview:self.statusLabel];
    [NSLayoutConstraint activateConstraints:@[
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.metalView.leadingAnchor constant:16],
        [self.statusLabel.topAnchor constraintEqualToAnchor:self.metalView.topAnchor constant:16],
        [self.statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.metalView.trailingAnchor constant:-16],
    ]];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.metalView];
    [NSApp activateIgnoringOtherApps:YES];
    if (!getenv("FNV_WINDOWED")) {
        [self.window toggleFullScreen:nil];
    }

    __weak typeof(self) weakSelf = self;
    self.eventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:
        (NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged |
         NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
         NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
         NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskScrollWheel)
        handler:^NSEvent *(NSEvent *event) {
            AppDelegate *strongSelf = weakSelf;
            if (!strongSelf) return event;
            if (event.window == strongSelf.window || event.type == NSEventTypeFlagsChanged) {
                if (strongSelf.window.isKeyWindow) {
                    [strongSelf handleEvent:event];
                    if ((event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp) &&
                        !(event.modifierFlags & NSEventModifierFlagCommand)) {
                        return nil; // suppress system beep for game keys
                    }
                }
            }
            return event;
        }];

    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { (void)n; [weakSelf pollControllers]; }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { (void)n; [weakSelf pollControllers]; }];
    self.padTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0
                                                     repeats:YES
                                                       block:^(NSTimer *t) { (void)t; [weakSelf pollControllers]; }];
    [self pollControllers];

    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *libURL = [fm URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *logDir = [libURL URLByAppendingPathComponent:[@"Logs/" stringByAppendingString:GAME_DIR] isDirectory:YES];
    [fm createDirectoryAtURL:logDir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSURL *logURL = [logDir URLByAppendingPathComponent:GAME_LOG];
    if (!getenv("FNV_STDERR") && !isatty(STDERR_FILENO)) {
        freopen(logURL.fileSystemRepresentation, "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    NSString *revision = [NSBundle.mainBundle objectForInfoDictionaryKey:@"FNVBuildSourceCommit"] ?: @"unknown";
    NSString *buildVersion = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"unknown";
    fprintf(stderr, "[build] source=%s version=%s\n", revision.UTF8String, buildVersion.UTF8String);

#ifdef FNV_PROBE
    [self showStatus:@"Input / Metal Probe — Move mouse for R/G, W/S or controller for Blue. Cmd+Q to quit."];
#else
    NSURL *appSupport = [fm URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *userDir = [appSupport URLByAppendingPathComponent:[GAME_DIR stringByAppendingString:@"/UserDocuments"] isDirectory:YES];
    [fm createDirectoryAtURL:userDir withIntermediateDirectories:YES attributes:nil error:NULL];
    setenv("FNV_USER_DIR", userDir.fileSystemRepresentation, 1);

    NSURL *root = nil;
    const char *envRoot = getenv("FNV_ROOT");
    if (envRoot && envRoot[0]) {
        root = [NSURL fileURLWithPath:@(envRoot) isDirectory:YES];
    } else {
        NSURL *bundleGameData = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"GameData" isDirectory:YES];
        if ([fm fileExistsAtPath:[bundleGameData URLByAppendingPathComponent:GAME_ESM].path] &&
            [fm fileExistsAtPath:[bundleGameData URLByAppendingPathComponent:GAME_EXE].path]) {
            root = bundleGameData;
        } else {
            root = [NSURL fileURLWithPath:fm.currentDirectoryPath isDirectory:YES];
        }
    }

    NSURL *exe = [root URLByAppendingPathComponent:GAME_EXE];
    if (![fm fileExistsAtPath:[root URLByAppendingPathComponent:GAME_ESM].path] ||
        ![fm fileExistsAtPath:exe.path]) {
        [self showStatus:[NSString stringWithFormat:@"Missing game files. Expected %@ and %@ in:\n%@", GAME_EXE, GAME_ESM, root.path]];
        return;
    }
    if (chdir(root.fileSystemRepresentation)) {
        [self showStatus:@"Could not open the game directory."];
        return;
    }

    NSString *expected = [NSBundle.mainBundle objectForInfoDictionaryKey:@"FNVExecutableSHA256"];
    if (expected.length > 0 && ![expected isEqualToString:exeHash(exe)]) {
        [self showStatus:[NSString stringWithFormat:@"%@ does not match this build.", GAME_EXE]];
        return;
    }

    setenv("FNV_ROOT", root.fileSystemRepresentation, 1);
    setenv("FNV_FALLBACK_LOG", [logDir URLByAppendingPathComponent:@"fallback_entries.txt"].fileSystemRepresentation, 1);

#ifdef FNV_GAME_FALLOUT3
    NSString *localAppDir = [userDir.path stringByAppendingPathComponent:@"LocalAppData/Fallout3"];
    [fm createDirectoryAtPath:localAppDir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *pluginsPath = [localAppDir stringByAppendingPathComponent:@"plugins.txt"];
    if (![fm fileExistsAtPath:pluginsPath]) {
        NSMutableArray<NSString *> *plugins = [NSMutableArray arrayWithObject:@"Fallout3.esm"];
        NSString *dlcListPath = [root.path stringByAppendingPathComponent:@"Data/DLCList.txt"];
        NSString *dlcList = [NSString stringWithContentsOfFile:dlcListPath encoding:NSUTF8StringEncoding error:NULL];
        if (dlcList) {
            for (NSString *raw in [dlcList componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
                NSString *esm = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                if (esm.length > 0 && ![plugins containsObject:esm] &&
                    [fm fileExistsAtPath:[root.path stringByAppendingPathComponent:[@"Data" stringByAppendingPathComponent:esm]]]) {
                    [plugins addObject:esm];
                }
            }
        }
        [[plugins componentsJoinedByString:@"\r\n"] writeToFile:pluginsPath atomically:YES encoding:NSISOLatin1StringEncoding error:NULL];
    }
#endif

    // Update display/controller keys in FalloutPrefs.ini while preserving all other user settings.
    NSString *myGamesDir = [userDir.path stringByAppendingPathComponent:[@"My Games/" stringByAppendingString:GAME_DIR]];
    NSString *prefs = [myGamesDir stringByAppendingPathComponent:@"FalloutPrefs.ini"];
    for (NSString *leaf in [fm contentsOfDirectoryAtPath:myGamesDir error:NULL]) {
        if ([leaf caseInsensitiveCompare:@"FalloutPrefs.ini"] == NSOrderedSame) {
            prefs = [myGamesDir stringByAppendingPathComponent:leaf];
            break;
        }
    }
    NSString *text = [NSString stringWithContentsOfFile:prefs encoding:NSISOLatin1StringEncoding error:NULL] ?: @"";
    if ([fm fileExistsAtPath:prefs] && ![fm fileExistsAtPath:[prefs stringByAppendingString:@".before-native-display"]]) {
        [fm copyItemAtPath:prefs toPath:[prefs stringByAppendingString:@".before-native-display"] error:NULL];
    }
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] mutableCopy];
    NSDictionary *settings = @{
        @"display": @{
            @"isize w": @[@"iSize W", @(fnv_display_width())],
            @"isize h": @[@"iSize H", @(fnv_display_height())],
            @"bfull screen": @[@"bFull Screen", @1],
            @"sd3ddevice": @[@"sD3DDevice", @(fnv_display_adapter_description())],
        },
        @"controls": @{
            @"buse joystick": @[@"bUse Joystick", @1],
        },
    };
    for (NSString *section in settings) {
        NSDictionary *keys = settings[section];
        NSMutableSet *pending = [NSMutableSet setWithArray:keys.allKeys];
        BOOL inside = NO;
        NSInteger insert = -1;
        for (NSUInteger i = 0; i < lines.count; i++) {
            NSString *line = [lines[i] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if ([line hasPrefix:@"["] && [line hasSuffix:@"]"]) {
                if (inside && insert < 0) insert = (NSInteger)i;
                inside = [[[line substringWithRange:NSMakeRange(1, line.length - 2)] lowercaseString] isEqualToString:section];
            } else if (inside) {
                NSRange equal = [line rangeOfString:@"="];
                if (equal.location != NSNotFound) {
                    NSString *key = [[[line substringToIndex:equal.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] lowercaseString];
                    NSArray *value = keys[key];
                    if (value) {
                        lines[i] = [NSString stringWithFormat:@"%@=%@", value[0], value[1]];
                        [pending removeObject:key];
                    }
                }
            }
        }
        if (insert < 0) {
            insert = (NSInteger)lines.count;
            if (!inside) {
                [lines addObject:[NSString stringWithFormat:@"[%@]", section]];
                insert++;
            }
        }
        for (NSString *key in pending) {
            NSArray *value = keys[key];
            [lines insertObject:[NSString stringWithFormat:@"%@=%@", value[0], value[1]] atIndex:(NSUInteger)insert++];
        }
    }
    // Without Fallout3Launcher.exe, Fallout3.exe initializes missing INIPrefSetting LOD
    // multipliers to 0.0000. Dividing by fLODFadeOutMultActors=0 produces +inf in the
    // actor fade check (0xad2960), culling every NPC and starving Actor::Update/dialogue.
    NSDictionary *lodDefaults = @{
        @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"5.0000"],
        @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"3.0000"],
        @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"6.0000"],
    };
    {
        NSMutableSet *pending = [NSMutableSet setWithArray:lodDefaults.allKeys];
        BOOL inside = NO;
        NSInteger insert = -1;
        for (NSUInteger i = 0; i < lines.count; i++) {
            NSString *line = [lines[i] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if ([line hasPrefix:@"["] && [line hasSuffix:@"]"]) {
                if (inside && insert < 0) insert = (NSInteger)i;
                inside = [[[line substringWithRange:NSMakeRange(1, line.length - 2)] lowercaseString] isEqualToString:@"lod"];
            } else if (inside) {
                NSRange equal = [line rangeOfString:@"="];
                if (equal.location != NSNotFound) {
                    NSString *key = [[[line substringToIndex:equal.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] lowercaseString];
                    NSArray *def = lodDefaults[key];
                    if (def) {
                        double curVal = [[[line substringFromIndex:equal.location + 1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] doubleValue];
                        if (!(curVal > 0.0)) {
                            lines[i] = [NSString stringWithFormat:@"%@=%@", def[0], def[1]];
                        }
                        [pending removeObject:key];
                    }
                }
            }
        }
        if (pending.count > 0) {
            if (insert < 0) {
                insert = (NSInteger)lines.count;
                if (!inside) {
                    [lines addObject:@"[LOD]"];
                    insert++;
                }
            }
            for (NSString *key in pending) {
                NSArray *def = lodDefaults[key];
                [lines insertObject:[NSString stringWithFormat:@"%@=%@", def[0], def[1]] atIndex:(NSUInteger)insert++];
            }
        }
    }

    [fm createDirectoryAtPath:prefs.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL];
    NSError *prefsError = nil;
    BOOL prefsOK = [[lines componentsJoinedByString:@"\n"] writeToFile:prefs atomically:YES encoding:NSISOLatin1StringEncoding error:&prefsError];
    fprintf(stderr, "[display] native %ux%u; user=%s; preferences=%s\n",
            fnv_display_width(), fnv_display_height(), userDir.fileSystemRepresentation,
            prefsOK ? "written" : prefsError.localizedDescription.UTF8String);

    fnv_audio_start();
#endif

    [self setCursorCapture:YES];

    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            fnv_wait_active();
#ifdef FNV_PROBE
            extern int fnv_probe(unsigned);
            int rc = fnv_probe(0);
#else
            extern int fnv_run_game(int, char **);
            char *argv[] = {GAME_ARGV0, GAME_EXE_C, NULL};
            int rc = fnv_run_game(2, argv);
#endif
            __block NSString *failureDetail = nil;
            if (rc) {
                NSString *diagnostic = [NSString stringWithContentsOfURL:logURL encoding:NSUTF8StringEncoding error:NULL];
                [diagnostic enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
                    if ([line hasPrefix:@"[host] TRAP"]) { failureDetail = line; *stop = YES; }
                }];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [self setCursorCapture:NO];
                if (rc == 0) {
                    [NSApp terminate:nil];
                } else {
                    [self showStatus:failureDetail ?: [NSString stringWithFormat:@"Host returned %d. See %@.", rc, logURL.path]];
                }
            });
        }
    }];
    thread.stackSize = 16 * 1024 * 1024;
    [thread start];
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    [self setCursorCapture:YES];
#ifndef FNV_PROBE
    fnv_media_active(1);
#endif
    setActive(YES);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    [self setCursorCapture:NO];
    [self releaseAllInputs];
}

- (void)applicationWillResignActive:(NSNotification *)notification {
    (void)notification;
    [self setCursorCapture:NO];
    [self releaseAllInputs];
#ifndef FNV_PROBE
    fnv_media_active(0);
#endif
}

- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    if (self.window.isKeyWindow) [self setCursorCapture:YES];
#ifndef FNV_PROBE
    fnv_media_active(1);
#endif
    setActive(YES);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self setCursorCapture:NO];
    if (self.eventMonitor) {
        [NSEvent removeMonitor:self.eventMonitor];
        self.eventMonitor = nil;
    }
}

@end

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [AppDelegate new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
