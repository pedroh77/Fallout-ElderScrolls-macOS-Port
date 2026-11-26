#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <GameController/GameController.h>
#import <CommonCrypto/CommonDigest.h>
#import "FNVGameView.h"
#import "FNVOptions.h"
#import "FNVSurface.h"
#import "FNVPad.h"
#include "FNVDisplay.h"
#include <IOKit/graphics/IOGraphicsTypes.h>
#ifndef FNV_PROBE
#include "FNVNativeMedia.h"
#endif
#include "input.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

static CAMetalLayer *screenLayer;
static CGSize lockedDrawableSize;

extern void fnv_display_configure_scaled(unsigned out_w, unsigned out_h, unsigned scale_pct);
extern unsigned fnv_output_width(void);
extern unsigned fnv_output_height(void);
extern void fnv_display_set_refresh(int hz);
extern void fnv_graphics_configure(int upscaler, int aa);

@interface FNVMetalView : NSView
@end
@implementation FNVMetalView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)applyLockedDrawableSize {
    if (lockedDrawableSize.width > 0 && lockedDrawableSize.height > 0 &&
        [self.layer isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer *ml = (CAMetalLayer *)self.layer;
        if (!CGSizeEqualToSize(ml.drawableSize, lockedDrawableSize)) {
            ml.drawableSize = lockedDrawableSize;
        }
    }
}
- (void)layout {
    [super layout];
    [self applyLockedDrawableSize];
}
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self applyLockedDrawableSize];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self applyLockedDrawableSize];
}
@end

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
        lockedDrawableSize = size;
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
        lockedDrawableSize = size;
        screenLayer.drawableSize = size;
    });
}

// Query the physical panel pixel dimensions for the target NSScreen even when
// macOS uses a scaled HiDPI backing buffer (e.g. 1440x900@2x = 2880x1800 on
// a 2560x1600 physical Retina panel). Supports FNV_OUTPUT_WIDTH/HEIGHT and
// SKYRIM_OUTPUT_WIDTH/HEIGHT.
static void detectPhysicalDisplayPixels(NSScreen *screen,
                                        unsigned *outPanelW, unsigned *outPanelH,
                                        unsigned *outBackingW, unsigned *outBackingH) {
    CGFloat scale = screen ? screen.backingScaleFactor : 2.0;
    NSRect screenFrame = screen ? screen.frame : NSMakeRect(0, 0, 1280, 720);
    unsigned backW = (unsigned)llround(screenFrame.size.width * scale);
    unsigned backH = (unsigned)llround(screenFrame.size.height * scale);
    if (backW < 640) backW = 1280;
    if (backH < 480) backH = 720;

    unsigned panelW = backW;
    unsigned panelH = backH;

    if (screen) {
        NSNumber *num = screen.deviceDescription[@"NSScreenNumber"];
        CGDirectDisplayID did = num ? (CGDirectDisplayID)num.unsignedIntValue : CGMainDisplayID();
        double curAspect = 0.0;
        CGDisplayModeRef curMode = CGDisplayCopyDisplayMode(did);
        if (curMode) {
            size_t cw = CGDisplayModeGetPixelWidth(curMode);
            size_t ch = CGDisplayModeGetPixelHeight(curMode);
            if (cw >= 640 && ch >= 480) curAspect = (double)cw / (double)ch;
            CGDisplayModeRelease(curMode);
        }

        NSDictionary *opts = @{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES};
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(did, (__bridge CFDictionaryRef)opts);
        if (modes) {
            uint64_t bestAspectPixels = 0;
            unsigned bestAspectW = 0, bestAspectH = 0;
            uint64_t bestAnyPixels = 0;
            unsigned bestAnyW = 0, bestAnyH = 0;

            CFIndex count = CFArrayGetCount(modes);
            for (CFIndex i = 0; i < count; i++) {
                CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
                if (!m || !(CGDisplayModeGetIOFlags(m) & kDisplayModeNativeFlag)) continue;
                size_t pw = CGDisplayModeGetPixelWidth(m);
                size_t ph = CGDisplayModeGetPixelHeight(m);
                if (pw < 640 || ph < 480 || pw > 16384 || ph > 16384) continue;
                uint64_t pixels = (uint64_t)pw * (uint64_t)ph;
                if (pixels > bestAnyPixels) {
                    bestAnyPixels = pixels;
                    bestAnyW = (unsigned)pw;
                    bestAnyH = (unsigned)ph;
                }
                if (curAspect > 0.0) {
                    double ma = (double)pw / (double)ph;
                    if (fabs(ma - curAspect) <= 0.05 && pixels > bestAspectPixels) {
                        bestAspectPixels = pixels;
                        bestAspectW = (unsigned)pw;
                        bestAspectH = (unsigned)ph;
                    }
                }
            }
            CFRelease(modes);

            if (bestAspectW > 0 && bestAspectH > 0) {
                panelW = bestAspectW;
                panelH = bestAspectH;
            } else if (bestAnyW > 0 && bestAnyH > 0) {
                panelW = bestAnyW;
                panelH = bestAnyH;
            }
        }
    }

    const char *envW = getenv("SKYRIM_OUTPUT_WIDTH");
    if (!envW || !envW[0]) envW = getenv("FNV_OUTPUT_WIDTH");
    const char *envH = getenv("SKYRIM_OUTPUT_HEIGHT");
    if (!envH || !envH[0]) envH = getenv("FNV_OUTPUT_HEIGHT");
    if (envW && envH) {
        unsigned ow = (unsigned)strtoul(envW, NULL, 10);
        unsigned oh = (unsigned)strtoul(envH, NULL, 10);
        if (ow >= 640 && ow <= 16384 && oh >= 480 && oh <= 16384) {
            panelW = ow;
            panelH = oh;
        }
    }

    if (outPanelW) *outPanelW = panelW;
    if (outPanelH) *outPanelH = panelH;
    if (outBackingW) *outBackingW = backW;
    if (outBackingH) *outBackingH = backH;
}

