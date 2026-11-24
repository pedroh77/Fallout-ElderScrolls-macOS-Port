#import "FNVOptions.h"

#ifndef FNV_PROBE

#include "FNVDisplay.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *const kPrefQualityPreset = @"SkyrimQualityPreset";
static NSString *const kPrefTextureQuality = @"SkyrimTextureQuality";
static NSString *const kPrefAnisotropy = @"SkyrimAnisotropy";
static NSString *const kPrefWaterReflections = @"SkyrimWaterReflections";
static NSString *const kPrefRenderScale = @"SkyrimRenderScale";
static NSString *const kPrefUpscaler = @"SkyrimUpscaler";
static NSString *const kPrefAntiAliasing = @"SkyrimAntiAliasing";
static NSString *const kPrefShadowQuality = @"SkyrimShadowQuality";
static NSString *const kPrefFovPreset = @"SkyrimFovPreset";
static NSString *const kPrefWorldFov = @"SkyrimWorldFov";
static NSString *const kPrefFirstPersonFov = @"SkyrimFirstPersonFov";
static NSString *const kPrefFullscreen = @"SkyrimFullscreen";
static NSString *const kPrefMouseAccel = @"SkyrimMouseAcceleration";
static NSString *const kPrefSkipLauncher = @"SkyrimSkipLauncher";

typedef struct {
    int preset;          // 0=Keep SkyrimPrefs.ini, 1=Low, 2=Medium, 3=High, 4=Ultra
    int textureQuality;  // 0=High (skip 0), 1=Medium (skip 1), 2=Low (skip 2)
    int anisotropy;      // 0, 2, 4, 8, 16
    int waterQuality;    // 0=Off, 1=Low (Land), 2=Medium (Land+Trees), 3=High (All)
    unsigned renderScale;
    int upscaler;
    int antiAliasing;
    int shadowQuality;   // 0=Keep, 1=Low (512), 2=Medium (1024), 3=High (2048), 4=Ultra (4096)
    int fovPreset;       // 0=Keep Skyrim.ini, 1=Original, 2=Expanded, 3=Wide, 4=Custom
    int worldFov;        // 65..105 (fDefaultWorldFOV / fDefaultFOV)
    int firstPersonFov;  // 65..95  (fDefault1stPersonFOV)
    BOOL fullscreen;
    BOOL mouseAcceleration;
    BOOL skipLauncher;
} SkyrimOptions;

static int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void applySkyrimFovPresetDefaults(SkyrimOptions *opts, int fovPreset) {
    switch (fovPreset) {
    case 2: // Expanded
        opts->worldFov = 85; opts->firstPersonFov = 80;
        break;
    case 3: // Wide
        opts->worldFov = 95; opts->firstPersonFov = 85;
        break;
    case 1: // Original
    default:
        opts->worldFov = 65; opts->firstPersonFov = 65;
        break;
    }
}

static int matchingSkyrimFovPreset(int worldFov, int firstPersonFov) {
    if (worldFov == 65 && firstPersonFov == 65) return 1;
    if (worldFov == 85 && firstPersonFov == 80) return 2;
    if (worldFov == 95 && firstPersonFov == 85) return 3;
    return 4;
}

static void applySkyrimPresetDefaults(SkyrimOptions *opts, int preset) {
    switch (preset) {
    case 1: // Low
        opts->textureQuality = 2; opts->anisotropy = 0; opts->shadowQuality = 1; opts->waterQuality = 0;
        break;
    case 2: // Medium
        opts->textureQuality = 1; opts->anisotropy = 4; opts->shadowQuality = 2; opts->waterQuality = 1;
        break;
    case 3: // High
        opts->textureQuality = 0; opts->anisotropy = 8; opts->shadowQuality = 3; opts->waterQuality = 2;
        break;
    case 4: // Ultra
    default:
        opts->textureQuality = 0; opts->anisotropy = 16; opts->shadowQuality = 4; opts->waterQuality = 3;
        break;
    }
}

static unsigned clampScale(long v) {
    if (v < 50) return 50;
    if (v > 100) return 100;
    return (unsigned)v;
}

static SkyrimOptions loadSkyrimOptions(unsigned nativeW) {
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    SkyrimOptions opts;
    id pObj = [ud objectForKey:kPrefQualityPreset];
    opts.preset = pObj ? (int)[pObj integerValue] : 3; // High default
    if (opts.preset < 0 || opts.preset > 4) opts.preset = 3;
    applySkyrimPresetDefaults(&opts, opts.preset > 0 ? opts.preset : 3);
    if (opts.preset == 0) opts.shadowQuality = 0;

    id tObj = [ud objectForKey:kPrefTextureQuality];
    if (tObj) { opts.textureQuality = (int)[tObj integerValue]; if (opts.textureQuality < 0 || opts.textureQuality > 2) opts.textureQuality = 0; }
    id anObj = [ud objectForKey:kPrefAnisotropy];
    if (anObj) { opts.anisotropy = (int)[anObj integerValue]; if (opts.anisotropy < 0 || opts.anisotropy > 16) opts.anisotropy = 8; }
    id wObj = [ud objectForKey:kPrefWaterReflections];
    if (wObj) { opts.waterQuality = (int)[wObj integerValue]; if (opts.waterQuality < 0 || opts.waterQuality > 3) opts.waterQuality = 2; }

    unsigned defScale = (nativeW >= 2000u) ? 75u : 100u;
    id sObj = [ud objectForKey:kPrefRenderScale];
    opts.renderScale = sObj ? clampScale([sObj integerValue]) : defScale;
    id uObj = [ud objectForKey:kPrefUpscaler];
    opts.upscaler = uObj ? ([uObj integerValue] ? 1 : 0) : 1;
    id aObj = [ud objectForKey:kPrefAntiAliasing];
    opts.antiAliasing = aObj ? (int)[aObj integerValue] : 0;
    if (opts.antiAliasing < 0 || opts.antiAliasing > 2) opts.antiAliasing = 0;
    id shObj = [ud objectForKey:kPrefShadowQuality];
    if (shObj) { opts.shadowQuality = (int)[shObj integerValue]; if (opts.shadowQuality < 0 || opts.shadowQuality > 4) opts.shadowQuality = 3; }
    id fpObj = [ud objectForKey:kPrefFovPreset];
    opts.fovPreset = fpObj ? (int)[fpObj integerValue] : 1; // Original default
    if (opts.fovPreset < 0 || opts.fovPreset > 4) opts.fovPreset = 1;
    applySkyrimFovPresetDefaults(&opts, (opts.fovPreset >= 1 && opts.fovPreset <= 3) ? opts.fovPreset : 1);
    id wfObj = [ud objectForKey:kPrefWorldFov];
    if (wfObj) opts.worldFov = clampInt((int)[wfObj integerValue], 65, 105);
    id fpfObj = [ud objectForKey:kPrefFirstPersonFov];
    if (fpfObj) opts.firstPersonFov = clampInt((int)[fpfObj integerValue], 65, 95);
    if (opts.fovPreset >= 1 && opts.fovPreset <= 3) {
        applySkyrimFovPresetDefaults(&opts, opts.fovPreset);
    } else if (opts.fovPreset == 4) {
        opts.fovPreset = matchingSkyrimFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    id fsObj = [ud objectForKey:kPrefFullscreen];
    opts.fullscreen = fsObj ? [fsObj boolValue] : YES;
    id maObj = [ud objectForKey:kPrefMouseAccel];
    opts.mouseAcceleration = maObj ? [maObj boolValue] : NO;
    id skObj = [ud objectForKey:kPrefSkipLauncher];
    opts.skipLauncher = skObj ? [skObj boolValue] : NO;

    const char *envScale = getenv("SKYRIM_RENDER_SCALE");
    if (envScale && envScale[0]) opts.renderScale = clampScale(strtol(envScale, NULL, 10));
    const char *envUp = getenv("SKYRIM_UPSCALER");
    if (envUp && envUp[0]) {
        if (!strcasecmp(envUp, "bilinear") || !strcmp(envUp, "0")) opts.upscaler = 0;
        else if (!strcasecmp(envUp, "metalfx") || !strcasecmp(envUp, "metalfx-spatial") || !strcmp(envUp, "1")) opts.upscaler = 1;
    }
    const char *envAA = getenv("SKYRIM_AA");
    if (envAA && envAA[0]) {
        if (!strcasecmp(envAA, "smaa") || !strcmp(envAA, "1")) opts.antiAliasing = 1;
        else if (!strcasecmp(envAA, "fxaa") || !strcmp(envAA, "2")) opts.antiAliasing = 2;
        else if (!strcasecmp(envAA, "off") || !strcasecmp(envAA, "none") || !strcmp(envAA, "0")) opts.antiAliasing = 0;
    }
    const char *envSh = getenv("SKYRIM_SHADOWS");
    if (envSh && envSh[0]) {
        if (!strcasecmp(envSh, "low") || !strcmp(envSh, "1")) opts.shadowQuality = 1;
        else if (!strcasecmp(envSh, "medium") || !strcmp(envSh, "2")) opts.shadowQuality = 2;
        else if (!strcasecmp(envSh, "high") || !strcmp(envSh, "3")) opts.shadowQuality = 3;
        else if (!strcasecmp(envSh, "ultra") || !strcmp(envSh, "4")) opts.shadowQuality = 4;
        else if (!strcasecmp(envSh, "keep") || !strcmp(envSh, "0")) opts.shadowQuality = 0;
    }
    const char *envFov = getenv("SKYRIM_FOV");
    if (envFov && envFov[0]) {
        opts.worldFov = clampInt((int)strtol(envFov, NULL, 10), 65, 105);
        opts.fovPreset = matchingSkyrimFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    const char *envFpFov = getenv("SKYRIM_1ST_PERSON_FOV");
    if (envFpFov && envFpFov[0]) {
        opts.firstPersonFov = clampInt((int)strtol(envFpFov, NULL, 10), 65, 95);
        opts.fovPreset = matchingSkyrimFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    const char *envMA = getenv("SKYRIM_MOUSE_ACCEL");
    if (envMA && envMA[0]) opts.mouseAcceleration = (atoi(envMA) != 0);
    if (getenv("FNV_WINDOWED")) opts.fullscreen = NO;
    return opts;
}

static void saveSkyrimOptions(SkyrimOptions opts) {
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    [ud setInteger:opts.preset forKey:kPrefQualityPreset];
    [ud setInteger:opts.textureQuality forKey:kPrefTextureQuality];
    [ud setInteger:opts.anisotropy forKey:kPrefAnisotropy];
    [ud setInteger:opts.waterQuality forKey:kPrefWaterReflections];
    [ud setInteger:(NSInteger)opts.renderScale forKey:kPrefRenderScale];
    [ud setInteger:opts.upscaler forKey:kPrefUpscaler];
    [ud setInteger:opts.antiAliasing forKey:kPrefAntiAliasing];
    [ud setInteger:opts.shadowQuality forKey:kPrefShadowQuality];
    [ud setInteger:opts.fovPreset forKey:kPrefFovPreset];
    [ud setInteger:opts.worldFov forKey:kPrefWorldFov];
    [ud setInteger:opts.firstPersonFov forKey:kPrefFirstPersonFov];
    [ud setBool:opts.fullscreen forKey:kPrefFullscreen];
    [ud setBool:opts.mouseAcceleration forKey:kPrefMouseAccel];
    [ud setBool:opts.skipLauncher forKey:kPrefSkipLauncher];
    [ud synchronize];
}

@interface FNVOptionsController ()
@property (nonatomic) unsigned nativeW;
@property (nonatomic) unsigned nativeH;
@property (nonatomic) unsigned backingW;
@property (nonatomic) unsigned backingH;
@property (nonatomic) int refreshHz;
@property (nonatomic) SkyrimOptions savedOpts;
@property (nonatomic) BOOL hasSavedOpts;
@property (nonatomic, copy) void (^onPlay)(FNVLaunchSettings settings);

@property (nonatomic, strong) NSWindow *optionsWindow;
@property (nonatomic, strong) NSPopUpButton *presetPopup;
@property (nonatomic, strong) NSPopUpButton *texturePopup;
@property (nonatomic, strong) NSPopUpButton *anisoPopup;
@property (nonatomic, strong) NSPopUpButton *waterPopup;
@property (nonatomic, strong) NSPopUpButton *scalePopup;
@property (nonatomic, strong) NSPopUpButton *upscalerPopup;
@property (nonatomic, strong) NSPopUpButton *aaPopup;
@property (nonatomic, strong) NSPopUpButton *shadowPopup;
@property (nonatomic, strong) NSPopUpButton *displayPopup;
@property (nonatomic, strong) NSPopUpButton *fovPresetPopup;
@property (nonatomic, strong) NSSlider *worldFovSlider;
@property (nonatomic, strong) NSSlider *firstPersonFovSlider;
@property (nonatomic, strong) NSTextField *worldFovValueLabel;
@property (nonatomic, strong) NSTextField *firstPersonFovValueLabel;
@property (nonatomic, strong) NSTextField *fovTipLabel;
@property (nonatomic, strong) NSTextField *resPreviewLabel;
@property (nonatomic, strong) NSButton *mouseAccelCheck;
@property (nonatomic, strong) NSButton *skipLauncherCheck;
@end

@implementation FNVOptionsController

- (instancetype)initWithNativeW:(unsigned)nativeW
                        nativeH:(unsigned)nativeH
                       backingW:(unsigned)backingW
                       backingH:(unsigned)backingH
                      refreshHz:(int)refreshHz {
    self = [super init];
    if (self) {
        _nativeW = nativeW;
        _nativeH = nativeH;
        _backingW = backingW;
        _backingH = backingH;
        _refreshHz = refreshHz;
    }
    return self;
}

- (BOOL)shouldShowLauncher {
    SkyrimOptions opts = loadSkyrimOptions(self.nativeW);
    const char *headlessEnv = getenv("FNV_HEADLESS");
    BOOL isHeadless = (headlessEnv && headlessEnv[0] && headlessEnv[0] != '0');
    BOOL optionHeld = (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0;
    return !isHeadless && !getenv("SKYRIM_SKIP_LAUNCHER") && !getenv("FNV_SKIP_LAUNCHER") && (!opts.skipLauncher || optionHeld);
}

- (void)enableOptionsOnNextLaunch {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:kPrefSkipLauncher];
    [NSUserDefaults.standardUserDefaults synchronize];
}

- (FNVLaunchSettings)currentLaunchSettings {
    SkyrimOptions opts = loadSkyrimOptions(self.nativeW);
    self.savedOpts = opts;
    self.hasSavedOpts = YES;
    return (FNVLaunchSettings){opts.renderScale, opts.upscaler, opts.antiAliasing, opts.fullscreen};
}

- (unsigned)selectedRenderScale {
    NSInteger tag = self.scalePopup.selectedItem.tag;
    return tag > 0 ? clampScale(tag) : 100u;
}

- (void)updateOptionsPreview {
    unsigned pct = [self selectedRenderScale];
    unsigned ow = ((self.nativeW / 2u) * 2u); if (ow < 640u) ow = 640u;
    unsigned oh = ((self.nativeH / 2u) * 2u); if (oh < 480u) oh = 480u;
    unsigned rw = ((ow * pct / 100u) / 2u) * 2u; if (rw < 640u) rw = 640u; if (rw > ow) rw = ow;
    unsigned rh = ((oh * pct / 100u) / 2u) * 2u; if (rh < 480u) rh = 480u; if (rh > oh) rh = oh;
    if (pct == 100u) {
        self.resPreviewLabel.stringValue = [NSString stringWithFormat:@"Internal: %ux%u (native, upscaler bypassed)", rw, rh];
        self.upscalerPopup.enabled = NO;
    } else {
        self.resPreviewLabel.stringValue = [NSString stringWithFormat:@"Internal: %ux%u -> Output: %ux%u", rw, rh, ow, oh];
        self.upscalerPopup.enabled = YES;
    }
}

- (void)updateSkyrimDetailControlsEnabled {
    BOOL enable = (self.presetPopup.selectedItem.tag != 0);
    self.texturePopup.enabled = enable;
    self.anisoPopup.enabled = enable;
    self.waterPopup.enabled = enable;
}

- (void)presetChanged:(id)sender {
    (void)sender;
    int p = (int)self.presetPopup.selectedItem.tag;
    if (p > 0) {
        SkyrimOptions tmp = {0};
        applySkyrimPresetDefaults(&tmp, p);
        [self.texturePopup selectItemWithTag:tmp.textureQuality];
        [self.anisoPopup selectItemWithTag:tmp.anisotropy];
        [self.shadowPopup selectItemWithTag:tmp.shadowQuality];
        [self.waterPopup selectItemWithTag:tmp.waterQuality];
    } else {
        [self.shadowPopup selectItemWithTag:0];
    }
    [self updateSkyrimDetailControlsEnabled];
}

- (void)scaleChanged:(id)sender {
    (void)sender;
    [self updateOptionsPreview];
}

- (void)updateFovUI {
    int fp = (int)self.fovPresetPopup.selectedItem.tag;
    int wf = clampInt((int)llround(self.worldFovSlider.doubleValue), 65, 105);
    int fpf = clampInt((int)llround(self.firstPersonFovSlider.doubleValue), 65, 95);
    BOOL enable = (fp != 0);
    self.worldFovSlider.enabled = enable;
    self.firstPersonFovSlider.enabled = enable;
    self.worldFovValueLabel.stringValue = [NSString stringWithFormat:@"%d°", wf];
    self.firstPersonFovValueLabel.stringValue = [NSString stringWithFormat:@"%d°", fpf];
    self.worldFovValueLabel.textColor = enable ? NSColor.labelColor : NSColor.tertiaryLabelColor;
    self.firstPersonFovValueLabel.textColor = enable ? NSColor.labelColor : NSColor.tertiaryLabelColor;
    if (!enable) {
        self.fovTipLabel.stringValue = @"Keeping existing FOV settings in Skyrim.ini.";
    } else if (wf == 65 && fpf == 65) {
        self.fovTipLabel.stringValue = @"Vanilla Skyrim defaults (Skyrim.ini).";
    } else if (fpf > 85) {
        self.fovTipLabel.stringValue = [NSString stringWithFormat:@">85° 1st-person may clip arms/bows. Saves: \"fov %d\" in console (~).", wf];
    } else {
        self.fovTipLabel.stringValue = [NSString stringWithFormat:@"New games apply automatically; existing saves: \"fov %d\" in console (~).", wf];
    }
}

- (void)fovPresetChanged:(id)sender {
    (void)sender;
    int fp = (int)self.fovPresetPopup.selectedItem.tag;
    if (fp >= 1 && fp <= 3) {
        SkyrimOptions tmp = {0};
        applySkyrimFovPresetDefaults(&tmp, fp);
        self.worldFovSlider.integerValue = tmp.worldFov;
        self.firstPersonFovSlider.integerValue = tmp.firstPersonFov;
    }
    [self updateFovUI];
}

- (void)fovSliderChanged:(id)sender {
    (void)sender;
    int wf = clampInt((int)llround(self.worldFovSlider.doubleValue), 65, 105);
    int fpf = clampInt((int)llround(self.firstPersonFovSlider.doubleValue), 65, 95);
    self.worldFovSlider.integerValue = wf;
    self.firstPersonFovSlider.integerValue = fpf;
    [self.fovPresetPopup selectItemWithTag:matchingSkyrimFovPreset(wf, fpf)];
    [self updateFovUI];
}

- (void)quitClicked:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}

- (void)playClicked:(id)sender {
    (void)sender;
    SkyrimOptions opts;
    opts.preset = (int)self.presetPopup.selectedItem.tag;
    opts.textureQuality = (int)self.texturePopup.selectedItem.tag;
    opts.anisotropy = (int)self.anisoPopup.selectedItem.tag;
    opts.waterQuality = (int)self.waterPopup.selectedItem.tag;
    opts.renderScale = [self selectedRenderScale];
    opts.upscaler = (int)self.upscalerPopup.selectedItem.tag;
    opts.antiAliasing = (int)self.aaPopup.selectedItem.tag;
    opts.shadowQuality = (int)self.shadowPopup.selectedItem.tag;
    opts.fovPreset = (int)self.fovPresetPopup.selectedItem.tag;
    opts.worldFov = clampInt((int)llround(self.worldFovSlider.doubleValue), 65, 105);
    opts.firstPersonFov = clampInt((int)llround(self.firstPersonFovSlider.doubleValue), 65, 95);
    opts.fullscreen = (self.displayPopup.selectedItem.tag != 0);
    opts.mouseAcceleration = (self.mouseAccelCheck.state != NSControlStateValueOn);
    opts.skipLauncher = (self.skipLauncherCheck.state == NSControlStateValueOn);
    saveSkyrimOptions(opts);
    // Re-apply env overrides if caller explicitly passed them
    SkyrimOptions launchOpts = loadSkyrimOptions(self.nativeW);
    launchOpts.preset = opts.preset;
    launchOpts.textureQuality = opts.textureQuality;
    launchOpts.anisotropy = opts.anisotropy;
    launchOpts.waterQuality = opts.waterQuality;
    launchOpts.renderScale = getenv("SKYRIM_RENDER_SCALE") ? launchOpts.renderScale : opts.renderScale;
    launchOpts.upscaler = getenv("SKYRIM_UPSCALER") ? launchOpts.upscaler : opts.upscaler;
    launchOpts.antiAliasing = getenv("SKYRIM_AA") ? launchOpts.antiAliasing : opts.antiAliasing;
    launchOpts.shadowQuality = getenv("SKYRIM_SHADOWS") ? launchOpts.shadowQuality : opts.shadowQuality;
    BOOL envHasFov = (getenv("SKYRIM_FOV") && getenv("SKYRIM_FOV")[0]);
    BOOL envHasFpFov = (getenv("SKYRIM_1ST_PERSON_FOV") && getenv("SKYRIM_1ST_PERSON_FOV")[0]);
    launchOpts.worldFov = envHasFov ? launchOpts.worldFov : opts.worldFov;
    launchOpts.firstPersonFov = envHasFpFov ? launchOpts.firstPersonFov : opts.firstPersonFov;
    launchOpts.fovPreset = (envHasFov || envHasFpFov) ? matchingSkyrimFovPreset(launchOpts.worldFov, launchOpts.firstPersonFov) : opts.fovPreset;
    launchOpts.fullscreen = getenv("FNV_WINDOWED") ? NO : opts.fullscreen;
    launchOpts.mouseAcceleration = getenv("SKYRIM_MOUSE_ACCEL") ? launchOpts.mouseAcceleration : opts.mouseAcceleration;

    self.savedOpts = launchOpts;
    self.hasSavedOpts = YES;
    if (self.onPlay) {
        self.onPlay((FNVLaunchSettings){launchOpts.renderScale, launchOpts.upscaler, launchOpts.antiAliasing, launchOpts.fullscreen});
    }
}

- (NSWindow *)presentWindowWithDelegate:(id<NSWindowDelegate>)delegate
                                 onPlay:(void (^)(FNVLaunchSettings settings))onPlay {
    self.onPlay = onPlay;
    SkyrimOptions opts = loadSkyrimOptions(self.nativeW);

    NSRect rect = NSMakeRect(0, 0, 480, 622);
    self.optionsWindow = [[NSWindow alloc] initWithContentRect:rect
                                                     styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    self.optionsWindow.title = [NSString stringWithFormat:@"%@ — Options", GAME_TITLE];
    self.optionsWindow.delegate = delegate;
    // ARC owns this window through the strong property. Leaving the AppKit default
    // (releasedWhenClosed = YES) makes -close release a window ARC still owns, which
    // leaves a dangling window in the autorelease pool and crashes on the next drain.
    self.optionsWindow.releasedWhenClosed = NO;
    NSView *cv = self.optionsWindow.contentView;

    NSTextField *title = [NSTextField labelWithString:@"Graphics & Input Options"];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.frame = NSMakeRect(24, 586, 432, 22);
    [cv addSubview:title];

    NSString *dispStr = (self.backingW != self.nativeW || self.backingH != self.nativeH)
        ? [NSString stringWithFormat:@"Display: %ux%u panel (desktop %ux%u) @ %d Hz",
           self.nativeW, self.nativeH, self.backingW, self.backingH, self.refreshHz]
        : [NSString stringWithFormat:@"Display: %ux%u @ %d Hz",
           self.nativeW, self.nativeH, self.refreshHz];
    NSTextField *sub = [NSTextField labelWithString:dispStr];
    sub.font = [NSFont systemFontOfSize:12];
    sub.textColor = NSColor.secondaryLabelColor;
    sub.frame = NSMakeRect(24, 566, 432, 16);
    [cv addSubview:sub];

    NSBox *sep1 = [[NSBox alloc] initWithFrame:NSMakeRect(24, 557, 432, 1)];
    sep1.boxType = NSBoxSeparator;
    [cv addSubview:sep1];

    NSTextField *(^mkLabel)(NSString *, CGFloat) = ^NSTextField *(NSString *txt, CGFloat y) {
        NSTextField *l = [NSTextField labelWithString:txt];
        l.alignment = NSTextAlignmentRight;
        l.frame = NSMakeRect(24, y, 160, 20);
        [cv addSubview:l];
        return l;
    };
    NSPopUpButton *(^mkPopup)(CGFloat) = ^NSPopUpButton *(CGFloat y) {
        NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(196, y, 260, 26) pullsDown:NO];
        [cv addSubview:p];
        return p;
    };

    mkLabel(@"Quality Preset:", 523);
    self.presetPopup = mkPopup(521);
    [self.presetPopup addItemWithTitle:@"Ultra"]; self.presetPopup.lastItem.tag = 4;
    [self.presetPopup addItemWithTitle:@"High (Recommended)"]; self.presetPopup.lastItem.tag = 3;
    [self.presetPopup addItemWithTitle:@"Medium"]; self.presetPopup.lastItem.tag = 2;
    [self.presetPopup addItemWithTitle:@"Low"]; self.presetPopup.lastItem.tag = 1;
    [self.presetPopup addItemWithTitle:@"Keep SkyrimPrefs.ini"]; self.presetPopup.lastItem.tag = 0;
    [self.presetPopup selectItemWithTag:opts.preset];
    self.presetPopup.target = self;
    self.presetPopup.action = @selector(presetChanged:);

    mkLabel(@"Render Scale:", 492);
    self.scalePopup = mkPopup(490);
    struct { NSString *label; NSInteger tag; } scaleItems[] = {
        {@"100% (Native)", 100}, {@"85% (Quality)", 85}, {@"75% (Recommended)", 75},
        {@"67% (Balanced)", 67}, {@"50% (Performance)", 50},
    };
    for (size_t i = 0; i < sizeof scaleItems / sizeof scaleItems[0]; i++) {
        [self.scalePopup addItemWithTitle:scaleItems[i].label];
        self.scalePopup.lastItem.tag = scaleItems[i].tag;
    }
    if (![self.scalePopup selectItemWithTag:(NSInteger)opts.renderScale]) {
        NSString *custom = [NSString stringWithFormat:@"%u%% (Custom)", opts.renderScale];
        [self.scalePopup addItemWithTitle:custom];
        self.scalePopup.lastItem.tag = (NSInteger)opts.renderScale;
        [self.scalePopup selectItemWithTag:(NSInteger)opts.renderScale];
    }
    self.scalePopup.target = self;
    self.scalePopup.action = @selector(scaleChanged:);

    self.resPreviewLabel = [NSTextField labelWithString:@""];
    self.resPreviewLabel.font = [NSFont systemFontOfSize:11];
    self.resPreviewLabel.textColor = NSColor.secondaryLabelColor;
    self.resPreviewLabel.frame = NSMakeRect(198, 471, 258, 15);
    [cv addSubview:self.resPreviewLabel];

    mkLabel(@"Upscaler:", 443);
    self.upscalerPopup = mkPopup(441);
    [self.upscalerPopup addItemWithTitle:@"MetalFX Spatial"]; self.upscalerPopup.lastItem.tag = 1;
    [self.upscalerPopup addItemWithTitle:@"Bilinear"]; self.upscalerPopup.lastItem.tag = 0;
    [self.upscalerPopup selectItemWithTag:opts.upscaler];

    mkLabel(@"Anti-Aliasing:", 412);
    self.aaPopup = mkPopup(410);
    [self.aaPopup addItemWithTitle:@"Off"]; self.aaPopup.lastItem.tag = 0;
    [self.aaPopup addItemWithTitle:@"SMAA (Post-Process)"]; self.aaPopup.lastItem.tag = 1;
    [self.aaPopup addItemWithTitle:@"FXAA (Fast, Post-Process)"]; self.aaPopup.lastItem.tag = 2;
    [self.aaPopup selectItemWithTag:opts.antiAliasing];

    mkLabel(@"Texture Quality:", 381);
    self.texturePopup = mkPopup(379);
    [self.texturePopup addItemWithTitle:@"High"]; self.texturePopup.lastItem.tag = 0;
    [self.texturePopup addItemWithTitle:@"Medium"]; self.texturePopup.lastItem.tag = 1;
    [self.texturePopup addItemWithTitle:@"Low"]; self.texturePopup.lastItem.tag = 2;
    [self.texturePopup selectItemWithTag:opts.textureQuality];

    mkLabel(@"Anisotropic Filtering:", 350);
    self.anisoPopup = mkPopup(348);
    [self.anisoPopup addItemWithTitle:@"16x (Max)"]; self.anisoPopup.lastItem.tag = 16;
    [self.anisoPopup addItemWithTitle:@"8x"]; self.anisoPopup.lastItem.tag = 8;
    [self.anisoPopup addItemWithTitle:@"4x"]; self.anisoPopup.lastItem.tag = 4;
    [self.anisoPopup addItemWithTitle:@"2x"]; self.anisoPopup.lastItem.tag = 2;
    [self.anisoPopup addItemWithTitle:@"Off"]; self.anisoPopup.lastItem.tag = 0;
    if (![self.anisoPopup selectItemWithTag:opts.anisotropy]) [self.anisoPopup selectItemWithTag:8];

    mkLabel(@"Shadow Quality:", 319);
    self.shadowPopup = mkPopup(317);
    [self.shadowPopup addItemWithTitle:@"Keep SkyrimPrefs.ini"]; self.shadowPopup.lastItem.tag = 0;
    [self.shadowPopup addItemWithTitle:@"Low (512)"]; self.shadowPopup.lastItem.tag = 1;
    [self.shadowPopup addItemWithTitle:@"Medium (1024)"]; self.shadowPopup.lastItem.tag = 2;
    [self.shadowPopup addItemWithTitle:@"High (2048)"]; self.shadowPopup.lastItem.tag = 3;
    [self.shadowPopup addItemWithTitle:@"Ultra (4096)"]; self.shadowPopup.lastItem.tag = 4;
    [self.shadowPopup selectItemWithTag:opts.shadowQuality];

    mkLabel(@"Water Reflections:", 288);
    self.waterPopup = mkPopup(286);
    [self.waterPopup addItemWithTitle:@"High (Land, Trees, Objects, Sky)"]; self.waterPopup.lastItem.tag = 3;
    [self.waterPopup addItemWithTitle:@"Medium (Land & Trees)"]; self.waterPopup.lastItem.tag = 2;
    [self.waterPopup addItemWithTitle:@"Low (Land Only)"]; self.waterPopup.lastItem.tag = 1;
    [self.waterPopup addItemWithTitle:@"Off"]; self.waterPopup.lastItem.tag = 0;
    [self.waterPopup selectItemWithTag:opts.waterQuality];

    mkLabel(@"Display Mode:", 257);
    self.displayPopup = mkPopup(255);
    [self.displayPopup addItemWithTitle:@"Fullscreen"]; self.displayPopup.lastItem.tag = 1;
    [self.displayPopup addItemWithTitle:@"Windowed"]; self.displayPopup.lastItem.tag = 0;
    [self.displayPopup selectItemWithTag:opts.fullscreen ? 1 : 0];

    mkLabel(@"FOV Preset:", 226);
    self.fovPresetPopup = mkPopup(224);
    [self.fovPresetPopup addItemWithTitle:@"Original (65° / 65°)"]; self.fovPresetPopup.lastItem.tag = 1;
    [self.fovPresetPopup addItemWithTitle:@"Expanded (85° / 80°)"]; self.fovPresetPopup.lastItem.tag = 2;
    [self.fovPresetPopup addItemWithTitle:@"Wide (95° / 85°)"]; self.fovPresetPopup.lastItem.tag = 3;
    [self.fovPresetPopup addItemWithTitle:@"Custom"]; self.fovPresetPopup.lastItem.tag = 4;
    [self.fovPresetPopup addItemWithTitle:@"Keep Skyrim.ini"]; self.fovPresetPopup.lastItem.tag = 0;
    [self.fovPresetPopup selectItemWithTag:opts.fovPreset];
    self.fovPresetPopup.target = self;
    self.fovPresetPopup.action = @selector(fovPresetChanged:);

    mkLabel(@"World FOV:", 197);
    self.worldFovSlider = [NSSlider sliderWithValue:opts.worldFov minValue:65 maxValue:105 target:self action:@selector(fovSliderChanged:)];
    self.worldFovSlider.frame = NSMakeRect(196, 194, 214, 24);
    self.worldFovSlider.numberOfTickMarks = 9;
    self.worldFovSlider.allowsTickMarkValuesOnly = NO;
    self.worldFovSlider.continuous = YES;
    [cv addSubview:self.worldFovSlider];

    self.worldFovValueLabel = [NSTextField labelWithString:@""];
    self.worldFovValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.worldFovValueLabel.alignment = NSTextAlignmentRight;
    self.worldFovValueLabel.frame = NSMakeRect(414, 197, 42, 20);
    [cv addSubview:self.worldFovValueLabel];

    mkLabel(@"1st-Person FOV:", 169);
    self.firstPersonFovSlider = [NSSlider sliderWithValue:opts.firstPersonFov minValue:65 maxValue:95 target:self action:@selector(fovSliderChanged:)];
    self.firstPersonFovSlider.frame = NSMakeRect(196, 166, 214, 24);
    self.firstPersonFovSlider.numberOfTickMarks = 7;
    self.firstPersonFovSlider.allowsTickMarkValuesOnly = NO;
    self.firstPersonFovSlider.continuous = YES;
    [cv addSubview:self.firstPersonFovSlider];

    self.firstPersonFovValueLabel = [NSTextField labelWithString:@""];
    self.firstPersonFovValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.firstPersonFovValueLabel.alignment = NSTextAlignmentRight;
    self.firstPersonFovValueLabel.frame = NSMakeRect(414, 169, 42, 20);
    [cv addSubview:self.firstPersonFovValueLabel];

    self.fovTipLabel = [NSTextField labelWithString:@""];
    self.fovTipLabel.font = [NSFont systemFontOfSize:11];
    self.fovTipLabel.textColor = NSColor.secondaryLabelColor;
    self.fovTipLabel.lineBreakMode = NSLineBreakByWordWrapping;
    self.fovTipLabel.maximumNumberOfLines = 2;
    self.fovTipLabel.frame = NSMakeRect(198, 136, 258, 26);
    [cv addSubview:self.fovTipLabel];

    self.mouseAccelCheck = [NSButton checkboxWithTitle:@"Disable in-game mouse acceleration (smoother mouse)"
                                                target:nil action:nil];
    self.mouseAccelCheck.frame = NSMakeRect(24, 110, 432, 18);
    self.mouseAccelCheck.state = opts.mouseAcceleration ? NSControlStateValueOff : NSControlStateValueOn;
    [cv addSubview:self.mouseAccelCheck];

    NSTextField *mouseTip = [NSTextField labelWithString:@"Tip: for external mice, also disable Pointer Acceleration in System Settings → Mouse."];
    mouseTip.font = [NSFont systemFontOfSize:11];
    mouseTip.textColor = NSColor.secondaryLabelColor;
    mouseTip.frame = NSMakeRect(42, 92, 414, 15);
    [cv addSubview:mouseTip];

    self.skipLauncherCheck = [NSButton checkboxWithTitle:@"Skip this window on next launch (hold Option ⌥ to reopen)"
                                                  target:nil action:nil];
    self.skipLauncherCheck.frame = NSMakeRect(24, 68, 432, 18);
    self.skipLauncherCheck.state = opts.skipLauncher ? NSControlStateValueOn : NSControlStateValueOff;
    [cv addSubview:self.skipLauncherCheck];

    NSBox *sep2 = [[NSBox alloc] initWithFrame:NSMakeRect(24, 56, 432, 1)];
    sep2.boxType = NSBoxSeparator;
    [cv addSubview:sep2];

    NSButton *quitBtn = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quitClicked:)];
    quitBtn.frame = NSMakeRect(276, 16, 88, 30);
    [cv addSubview:quitBtn];

    NSButton *playBtn = [NSButton buttonWithTitle:@"Play" target:self action:@selector(playClicked:)];
    playBtn.frame = NSMakeRect(372, 16, 84, 30);
    playBtn.keyEquivalent = @"\r";
    [cv addSubview:playBtn];

    [self updateOptionsPreview];
    [self updateSkyrimDetailControlsEnabled];
    [self updateFovUI];
    [self.optionsWindow center];
    [self.optionsWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return self.optionsWindow;
}

- (void)prepareGameFilesInRoot:(NSURL *)root
                       userDir:(NSURL *)userDir
                          outW:(unsigned)outW
                          outH:(unsigned)outH {
    SkyrimOptions opts = self.hasSavedOpts ? self.savedOpts : loadSkyrimOptions(self.nativeW);
    NSFileManager *fm = NSFileManager.defaultManager;

    // Ensure LocalAppData/Skyrim/plugins.txt exists so official masters/DLCs load without SkyrimLauncher.
    NSString *localSkyrimDir = [userDir.path stringByAppendingPathComponent:@"LocalAppData/Skyrim"];
    [fm createDirectoryAtPath:localSkyrimDir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *pluginsTxt = [localSkyrimDir stringByAppendingPathComponent:@"plugins.txt"];
    if (![fm fileExistsAtPath:pluginsTxt]) {
        NSMutableArray<NSString *> *plugins = [NSMutableArray array];
        NSArray<NSString *> *knownOrder = @[
            @"Skyrim.esm", @"Update.esm", @"Dawnguard.esm", @"HearthFires.esm", @"Dragonborn.esm",
            @"HighResTexturePack01.esp", @"HighResTexturePack02.esp", @"HighResTexturePack03.esp",
        ];
        NSString *dataDir = [root URLByAppendingPathComponent:@"Data"].path;
        for (NSString *p in knownOrder) {
            if ([fm fileExistsAtPath:[dataDir stringByAppendingPathComponent:p]]) [plugins addObject:p];
        }
        if (plugins.count > 0) {
            [[[plugins componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"]
                writeToFile:pluginsTxt atomically:YES encoding:NSISOLatin1StringEncoding error:NULL];
        }
    }

    // Update display/controller keys in SkyrimPrefs.ini while preserving all other user settings.
    NSString *myGamesDir = [userDir.path stringByAppendingPathComponent:@"My Games/Skyrim"];
    [fm createDirectoryAtPath:myGamesDir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *prefs = [myGamesDir stringByAppendingPathComponent:@"SkyrimPrefs.ini"];
    for (NSString *leaf in [fm contentsOfDirectoryAtPath:myGamesDir error:NULL]) {
        if ([leaf caseInsensitiveCompare:@"SkyrimPrefs.ini"] == NSOrderedSame) {
            prefs = [myGamesDir stringByAppendingPathComponent:leaf];
            break;
        }
    }
    if (![fm fileExistsAtPath:prefs]) {
        NSString *defaultPrefs = [root URLByAppendingPathComponent:@"Skyrim/SkyrimPrefs.ini"].path;
        if ([fm fileExistsAtPath:defaultPrefs]) [fm copyItemAtPath:defaultPrefs toPath:prefs error:NULL];
    }
    NSString *skyrimIni = [myGamesDir stringByAppendingPathComponent:@"Skyrim.ini"];
    for (NSString *leaf in [fm contentsOfDirectoryAtPath:myGamesDir error:NULL]) {
        if ([leaf caseInsensitiveCompare:@"Skyrim.ini"] == NSOrderedSame) {
            skyrimIni = [myGamesDir stringByAppendingPathComponent:leaf];
            break;
        }
    }
    if (![fm fileExistsAtPath:skyrimIni]) {
        NSString *defaultIni = [root URLByAppendingPathComponent:@"Skyrim_default.ini"].path;
        if ([fm fileExistsAtPath:defaultIni]) [fm copyItemAtPath:defaultIni toPath:skyrimIni error:NULL];
    }
    if (opts.fovPreset != 0) {
        NSString *iniText = [NSString stringWithContentsOfFile:skyrimIni encoding:NSISOLatin1StringEncoding error:NULL] ?: @"";
        NSMutableArray<NSString *> *iniLines = [[iniText componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] mutableCopy];
        FNVApplyIniSettings(iniLines, @{
            @"display": @{
                @"fdefaultworldfov":     @[@"fDefaultWorldFOV",     [NSString stringWithFormat:@"%.4f", (double)opts.worldFov]],
                @"fdefault1stpersonfov": @[@"fDefault1stPersonFOV", [NSString stringWithFormat:@"%.4f", (double)opts.firstPersonFov]],
            },
        });
        [[iniLines componentsJoinedByString:@"\n"] writeToFile:skyrimIni atomically:YES encoding:NSISOLatin1StringEncoding error:NULL];
    }
    NSString *text = [NSString stringWithContentsOfFile:prefs encoding:NSISOLatin1StringEncoding error:NULL] ?: @"";
    if ([fm fileExistsAtPath:prefs] && ![fm fileExistsAtPath:[prefs stringByAppendingString:@".before-native-display"]]) {
        [fm copyItemAtPath:prefs toPath:[prefs stringByAppendingString:@".before-native-display"] error:NULL];
    }
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] mutableCopy];
    NSMutableDictionary *displaySettings = [@{
        @"isize w": @[@"iSize W", @(fnv_display_width())],
        @"isize h": @[@"iSize H", @(fnv_display_height())],
        @"bfull screen": @[@"bFull Screen", @1],
        @"sd3ddevice": @[@"sD3DDevice", @(fnv_display_adapter_description())],
    } mutableCopy];
    if (opts.shadowQuality == 1) {
        displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @512];
        displaySettings[@"ishadowmapresolutionprimary"] = @[@"iShadowMapResolutionPrimary", @512];
        displaySettings[@"ishadowmapresolutionsecondary"] = @[@"iShadowMapResolutionSecondary", @512];
        displaySettings[@"iblurdeferredshadowmask"] = @[@"iBlurDeferredShadowMask", @2];
        displaySettings[@"fshadowdistance"] = @[@"fShadowDistance", @"1500.0000"];
    } else if (opts.shadowQuality == 2) {
        displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @1024];
        displaySettings[@"ishadowmapresolutionprimary"] = @[@"iShadowMapResolutionPrimary", @1024];
        displaySettings[@"ishadowmapresolutionsecondary"] = @[@"iShadowMapResolutionSecondary", @512];
        displaySettings[@"iblurdeferredshadowmask"] = @[@"iBlurDeferredShadowMask", @3];
        displaySettings[@"fshadowdistance"] = @[@"fShadowDistance", @"2500.0000"];
    } else if (opts.shadowQuality == 3) {
        displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @2048];
        displaySettings[@"ishadowmapresolutionprimary"] = @[@"iShadowMapResolutionPrimary", @2048];
        displaySettings[@"ishadowmapresolutionsecondary"] = @[@"iShadowMapResolutionSecondary", @1024];
        displaySettings[@"iblurdeferredshadowmask"] = @[@"iBlurDeferredShadowMask", @5];
        displaySettings[@"fshadowdistance"] = @[@"fShadowDistance", @"4000.0000"];
    } else if (opts.shadowQuality == 4) {
        displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @4096];
        displaySettings[@"ishadowmapresolutionprimary"] = @[@"iShadowMapResolutionPrimary", @4096];
        displaySettings[@"ishadowmapresolutionsecondary"] = @[@"iShadowMapResolutionSecondary", @2048];
        displaySettings[@"iblurdeferredshadowmask"] = @[@"iBlurDeferredShadowMask", @3];
        displaySettings[@"fshadowdistance"] = @[@"fShadowDistance", @"8000.0000"];
    }
    NSMutableDictionary *settings = [@{
        @"display": displaySettings,
        @"main": @{
            @"bgamepadenable": @[@"bGamepadEnable", @1],
        },
        @"controls": @{
            @"bmouseacceleration": @[@"bMouseAcceleration", opts.mouseAcceleration ? @1 : @0],
        },
    } mutableCopy];
    if (opts.fovPreset != 0) {
        settings[@"general"] = @{
            @"fdefaultfov": @[@"fDefaultFOV", [NSString stringWithFormat:@"%.4f", (double)opts.worldFov]],
        };
    }
    if (opts.preset > 0) {
        displaySettings[@"itexmipmapskip"] = @[@"iTexMipMapSkip", @(opts.textureQuality)];
        displaySettings[@"imaxanisotropy"] = @[@"iMaxAnisotropy", @(opts.anisotropy)];
        displaySettings[@"bfloatpointrendertarget"] = @[@"bFloatPointRenderTarget", @0];
        settings[@"water"] = @{
            @"breflectlodland":    @[@"bReflectLODLand",    (opts.waterQuality >= 1) ? @1 : @0],
            @"breflectlodtrees":   @[@"bReflectLODTrees",   (opts.waterQuality >= 2) ? @1 : @0],
            @"breflectlodobjects": @[@"bReflectLODObjects", (opts.waterQuality >= 3) ? @1 : @0],
            @"breflectsky":        @[@"bReflectSky",        (opts.waterQuality >= 3) ? @1 : @0],
        };
        if (opts.preset == 1) {
            settings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"4.0000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"2.5000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"3.5000"],
            };
            settings[@"grass"] = @{@"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"2000.0000"]};
        } else if (opts.preset == 2) {
            settings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"6.0000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"4.0000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"5.0000"],
            };
            settings[@"grass"] = @{@"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"3500.0000"]};
        } else if (opts.preset == 3) {
            settings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"9.0000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"7.0000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"9.0000"],
            };
            settings[@"grass"] = @{@"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"5000.0000"]};
        } else {
            settings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"15.0000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"15.0000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"15.0000"],
            };
            settings[@"grass"] = @{@"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"7000.0000"]};
        }
    }
    FNVApplyIniSettings(lines, settings);
    [fm createDirectoryAtPath:prefs.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL];
    NSError *prefsError = nil;
    BOOL prefsOK = [[lines componentsJoinedByString:@"\n"] writeToFile:prefs atomically:YES encoding:NSISOLatin1StringEncoding error:&prefsError];
    NSString *fovLog = (opts.fovPreset == 0) ? @"keep" : [NSString stringWithFormat:@"%d/%d", opts.worldFov, opts.firstPersonFov];
    fprintf(stderr, "[display] render %ux%u -> output %ux%u (%u%%) @ %dHz; upscaler=%s; aa=%s; fov=%s; mouse_accel=%d; user=%s; preferences=%s\n",
            fnv_display_width(), fnv_display_height(), outW, outH, opts.renderScale, self.refreshHz,
            opts.upscaler == 1 ? "metalfx" : "bilinear",
            opts.antiAliasing == 1 ? "smaa" : (opts.antiAliasing == 2 ? "fxaa" : "off"),
            fovLog.UTF8String,
            opts.mouseAcceleration ? 1 : 0,
            userDir.fileSystemRepresentation,
            prefsOK ? "written" : prefsError.localizedDescription.UTF8String);
}

@end

#endif