#ifndef FNV_PROBE
// The camera housing (notch) covers a strip at the top of the built-in display. Falling back to
// macOS's compatibility mode ("Scale to fit below built-in camera") letterboxes the whole app,
// which also shrinks the width and leaves bars on three sides. Rendering at the safe-area aspect
// keeps the native width: the game fills everything below the housing and only that strip is black.
// Fallout's HUD sits at the bottom, so it keeps the whole panel; Skyrim's compass is top-center and
// would end up behind the housing, so it reserves the strip by default. FNV_NOTCH_SAFE=0/1 overrides.
static BOOL notchSafeWanted(void) {
    const char *env = getenv("FNV_NOTCH_SAFE");
    if (env && env[0]) return env[0] != '0';
#ifdef FNV_GAME_SKYRIM
    return YES;
#else
    return NO;
#endif
}

// An explicit output resolution (FNV_OUTPUT_WIDTH/HEIGHT) is a deliberate choice; leave it alone.
static BOOL outputSizeOverridden(void) {
    const char *w = getenv("SKYRIM_OUTPUT_WIDTH");
    if (!w || !w[0]) w = getenv("FNV_OUTPUT_WIDTH");
    const char *h = getenv("SKYRIM_OUTPUT_HEIGHT");
    if (!h || !h[0]) h = getenv("FNV_OUTPUT_HEIGHT");
    return (w && w[0]) || (h && h[0]);
}

// Height of the camera housing in points: the screen's safe-area inset, or FNV_NOTCH_SIM_PX
// (pixels) on Macs without a notch, so the layout can be exercised and screenshotted everywhere.
static CGFloat notchTopInsetPoints(NSScreen *screen) {
    if (!screen) return 0;
    const char *sim = getenv("FNV_NOTCH_SIM_PX");
    if (sim && sim[0]) {
        double px = strtod(sim, NULL);
        CGFloat scale = screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 2.0;
        return px > 0 ? (CGFloat)(px / scale) : 0;
    }
    CGFloat top = 0;
    if (@available(macOS 12.0, *)) top = screen.safeAreaInsets.top;
    if (!(top >= 4)) return 0;
    return ceil(MIN(top, (CGFloat)80));
}
#endif

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
void FNVApplyIniSettings(NSMutableArray<NSString *> *lines,
                         NSDictionary<NSString *, NSDictionary *> *settings) {
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
        if (pending.count > 0) {
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
    }
}

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

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    double _mouseAccumX;
    double _mouseAccumY;
}
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) FNVMetalView *metalView;
@property (nonatomic, strong) FNVGameView *gameContainer;
@property (nonatomic, strong) NSTextField *statusLabel;
@property (nonatomic, strong) NSTimer *padTimer;
@property (nonatomic) BOOL cursorCaptured;
@property (nonatomic) id eventMonitor;
#ifndef FNV_PROBE
@property (nonatomic, strong) FNVOptionsController *optionsController;
@property (nonatomic, strong) NSWindow *optionsWindow;
@property (nonatomic) unsigned nativeW;
@property (nonatomic) unsigned nativeH;
@property (nonatomic) CGFloat screenScale;
@property (nonatomic, strong) NSURL *logDir;
@property (nonatomic, strong) NSURL *logURL;
@property (nonatomic) BOOL gameLaunched;
#endif
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
#ifndef FNV_PROBE
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *optItem = [appMenu addItemWithTitle:@"Show Options on Next Launch"
                                             action:@selector(enableOptionsOnNextLaunch:)
                                      keyEquivalent:@""];
    optItem.target = self;
#endif
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
    _mouseAccumX = 0.0;
    _mouseAccumY = 0.0;
    CGAssociateMouseAndMouseCursorPosition(!capture);
    if (capture) {
        [NSCursor hide];
    } else {
        [NSCursor unhide];
    }
}

- (void)releaseAllInputs {
    _mouseAccumX = 0.0;
    _mouseAccumY = 0.0;
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
            _mouseAccumX += (double)e.deltaX;
            _mouseAccumY += (double)e.deltaY;
            int dx = (int)trunc(_mouseAccumX);
            int dy = (int)trunc(_mouseAccumY);
            if (dx != 0 || dy != 0) {
                _mouseAccumX -= (double)dx;
                _mouseAccumY -= (double)dy;
                input_mouse_move(dx, dy);
            }
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

#ifndef FNV_PROBE
- (void)enableOptionsOnNextLaunch:(id)sender {
    (void)sender;
    [self.optionsController enableOptionsOnNextLaunch];
}
#endif

- (void)setupGameWindowWithFullscreen:(BOOL)fullscreen scale:(CGFloat)scale screen:(NSScreen *)screen {
    NSRect vis = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1280, 720);
    unsigned outW = fnv_output_width() ?: fnv_display_width();
    unsigned outH = fnv_output_height() ?: fnv_display_height();
    CGFloat w = outW / scale;
    CGFloat h = outH / scale;
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
    mainWindow = self.window;

    self.gameContainer = [[FNVGameView alloc] initWithFrame:winRect];
    self.gameContainer.contentPixelSize = CGSizeMake(outW, outH);
    self.metalView = [[FNVMetalView alloc] initWithFrame:self.gameContainer.bounds];
    self.metalView.wantsLayer = YES;
    self.gameContainer.gameView = self.metalView;
    [self.gameContainer addSubview:self.metalView];
    [self.gameContainer layoutGameView];
    self.window.contentView = self.gameContainer;
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

    const char *headlessEnv = getenv("FNV_HEADLESS");
    BOOL isHeadless = (headlessEnv && headlessEnv[0] && headlessEnv[0] != '0');
    if (fullscreen && !isHeadless) {
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
                    // Let macOS Metal Performance HUD shortcuts (Shift+Fn+F9 / Shift+Fn+F7)
                    // pass through to the system instead of swallowing them or triggering F9.
                    if ((event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp) &&
                        (event.modifierFlags & NSEventModifierFlagShift) &&
                        (event.keyCode == 0x65 || event.keyCode == 0x62)) {
                        return event;
                    }
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
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= 4096) ? 4096 : rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    [self setupMenus];

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

    NSScreen *screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    CGFloat scale = screen ? screen.backingScaleFactor : 2.0;
    unsigned nativeW = 0, nativeH = 0, backingW = 0, backingH = 0;
    detectPhysicalDisplayPixels(screen, &nativeW, &nativeH, &backingW, &backingH);
    int hz = (screen && [screen respondsToSelector:@selector(maximumFramesPerSecond)]) ? (int)screen.maximumFramesPerSecond : 60;
    if (hz <= 0) hz = 60;
    fnv_display_set_refresh(hz);
    fnv_display_configure(nativeW, nativeH);
    if (backingW != nativeW || backingH != nativeH) {
        fprintf(stderr, "[display] physical panel %ux%u (desktop backing %ux%u)\n",
                nativeW, nativeH, backingW, backingH);
    }

#ifdef FNV_PROBE
    fnv_graphics_configure(0, 0);
    [self setupGameWindowWithFullscreen:(getenv("FNV_WINDOWED") == NULL) scale:scale screen:screen];
    [self showStatus:@"Input / Metal Probe — Move mouse for R/G, W/S or controller for Blue. Cmd+Q to quit."];
    [self setCursorCapture:YES];
    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            fnv_wait_active();
            extern int fnv_probe(unsigned);
            int rc = fnv_probe(0);
            dispatch_async(dispatch_get_main_queue(), ^{
                [self setCursorCapture:NO];
                if (rc == 0) [NSApp terminate:nil];
                else [self showStatus:[NSString stringWithFormat:@"Host returned %d. See %@.", rc, logURL.path]];
            });
        }
    }];
    thread.stackSize = 16 * 1024 * 1024;
    [thread start];
#else
    self.nativeW = nativeW;
    self.nativeH = nativeH;
    self.screenScale = scale;
    self.logDir = logDir;
    self.logURL = logURL;
    self.optionsController = [[FNVOptionsController alloc] initWithNativeW:nativeW
                                                                   nativeH:nativeH
                                                                  backingW:backingW
                                                                  backingH:backingH
                                                                 refreshHz:hz];
    if ([self.optionsController shouldShowLauncher]) {
        __weak typeof(self) weakSelf = self;
        self.optionsWindow = [self.optionsController presentWindowWithDelegate:self
                                                                        onPlay:^(FNVLaunchSettings settings) {
            [weakSelf launchWithSettings:settings];
        }];
        return;
    }
    [self launchWithSettings:[self.optionsController currentLaunchSettings]];
#endif
}

#ifndef FNV_PROBE
- (void)launchWithSettings:(FNVLaunchSettings)settings {
    NSScreen *screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    CGFloat scale = self.screenScale ?: 2.0;

    // Reserve the camera housing strip by rendering a shorter frame at full width (see
    // notchSafeWanted). This applies to windowed launches too, so toggling fullscreen later keeps
    // the same image aspect and still leaves the strip black; only the view placement differs.
    unsigned panelW = self.nativeW, panelH = self.nativeH;
    CGFloat notchInset = 0;
    if (notchSafeWanted() && !outputSizeOverridden()) {
        CGFloat points = notchTopInsetPoints(screen);
        unsigned insetPx = points > 0 ? (unsigned)llround(points * scale) : 0;
        if (insetPx > 0 && panelH > insetPx + 480) {
            panelH -= insetPx;
            notchInset = points;
        }
    }
    if (notchInset > 0) {
        fprintf(stderr, "[display] camera housing: %.0fpt reserved at the top, output %ux%u\n",
                notchInset, panelW, panelH);
    }
    fnv_display_configure_scaled(panelW, panelH, settings.renderScale);
    fnv_graphics_configure(settings.upscaler, settings.antiAliasing);
    unsigned outW = fnv_output_width() ?: fnv_display_width();
    unsigned outH = fnv_output_height() ?: fnv_display_height();

    self.gameLaunched = YES;
    [self setupGameWindowWithFullscreen:settings.fullscreen scale:scale screen:screen];

    // Show the game window before dismissing the options window (so AppKit never sees
    // the last visible window close and terminates the app), and keep the options
    // window alive until this button action has returned before releasing it.
    if (self.optionsWindow) {
        NSWindow *optionsWindow = self.optionsWindow;
        self.optionsWindow = nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            [optionsWindow close];
        });
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *appSupport = [fm URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
#ifdef FNV_GAME_SKYRIM
    const char *envUserDir = getenv("SKYRIM_USER_DIR");
    if (!envUserDir || !envUserDir[0]) envUserDir = getenv("FNV_USER_DIR");
#else
    const char *envUserDir = getenv("FNV_USER_DIR");
#endif
    NSURL *userDir = (envUserDir && envUserDir[0])
        ? [NSURL fileURLWithPath:@(envUserDir) isDirectory:YES]
        : [appSupport URLByAppendingPathComponent:[GAME_DIR stringByAppendingString:@"/UserDocuments"] isDirectory:YES];
    [fm createDirectoryAtURL:userDir withIntermediateDirectories:YES attributes:nil error:NULL];
    setenv("FNV_USER_DIR", userDir.fileSystemRepresentation, 1);
#ifdef FNV_GAME_SKYRIM
    setenv("SKYRIM_USER_DIR", userDir.fileSystemRepresentation, 1);
#endif

    NSURL *root = nil;
#ifdef FNV_GAME_SKYRIM
    const char *envRoot = getenv("SKYRIM_ROOT");
    if (!envRoot || !envRoot[0]) envRoot = getenv("FNV_ROOT");
#else
    const char *envRoot = getenv("FNV_ROOT");
#endif
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
#ifdef FNV_GAME_SKYRIM
    setenv("SKYRIM_ROOT", root.fileSystemRepresentation, 1);
#endif
    setenv("FNV_FALLBACK_LOG", [self.logDir URLByAppendingPathComponent:@"fallback_entries.txt"].fileSystemRepresentation, 1);

    [self.optionsController prepareGameFilesInRoot:root userDir:userDir outW:outW outH:outH];

    fnv_audio_start();

    [self setCursorCapture:YES];

    NSURL *errLogURL = self.logURL;
    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            fnv_wait_active();
            extern int fnv_run_game(int, char **);
            char *argv[] = {GAME_ARGV0, GAME_EXE_C, NULL};
            int rc = fnv_run_game(2, argv);
            __block NSString *failureDetail = nil;
            if (rc) {
                NSString *diagnostic = [NSString stringWithContentsOfURL:errLogURL encoding:NSUTF8StringEncoding error:NULL];
                [diagnostic enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
                    if ([line hasPrefix:@"[host] TRAP"]) { failureDetail = line; *stop = YES; }
                }];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [self setCursorCapture:NO];
                if (rc == 0) {
                    [NSApp terminate:nil];
                } else {
                    [self showStatus:failureDetail ?: [NSString stringWithFormat:@"Host returned %d. See %@.", rc, errLogURL.path]];
                }
            });
        }
    }];
    thread.stackSize = 16 * 1024 * 1024;
    [thread start];
}
#endif

// Fullscreen keeps the image aspect-exact and bottom-anchored, so whatever height AppKit does not
// give the content (the camera-housing strip, when it is not already excluded) stays black.
- (void)windowDidEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    self.gameContainer.fullscreenMode = YES;
}

- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;
    self.gameContainer.fullscreenMode = NO;
}

- (void)windowWillClose:(NSNotification *)notification {
#ifndef FNV_PROBE
    if (notification.object == self.optionsWindow && !self.gameLaunched) {
        [NSApp terminate:nil];
    }
#else
    (void)notification;
#endif
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    if (notification.object != self.window) return;
    [self setCursorCapture:YES];
#ifndef FNV_PROBE
    fnv_media_active(1);
#endif
    setActive(YES);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    if (notification.object != self.window) return;
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
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"ApplePersistenceIgnoreState"];
        [NSApplication sharedApplication];
        const char *headless = getenv("FNV_HEADLESS");
        [NSApp setActivationPolicy:(headless && headless[0] && headless[0] != '0')
            ? NSApplicationActivationPolicyProhibited
            : NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [AppDelegate new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
