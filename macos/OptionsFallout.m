#import "FNVOptions.h"

#ifndef FNV_PROBE

#include "FNVDisplay.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *prefKey(NSString *suffix) {
    return [GAME_PREF_PREFIX stringByAppendingString:suffix];
}

typedef struct {
    int preset;          // 0=Keep FalloutPrefs.ini, 1=Low, 2=Medium, 3=High, 4=Ultra
    unsigned renderScale;// 50..100 (default 100 = Native)
    int upscaler;        // 0=Bilinear, 1=MetalFX Spatial
    int antiAliasing;    // 0=Off, 1=SMAA, 2=FXAA
    int textureSize;     // 0=Large (skip 0), 1=Medium (skip 1), 2=Small (skip 2)
    int anisotropy;      // 0, 2, 4, 8, 15
    int shadowQuality;   // 0=Off, 1=Medium (256), 2=High (512), 3=Ultra (1024)
    int waterQuality;    // 0=Low, 1=Medium, 2=High (512), 3=Ultra (1024)
    int screenEffects;   // 0=Off, 1=Bloom, 2=HDR, 3=HDR + Depth of Field
    int fovPreset;       // 0=Keep FALLOUT.INI, 1=Original, 2=Expanded, 3=Wide, 4=Custom
    int worldFov;        // 75..105 (fDefaultWorldFOV)
    int firstPersonFov;  // 55..75  (fDefault1stPersonFOV)
    BOOL fullscreen;
    BOOL disableMouseAccel;
    BOOL skipLauncher;
} FalloutOptions;

static int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void applyFovPresetDefaults(FalloutOptions *opts, int fovPreset) {
    switch (fovPreset) {
    case 2: // Expanded
        opts->worldFov = 90; opts->firstPersonFov = 60;
        break;
    case 3: // Wide
        opts->worldFov = 100; opts->firstPersonFov = 65;
        break;
    case 1: // Original
    default:
        opts->worldFov = 75; opts->firstPersonFov = 55;
        break;
    }
}

static int matchingFovPreset(int worldFov, int firstPersonFov) {
    if (worldFov == 75 && firstPersonFov == 55) return 1;
    if (worldFov == 90 && firstPersonFov == 60) return 2;
    if (worldFov == 100 && firstPersonFov == 65) return 3;
    return 4;
}

static void applyPresetDefaults(FalloutOptions *opts, int preset) {
    switch (preset) {
    case 1: // Low
        opts->textureSize = 1; opts->anisotropy = 0; opts->shadowQuality = 0;
        opts->waterQuality = 0; opts->screenEffects = 0;
        break;
    case 2: // Medium
        opts->textureSize = 0; opts->anisotropy = 2; opts->shadowQuality = 1;
        opts->waterQuality = 1; opts->screenEffects = 2;
        break;
    case 3: // High
        opts->textureSize = 0; opts->anisotropy = 8; opts->shadowQuality = 2;
        opts->waterQuality = 2; opts->screenEffects = 3;
        break;
    case 4: // Ultra
    default:
        opts->textureSize = 0; opts->anisotropy = 15; opts->shadowQuality = 3;
        opts->waterQuality = 3; opts->screenEffects = 3;
        break;
    }
}

static FalloutOptions loadFalloutOptions(void) {
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    FalloutOptions opts;
    id pObj = [ud objectForKey:prefKey(@"QualityPreset")];
    opts.preset = pObj ? (int)[pObj integerValue] : 4; // Ultra default
    if (opts.preset < 0 || opts.preset > 4) opts.preset = 4;
    applyPresetDefaults(&opts, opts.preset > 0 ? opts.preset : 4);

    id rsObj = [ud objectForKey:prefKey(@"RenderScale")];
    opts.renderScale = rsObj ? (unsigned)[rsObj integerValue] : 100u;
    if (opts.renderScale < 50u || opts.renderScale > 100u) opts.renderScale = 100u;
    id upObj = [ud objectForKey:prefKey(@"Upscaler")];
    opts.upscaler = upObj ? (int)[upObj integerValue] : 1;
    if (opts.upscaler < 0 || opts.upscaler > 1) opts.upscaler = 1;
    id aaObj = [ud objectForKey:prefKey(@"AntiAliasing")];
    opts.antiAliasing = aaObj ? (int)[aaObj integerValue] : 0;
    if (opts.antiAliasing < 0 || opts.antiAliasing > 2) opts.antiAliasing = 0;

    id tObj = [ud objectForKey:prefKey(@"TextureSize")];
    if (tObj) { opts.textureSize = (int)[tObj integerValue]; if (opts.textureSize < 0 || opts.textureSize > 2) opts.textureSize = 0; }
    id aObj = [ud objectForKey:prefKey(@"Anisotropy")];
    if (aObj) { opts.anisotropy = (int)[aObj integerValue]; if (opts.anisotropy < 0 || opts.anisotropy > 16) opts.anisotropy = 15; }
    id sObj = [ud objectForKey:prefKey(@"ShadowQuality")];
    if (sObj) { opts.shadowQuality = (int)[sObj integerValue]; if (opts.shadowQuality < 0 || opts.shadowQuality > 3) opts.shadowQuality = 3; }
    id wObj = [ud objectForKey:prefKey(@"WaterQuality")];
    if (wObj) { opts.waterQuality = (int)[wObj integerValue]; if (opts.waterQuality < 0 || opts.waterQuality > 3) opts.waterQuality = 3; }
    id eObj = [ud objectForKey:prefKey(@"ScreenEffects")];
    if (eObj) { opts.screenEffects = (int)[eObj integerValue]; if (opts.screenEffects < 0 || opts.screenEffects > 3) opts.screenEffects = 3; }
    id fpObj = [ud objectForKey:prefKey(@"FovPreset")];
    opts.fovPreset = fpObj ? (int)[fpObj integerValue] : 1; // Original default
    if (opts.fovPreset < 0 || opts.fovPreset > 4) opts.fovPreset = 1;
    applyFovPresetDefaults(&opts, (opts.fovPreset >= 1 && opts.fovPreset <= 3) ? opts.fovPreset : 1);
    id wfObj = [ud objectForKey:prefKey(@"WorldFov")];
    if (wfObj) opts.worldFov = clampInt((int)[wfObj integerValue], 75, 105);
    id fpfObj = [ud objectForKey:prefKey(@"FirstPersonFov")];
    if (fpfObj) opts.firstPersonFov = clampInt((int)[fpfObj integerValue], 55, 75);
    if (opts.fovPreset >= 1 && opts.fovPreset <= 3) {
        applyFovPresetDefaults(&opts, opts.fovPreset);
    } else if (opts.fovPreset == 4) {
        opts.fovPreset = matchingFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    id fsObj = [ud objectForKey:prefKey(@"Fullscreen")];
    opts.fullscreen = fsObj ? [fsObj boolValue] : YES;
    id maObj = [ud objectForKey:prefKey(@"DisableMouseAccel")];
    opts.disableMouseAccel = maObj ? [maObj boolValue] : YES;
    id skObj = [ud objectForKey:prefKey(@"SkipLauncher")];
    opts.skipLauncher = skObj ? [skObj boolValue] : NO;

    const char *envPreset = getenv("FNV_PRESET");
    if (envPreset && envPreset[0]) {
        if (!strcasecmp(envPreset, "keep") || !strcmp(envPreset, "0")) opts.preset = 0;
        else if (!strcasecmp(envPreset, "low") || !strcmp(envPreset, "1")) { opts.preset = 1; applyPresetDefaults(&opts, 1); }
        else if (!strcasecmp(envPreset, "medium") || !strcmp(envPreset, "2")) { opts.preset = 2; applyPresetDefaults(&opts, 2); }
        else if (!strcasecmp(envPreset, "high") || !strcmp(envPreset, "3")) { opts.preset = 3; applyPresetDefaults(&opts, 3); }
        else if (!strcasecmp(envPreset, "ultra") || !strcmp(envPreset, "4")) { opts.preset = 4; applyPresetDefaults(&opts, 4); }
    }
    const char *envScale = getenv("FNV_RENDER_SCALE");
    if (envScale && envScale[0]) {
        unsigned v = (unsigned)strtoul(envScale, NULL, 10);
        if (v >= 50u && v <= 100u) opts.renderScale = v;
    }
    const char *envUp = getenv("FNV_UPSCALER");
    if (envUp && envUp[0]) {
        if (!strcasecmp(envUp, "bilinear") || !strcmp(envUp, "0")) opts.upscaler = 0;
        else if (!strcasecmp(envUp, "metalfx") || !strcmp(envUp, "1")) opts.upscaler = 1;
    }
    const char *envAA = getenv("FNV_AA");
    if (envAA && envAA[0]) {
        if (!strcasecmp(envAA, "off") || !strcasecmp(envAA, "none") || !strcmp(envAA, "0")) opts.antiAliasing = 0;
        else if (!strcasecmp(envAA, "smaa") || !strcmp(envAA, "1")) opts.antiAliasing = 1;
        else if (!strcasecmp(envAA, "fxaa") || !strcmp(envAA, "2")) opts.antiAliasing = 2;
    }
    const char *envFov = getenv("FNV_FOV");
    if (envFov && envFov[0]) {
        opts.worldFov = clampInt((int)strtol(envFov, NULL, 10), 75, 105);
        opts.fovPreset = matchingFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    const char *envFpFov = getenv("FNV_1ST_PERSON_FOV");
    if (envFpFov && envFpFov[0]) {
        opts.firstPersonFov = clampInt((int)strtol(envFpFov, NULL, 10), 55, 75);
        opts.fovPreset = matchingFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    const char *envMA = getenv("FNV_MOUSE_ACCEL");
    if (envMA && envMA[0]) opts.disableMouseAccel = (atoi(envMA) == 0);
    if (getenv("FNV_WINDOWED")) opts.fullscreen = NO;
    return opts;
}

static void saveFalloutOptions(FalloutOptions opts) {
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    [ud setInteger:opts.preset forKey:prefKey(@"QualityPreset")];
    [ud setInteger:(NSInteger)opts.renderScale forKey:prefKey(@"RenderScale")];
    [ud setInteger:opts.upscaler forKey:prefKey(@"Upscaler")];
    [ud setInteger:opts.antiAliasing forKey:prefKey(@"AntiAliasing")];
    [ud setInteger:opts.textureSize forKey:prefKey(@"TextureSize")];
    [ud setInteger:opts.anisotropy forKey:prefKey(@"Anisotropy")];
    [ud setInteger:opts.shadowQuality forKey:prefKey(@"ShadowQuality")];
    [ud setInteger:opts.waterQuality forKey:prefKey(@"WaterQuality")];
    [ud setInteger:opts.screenEffects forKey:prefKey(@"ScreenEffects")];
    [ud setInteger:opts.fovPreset forKey:prefKey(@"FovPreset")];
    [ud setInteger:opts.worldFov forKey:prefKey(@"WorldFov")];
    [ud setInteger:opts.firstPersonFov forKey:prefKey(@"FirstPersonFov")];
    [ud setBool:opts.fullscreen forKey:prefKey(@"Fullscreen")];
    [ud setBool:opts.disableMouseAccel forKey:prefKey(@"DisableMouseAccel")];
    [ud setBool:opts.skipLauncher forKey:prefKey(@"SkipLauncher")];
    [ud synchronize];
}

@interface FNVOptionsController ()
@property (nonatomic) unsigned nativeW;
@property (nonatomic) unsigned nativeH;
@property (nonatomic) unsigned backingW;
@property (nonatomic) unsigned backingH;
@property (nonatomic) int refreshHz;
@property (nonatomic) FalloutOptions savedOpts;
@property (nonatomic) BOOL hasSavedOpts;
@property (nonatomic, copy) void (^onPlay)(FNVLaunchSettings settings);

@property (nonatomic, strong) NSWindow *optionsWindow;
@property (nonatomic, strong) NSPopUpButton *presetPopup;
@property (nonatomic, strong) NSPopUpButton *scalePopup;
@property (nonatomic, strong) NSPopUpButton *upscalerPopup;
@property (nonatomic, strong) NSPopUpButton *aaPopup;
@property (nonatomic, strong) NSTextField *resPreviewLabel;
@property (nonatomic, strong) NSPopUpButton *texturePopup;
@property (nonatomic, strong) NSPopUpButton *anisoPopup;
@property (nonatomic, strong) NSPopUpButton *shadowPopup;
@property (nonatomic, strong) NSPopUpButton *waterPopup;
@property (nonatomic, strong) NSPopUpButton *effectsPopup;
@property (nonatomic, strong) NSPopUpButton *displayPopup;
@property (nonatomic, strong) NSPopUpButton *fovPresetPopup;
@property (nonatomic, strong) NSSlider *worldFovSlider;
@property (nonatomic, strong) NSSlider *firstPersonFovSlider;
@property (nonatomic, strong) NSTextField *worldFovValueLabel;
@property (nonatomic, strong) NSTextField *firstPersonFovValueLabel;
@property (nonatomic, strong) NSTextField *fovTipLabel;
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
    FalloutOptions opts = loadFalloutOptions();
    const char *headlessEnv = getenv("FNV_HEADLESS");
    BOOL isHeadless = (headlessEnv && headlessEnv[0] && headlessEnv[0] != '0');
    BOOL optionHeld = (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0;
    return !isHeadless && !getenv("FNV_SKIP_LAUNCHER") && (!opts.skipLauncher || optionHeld);
}

- (void)enableOptionsOnNextLaunch {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:prefKey(@"SkipLauncher")];
    [NSUserDefaults.standardUserDefaults synchronize];
}

- (FNVLaunchSettings)currentLaunchSettings {
    FalloutOptions opts = loadFalloutOptions();
    self.savedOpts = opts;
    self.hasSavedOpts = YES;
    return (FNVLaunchSettings){opts.renderScale, opts.upscaler, opts.antiAliasing, opts.fullscreen};
}

- (void)updateDetailControlsEnabled {
    BOOL enable = (self.presetPopup.selectedItem.tag != 0);
    self.texturePopup.enabled = enable;
    self.anisoPopup.enabled = enable;
    self.shadowPopup.enabled = enable;
    self.waterPopup.enabled = enable;
    self.effectsPopup.enabled = enable;
}

- (void)presetChanged:(id)sender {
    (void)sender;
    int p = (int)self.presetPopup.selectedItem.tag;
    if (p > 0) {
        FalloutOptions tmp = {0};
        applyPresetDefaults(&tmp, p);
        [self.texturePopup selectItemWithTag:tmp.textureSize];
        [self.anisoPopup selectItemWithTag:tmp.anisotropy];
        [self.shadowPopup selectItemWithTag:tmp.shadowQuality];
        [self.waterPopup selectItemWithTag:tmp.waterQuality];
        [self.effectsPopup selectItemWithTag:tmp.screenEffects];
    }
    [self updateDetailControlsEnabled];
}

- (void)updateResolutionPreview {
    unsigned pct = (unsigned)self.scalePopup.selectedItem.tag;
    if (pct < 50u) pct = 50u;
    if (pct > 100u) pct = 100u;
    unsigned rw = self.nativeW, rh = self.nativeH;
    if (pct < 100u) {
        rw = ((self.nativeW * pct / 100u) + 1u) & ~1u;
        rh = ((self.nativeH * pct / 100u) + 1u) & ~1u;
        if (rw < 640u) rw = 640u;
        if (rh < 480u) rh = 480u;
    }
    BOOL scaled = (rw != self.nativeW || rh != self.nativeH);
    self.upscalerPopup.enabled = scaled;
    if (scaled) {
        NSString *up = (self.upscalerPopup.selectedItem.tag == 1) ? @"MetalFX" : @"Bilinear";
        self.resPreviewLabel.stringValue = [NSString stringWithFormat:@"Render: %ux%u → %ux%u (%@)",
                                            rw, rh, self.nativeW, self.nativeH, up];
    } else {
        self.resPreviewLabel.stringValue = [NSString stringWithFormat:@"Render: %ux%u (Native)",
                                            self.nativeW, self.nativeH];
    }
}

- (void)scaleChanged:(id)sender { (void)sender; [self updateResolutionPreview]; }
- (void)upscalerChanged:(id)sender { (void)sender; [self updateResolutionPreview]; }

- (void)updateFovUI {
    int fp = (int)self.fovPresetPopup.selectedItem.tag;
    int wf = clampInt((int)llround(self.worldFovSlider.doubleValue), 75, 105);
    int fpf = clampInt((int)llround(self.firstPersonFovSlider.doubleValue), 55, 75);
    BOOL enable = (fp != 0);
    self.worldFovSlider.enabled = enable;
    self.firstPersonFovSlider.enabled = enable;
    self.worldFovValueLabel.stringValue = [NSString stringWithFormat:@"%d°", wf];
    self.firstPersonFovValueLabel.stringValue = [NSString stringWithFormat:@"%d°", fpf];
    self.worldFovValueLabel.textColor = enable ? NSColor.labelColor : NSColor.tertiaryLabelColor;
    self.firstPersonFovValueLabel.textColor = enable ? NSColor.labelColor : NSColor.tertiaryLabelColor;
    if (!enable) {
        self.fovTipLabel.stringValue = @"Keeping existing FOV settings in FALLOUT.INI.";
    } else if (fpf > 65) {
        self.fovTipLabel.stringValue = @"Pip-Boy stays 75°. >65° 1st-person may clip arms or iron sights.";
    } else {
        self.fovTipLabel.stringValue = @"Pip-Boy & terminals stay at 75° (untouched).";
    }
}

- (void)fovPresetChanged:(id)sender {
    (void)sender;
    int fp = (int)self.fovPresetPopup.selectedItem.tag;
    if (fp >= 1 && fp <= 3) {
        FalloutOptions tmp = {0};
        applyFovPresetDefaults(&tmp, fp);
        self.worldFovSlider.integerValue = tmp.worldFov;
        self.firstPersonFovSlider.integerValue = tmp.firstPersonFov;
    }
    [self updateFovUI];
}

- (void)fovSliderChanged:(id)sender {
    (void)sender;
    int wf = clampInt((int)llround(self.worldFovSlider.doubleValue), 75, 105);
    int fpf = clampInt((int)llround(self.firstPersonFovSlider.doubleValue), 55, 75);
    self.worldFovSlider.integerValue = wf;
    self.firstPersonFovSlider.integerValue = fpf;
    [self.fovPresetPopup selectItemWithTag:matchingFovPreset(wf, fpf)];
    [self updateFovUI];
}

- (void)quitClicked:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}

- (void)playClicked:(id)sender {
    (void)sender;
    FalloutOptions opts;
    opts.preset = (int)self.presetPopup.selectedItem.tag;
    opts.renderScale = (unsigned)self.scalePopup.selectedItem.tag;
    opts.upscaler = (int)self.upscalerPopup.selectedItem.tag;
    opts.antiAliasing = (int)self.aaPopup.selectedItem.tag;
    opts.textureSize = (int)self.texturePopup.selectedItem.tag;
    opts.anisotropy = (int)self.anisoPopup.selectedItem.tag;
    opts.shadowQuality = (int)self.shadowPopup.selectedItem.tag;
    opts.waterQuality = (int)self.waterPopup.selectedItem.tag;
    opts.screenEffects = (int)self.effectsPopup.selectedItem.tag;
    opts.fovPreset = (int)self.fovPresetPopup.selectedItem.tag;
    opts.worldFov = clampInt((int)llround(self.worldFovSlider.doubleValue), 75, 105);
    opts.firstPersonFov = clampInt((int)llround(self.firstPersonFovSlider.doubleValue), 55, 75);
    opts.fullscreen = (self.displayPopup.selectedItem.tag != 0);
    opts.disableMouseAccel = (self.mouseAccelCheck.state == NSControlStateValueOn);
    opts.skipLauncher = (self.skipLauncherCheck.state == NSControlStateValueOn);
    saveFalloutOptions(opts);
    const char *envScale = getenv("FNV_RENDER_SCALE");
    if (envScale && envScale[0]) {
        unsigned v = (unsigned)strtoul(envScale, NULL, 10);
        if (v >= 50u && v <= 100u) opts.renderScale = v;
    }
    const char *envUp = getenv("FNV_UPSCALER");
    if (envUp && envUp[0]) {
        if (!strcasecmp(envUp, "bilinear") || !strcmp(envUp, "0")) opts.upscaler = 0;
        else if (!strcasecmp(envUp, "metalfx") || !strcmp(envUp, "1")) opts.upscaler = 1;
    }
    const char *envAA = getenv("FNV_AA");
    if (envAA && envAA[0]) {
        if (!strcasecmp(envAA, "off") || !strcasecmp(envAA, "none") || !strcmp(envAA, "0")) opts.antiAliasing = 0;
        else if (!strcasecmp(envAA, "smaa") || !strcmp(envAA, "1")) opts.antiAliasing = 1;
        else if (!strcasecmp(envAA, "fxaa") || !strcmp(envAA, "2")) opts.antiAliasing = 2;
    }
    const char *envFov = getenv("FNV_FOV");
    if (envFov && envFov[0]) {
        opts.worldFov = clampInt((int)strtol(envFov, NULL, 10), 75, 105);
        opts.fovPreset = matchingFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    const char *envFpFov = getenv("FNV_1ST_PERSON_FOV");
    if (envFpFov && envFpFov[0]) {
        opts.firstPersonFov = clampInt((int)strtol(envFpFov, NULL, 10), 55, 75);
        opts.fovPreset = matchingFovPreset(opts.worldFov, opts.firstPersonFov);
    }
    if (getenv("FNV_WINDOWED")) opts.fullscreen = NO;
    const char *envMA = getenv("FNV_MOUSE_ACCEL");
    if (envMA && envMA[0]) opts.disableMouseAccel = (atoi(envMA) == 0);

    self.savedOpts = opts;
    self.hasSavedOpts = YES;
    if (self.onPlay) {
        self.onPlay((FNVLaunchSettings){opts.renderScale, opts.upscaler, opts.antiAliasing, opts.fullscreen});
    }
}

- (NSWindow *)presentWindowWithDelegate:(id<NSWindowDelegate>)delegate
                                 onPlay:(void (^)(FNVLaunchSettings settings))onPlay {
    self.onPlay = onPlay;
    FalloutOptions opts = loadFalloutOptions();

    NSRect rect = NSMakeRect(0, 0, 480, 653);
    self.optionsWindow = [[NSWindow alloc] initWithContentRect:rect
                                                     styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    self.optionsWindow.title = [NSString stringWithFormat:@"%@ — Options", GAME_TITLE];
    self.optionsWindow.delegate = delegate;
    self.optionsWindow.releasedWhenClosed = NO;
    NSView *cv = self.optionsWindow.contentView;

    NSTextField *title = [NSTextField labelWithString:@"Graphics & Input Options"];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.frame = NSMakeRect(24, 617, 432, 22);
    [cv addSubview:title];

    NSString *dispStr = (self.backingW != self.nativeW || self.backingH != self.nativeH)
        ? [NSString stringWithFormat:@"Display: %ux%u panel (desktop %ux%u) @ %d Hz",
           self.nativeW, self.nativeH, self.backingW, self.backingH, self.refreshHz]
        : [NSString stringWithFormat:@"Display: %ux%u @ %d Hz",
           self.nativeW, self.nativeH, self.refreshHz];
    NSTextField *sub = [NSTextField labelWithString:dispStr];
    sub.font = [NSFont systemFontOfSize:12];
    sub.textColor = NSColor.secondaryLabelColor;
    sub.frame = NSMakeRect(24, 597, 432, 16);
    [cv addSubview:sub];

    NSBox *sep1 = [[NSBox alloc] initWithFrame:NSMakeRect(24, 588, 432, 1)];
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

    mkLabel(@"Quality Preset:", 554);
    self.presetPopup = mkPopup(552);
    [self.presetPopup addItemWithTitle:@"Ultra (Recommended)"]; self.presetPopup.lastItem.tag = 4;
    [self.presetPopup addItemWithTitle:@"High"]; self.presetPopup.lastItem.tag = 3;
    [self.presetPopup addItemWithTitle:@"Medium"]; self.presetPopup.lastItem.tag = 2;
    [self.presetPopup addItemWithTitle:@"Low"]; self.presetPopup.lastItem.tag = 1;
    [self.presetPopup addItemWithTitle:@"Keep FalloutPrefs.ini"]; self.presetPopup.lastItem.tag = 0;
    [self.presetPopup selectItemWithTag:opts.preset];
    self.presetPopup.target = self;
    self.presetPopup.action = @selector(presetChanged:);

    mkLabel(@"Render Scale:", 523);
    self.scalePopup = mkPopup(521);
    [self.scalePopup addItemWithTitle:@"100% (Native)"]; self.scalePopup.lastItem.tag = 100;
    [self.scalePopup addItemWithTitle:@"85% (Ultra Quality)"]; self.scalePopup.lastItem.tag = 85;
    [self.scalePopup addItemWithTitle:@"75% (Quality)"]; self.scalePopup.lastItem.tag = 75;
    [self.scalePopup addItemWithTitle:@"67% (Balanced)"]; self.scalePopup.lastItem.tag = 67;
    [self.scalePopup addItemWithTitle:@"50% (Performance)"]; self.scalePopup.lastItem.tag = 50;
    if (![self.scalePopup selectItemWithTag:(NSInteger)opts.renderScale]) [self.scalePopup selectItemWithTag:100];
    self.scalePopup.target = self;
    self.scalePopup.action = @selector(scaleChanged:);

    self.resPreviewLabel = [NSTextField labelWithString:@""];
    self.resPreviewLabel.font = [NSFont systemFontOfSize:11];
    self.resPreviewLabel.textColor = NSColor.secondaryLabelColor;
    self.resPreviewLabel.frame = NSMakeRect(198, 502, 258, 15);
    [cv addSubview:self.resPreviewLabel];

    mkLabel(@"Upscaler:", 474);
    self.upscalerPopup = mkPopup(472);
    [self.upscalerPopup addItemWithTitle:@"MetalFX Spatial"]; self.upscalerPopup.lastItem.tag = 1;
    [self.upscalerPopup addItemWithTitle:@"Bilinear"]; self.upscalerPopup.lastItem.tag = 0;
    [self.upscalerPopup selectItemWithTag:opts.upscaler];
    self.upscalerPopup.target = self;
    self.upscalerPopup.action = @selector(upscalerChanged:);

    mkLabel(@"Anti-Aliasing:", 443);
    self.aaPopup = mkPopup(441);
    [self.aaPopup addItemWithTitle:@"Off"]; self.aaPopup.lastItem.tag = 0;
    [self.aaPopup addItemWithTitle:@"SMAA 1x (Post-Process)"]; self.aaPopup.lastItem.tag = 1;
    [self.aaPopup addItemWithTitle:@"FXAA (Post-Process)"]; self.aaPopup.lastItem.tag = 2;
    [self.aaPopup selectItemWithTag:opts.antiAliasing];

    mkLabel(@"Texture Size:", 412);
    self.texturePopup = mkPopup(410);
    [self.texturePopup addItemWithTitle:@"Large (High)"]; self.texturePopup.lastItem.tag = 0;
    [self.texturePopup addItemWithTitle:@"Medium"]; self.texturePopup.lastItem.tag = 1;
    [self.texturePopup addItemWithTitle:@"Small (Low)"]; self.texturePopup.lastItem.tag = 2;
    [self.texturePopup selectItemWithTag:opts.textureSize];

    mkLabel(@"Anisotropic Filtering:", 381);
    self.anisoPopup = mkPopup(379);
    [self.anisoPopup addItemWithTitle:@"15x (Max)"]; self.anisoPopup.lastItem.tag = 15;
    [self.anisoPopup addItemWithTitle:@"8x"]; self.anisoPopup.lastItem.tag = 8;
    [self.anisoPopup addItemWithTitle:@"4x"]; self.anisoPopup.lastItem.tag = 4;
    [self.anisoPopup addItemWithTitle:@"2x"]; self.anisoPopup.lastItem.tag = 2;
    [self.anisoPopup addItemWithTitle:@"Off"]; self.anisoPopup.lastItem.tag = 0;
    if (![self.anisoPopup selectItemWithTag:opts.anisotropy]) [self.anisoPopup selectItemWithTag:15];

    mkLabel(@"Shadow Quality:", 350);
    self.shadowPopup = mkPopup(348);
    [self.shadowPopup addItemWithTitle:@"Ultra (1024)"]; self.shadowPopup.lastItem.tag = 3;
    [self.shadowPopup addItemWithTitle:@"High (512)"]; self.shadowPopup.lastItem.tag = 2;
    [self.shadowPopup addItemWithTitle:@"Medium (256)"]; self.shadowPopup.lastItem.tag = 1;
    [self.shadowPopup addItemWithTitle:@"Off"]; self.shadowPopup.lastItem.tag = 0;
    [self.shadowPopup selectItemWithTag:opts.shadowQuality];

    mkLabel(@"Water Detail:", 319);
    self.waterPopup = mkPopup(317);
    [self.waterPopup addItemWithTitle:@"Ultra (1024 Reflections)"]; self.waterPopup.lastItem.tag = 3;
    [self.waterPopup addItemWithTitle:@"High (512 Reflections)"]; self.waterPopup.lastItem.tag = 2;
    [self.waterPopup addItemWithTitle:@"Medium (512 Reflections)"]; self.waterPopup.lastItem.tag = 1;
    [self.waterPopup addItemWithTitle:@"Low (Off)"]; self.waterPopup.lastItem.tag = 0;
    [self.waterPopup selectItemWithTag:opts.waterQuality];

    mkLabel(@"Screen Effects:", 288);
    self.effectsPopup = mkPopup(286);
    [self.effectsPopup addItemWithTitle:@"HDR + Depth of Field"]; self.effectsPopup.lastItem.tag = 3;
    [self.effectsPopup addItemWithTitle:@"HDR Only"]; self.effectsPopup.lastItem.tag = 2;
    [self.effectsPopup addItemWithTitle:@"Bloom"]; self.effectsPopup.lastItem.tag = 1;
    [self.effectsPopup addItemWithTitle:@"Off"]; self.effectsPopup.lastItem.tag = 0;
    [self.effectsPopup selectItemWithTag:opts.screenEffects];

    mkLabel(@"Display Mode:", 257);
    self.displayPopup = mkPopup(255);
    [self.displayPopup addItemWithTitle:@"Fullscreen"]; self.displayPopup.lastItem.tag = 1;
    [self.displayPopup addItemWithTitle:@"Windowed"]; self.displayPopup.lastItem.tag = 0;
    [self.displayPopup selectItemWithTag:opts.fullscreen ? 1 : 0];

    mkLabel(@"FOV Preset:", 226);
    self.fovPresetPopup = mkPopup(224);
    [self.fovPresetPopup addItemWithTitle:@"Original (75° / 55°)"]; self.fovPresetPopup.lastItem.tag = 1;
    [self.fovPresetPopup addItemWithTitle:@"Expanded (90° / 60°)"]; self.fovPresetPopup.lastItem.tag = 2;
    [self.fovPresetPopup addItemWithTitle:@"Wide (100° / 65°)"]; self.fovPresetPopup.lastItem.tag = 3;
    [self.fovPresetPopup addItemWithTitle:@"Custom"]; self.fovPresetPopup.lastItem.tag = 4;
    [self.fovPresetPopup addItemWithTitle:@"Keep FALLOUT.INI"]; self.fovPresetPopup.lastItem.tag = 0;
    [self.fovPresetPopup selectItemWithTag:opts.fovPreset];
    self.fovPresetPopup.target = self;
    self.fovPresetPopup.action = @selector(fovPresetChanged:);

    mkLabel(@"World FOV:", 197);
    self.worldFovSlider = [NSSlider sliderWithValue:opts.worldFov minValue:75 maxValue:105 target:self action:@selector(fovSliderChanged:)];
    self.worldFovSlider.frame = NSMakeRect(196, 194, 214, 24);
    self.worldFovSlider.numberOfTickMarks = 7;
    self.worldFovSlider.allowsTickMarkValuesOnly = NO;
    self.worldFovSlider.continuous = YES;
    [cv addSubview:self.worldFovSlider];

    self.worldFovValueLabel = [NSTextField labelWithString:@""];
    self.worldFovValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.worldFovValueLabel.alignment = NSTextAlignmentRight;
    self.worldFovValueLabel.frame = NSMakeRect(414, 197, 42, 20);
    [cv addSubview:self.worldFovValueLabel];

    mkLabel(@"1st-Person FOV:", 169);
    self.firstPersonFovSlider = [NSSlider sliderWithValue:opts.firstPersonFov minValue:55 maxValue:75 target:self action:@selector(fovSliderChanged:)];
    self.firstPersonFovSlider.frame = NSMakeRect(196, 166, 214, 24);
    self.firstPersonFovSlider.numberOfTickMarks = 5;
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

    self.mouseAccelCheck = [NSButton checkboxWithTitle:@"Disable in-game mouse acceleration (menus & camera)"
                                                target:nil action:nil];
    self.mouseAccelCheck.frame = NSMakeRect(24, 110, 432, 18);
    self.mouseAccelCheck.state = opts.disableMouseAccel ? NSControlStateValueOn : NSControlStateValueOff;
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

    [self updateResolutionPreview];
    [self updateDetailControlsEnabled];
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
    FalloutOptions opts = self.hasSavedOpts ? self.savedOpts : loadFalloutOptions();
    NSFileManager *fm = NSFileManager.defaultManager;

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

    // Ensure My Games/<Game> directory and seed FALLOUT.INI from Fallout_default.ini on clean install.
    NSString *myGamesDir = [userDir.path stringByAppendingPathComponent:[@"My Games/" stringByAppendingString:GAME_DIR]];
    [fm createDirectoryAtPath:myGamesDir withIntermediateDirectories:YES attributes:nil error:NULL];

    NSString *mainIni = [myGamesDir stringByAppendingPathComponent:@"FALLOUT.INI"];
    for (NSString *leaf in [fm contentsOfDirectoryAtPath:myGamesDir error:NULL]) {
        if ([leaf caseInsensitiveCompare:@"FALLOUT.INI"] == NSOrderedSame) {
            mainIni = [myGamesDir stringByAppendingPathComponent:leaf];
            break;
        }
    }
    if (![fm fileExistsAtPath:mainIni]) {
        NSString *defaultIni = [root URLByAppendingPathComponent:@"Fallout_default.ini"].path;
        if ([fm fileExistsAtPath:defaultIni]) {
            [fm copyItemAtPath:defaultIni toPath:mainIni error:NULL];
        }
    }
    if ([fm fileExistsAtPath:mainIni]) {
        NSString *iniText = [NSString stringWithContentsOfFile:mainIni encoding:NSISOLatin1StringEncoding error:NULL] ?: @"";
        NSMutableArray<NSString *> *iniLines = [[iniText componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] mutableCopy];
        NSMutableDictionary *iniSettings = [NSMutableDictionary dictionary];
        iniSettings[@"controls"] = opts.disableMouseAccel
            ? @{
                @"fforegroundmouseaccelbase": @[@"fForegroundMouseAccelBase", @"0"],
                @"fforegroundmouseacceltop":  @[@"fForegroundMouseAccelTop",  @"0"],
                @"fforegroundmousebase":      @[@"fForegroundMouseBase",      @"0"],
                @"fforegroundmousemult":      @[@"fForegroundMouseMult",      @"0"],
            }
            : @{
                @"fforegroundmouseaccelbase": @[@"fForegroundMouseAccelBase", @"0.005"],
                @"fforegroundmouseacceltop":  @[@"fForegroundMouseAccelTop",  @"0.025"],
                @"fforegroundmousebase":      @[@"fForegroundMouseBase",      @"0"],
                @"fforegroundmousemult":      @[@"fForegroundMouseMult",      @"4"],
            };
        if (opts.fovPreset != 0) {
            iniSettings[@"display"] = @{
                @"fdefaultworldfov":     @[@"fDefaultWorldFOV",     [NSString stringWithFormat:@"%.4f", (double)opts.worldFov]],
                @"fdefault1stpersonfov": @[@"fDefault1stPersonFOV", [NSString stringWithFormat:@"%.4f", (double)opts.firstPersonFov]],
            };
        }
        FNVApplyIniSettings(iniLines, iniSettings);
        [[iniLines componentsJoinedByString:@"\n"] writeToFile:mainIni atomically:YES encoding:NSISOLatin1StringEncoding error:NULL];
    }

    // Update display/graphics/controller keys in FalloutPrefs.ini while preserving all other user settings.
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

    NSMutableDictionary *displaySettings = [@{
        @"isize w": @[@"iSize W", @(fnv_display_width())],
        @"isize h": @[@"iSize H", @(fnv_display_height())],
        @"bfull screen": @[@"bFull Screen", @1],
        @"sd3ddevice": @[@"sD3DDevice", @(fnv_display_adapter_description())],
        @"imultisample": @[@"iMultiSample", @0],
        @"iwatermultisamples": @[@"iWaterMultisamples", @0],
        @"btransparencymultisampling": @[@"bTransparencyMultisampling", @0],
    } mutableCopy];
    if (opts.fovPreset != 0) {
        displaySettings[@"fdefaultworldfov"] = @[@"fDefaultWorldFOV", [NSString stringWithFormat:@"%.4f", (double)opts.worldFov]];
        displaySettings[@"fdefault1stpersonfov"] = @[@"fDefault1stPersonFOV", [NSString stringWithFormat:@"%.4f", (double)opts.firstPersonFov]];
    }
    NSMutableDictionary *allSettings = [@{
        @"display": displaySettings,
        @"controls": @{
            @"buse joystick": @[@"bUse Joystick", @1],
        },
    } mutableCopy];

    if (opts.preset > 0) {
        // Base draw distances / LOD / terrain / grass / decals matching Bethesda's official presets.
        if (opts.preset == 1) { // Low
            displaySettings[@"imaxdecalsperframe"] = @[@"iMaxDecalsPerFrame", @2];
            displaySettings[@"fspecularlodstartfade"] = @[@"fSpecularLODStartFade", @"200.0000"];
            displaySettings[@"fshadowlodstartfade"] = @[@"fShadowLODStartFade", @"100.0000"];
            displaySettings[@"flightlodstartfade"] = @[@"fLightLODStartFade", @"200.0000"];
            allSettings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"2.0000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"2.5000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"1.0000"],
            };
            allSettings[@"terrainmanager"] = @{
                @"ftreeloaddistance":         @[@"fTreeLoadDistance",         @"0.0000"],
                @"fblockloaddistancelow":     @[@"fBlockLoadDistanceLow",     @"25000.0000"],
                @"fblockloaddistance":        @[@"fBlockLoadDistance",        @"125000.0000"],
                @"fsplitdistancemult":        @[@"fSplitDistanceMult",        @"0.7500"],
                @"fhighblockloaddistancelow": @[@"fHighBlockLoadDistanceLow", @"20000.0000"],
            };
            allSettings[@"grass"] = @{
                @"b30grassvs":              @[@"b30GrassVS",              @0],
                @"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"0.0000"],
            };
        } else if (opts.preset == 2) { // Medium
            displaySettings[@"imaxdecalsperframe"] = @[@"iMaxDecalsPerFrame", @5];
            displaySettings[@"fspecularlodstartfade"] = @[@"fSpecularLODStartFade", @"500.0000"];
            displaySettings[@"fshadowlodstartfade"] = @[@"fShadowLODStartFade", @"200.0000"];
            displaySettings[@"flightlodstartfade"] = @[@"fLightLODStartFade", @"1000.0000"];
            allSettings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"2.5000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"2.5000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"3.5000"],
            };
            allSettings[@"terrainmanager"] = @{
                @"ftreeloaddistance":         @[@"fTreeLoadDistance",         @"25000.0000"],
                @"fblockloaddistancelow":     @[@"fBlockLoadDistanceLow",     @"50000.0000"],
                @"fblockloaddistance":        @[@"fBlockLoadDistance",        @"125000.0000"],
                @"fsplitdistancemult":        @[@"fSplitDistanceMult",        @"1.0000"],
                @"fhighblockloaddistancelow": @[@"fHighBlockLoadDistanceLow", @"20000.0000"],
            };
            allSettings[@"grass"] = @{
                @"b30grassvs":              @[@"b30GrassVS",              @0],
                @"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"3000.0000"],
            };
        } else if (opts.preset == 3) { // High
            displaySettings[@"imaxdecalsperframe"] = @[@"iMaxDecalsPerFrame", @10];
            displaySettings[@"fspecularlodstartfade"] = @[@"fSpecularLODStartFade", @"1000.0000"];
            displaySettings[@"fshadowlodstartfade"] = @[@"fShadowLODStartFade", @"460.0000"];
            displaySettings[@"flightlodstartfade"] = @[@"fLightLODStartFade", @"2500.0000"];
            allSettings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"6.0000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"7.5000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"4.5000"],
            };
            allSettings[@"terrainmanager"] = @{
                @"ftreeloaddistance":         @[@"fTreeLoadDistance",         @"26000.0000"],
                @"fblockloaddistancelow":     @[@"fBlockLoadDistanceLow",     @"50000.0000"],
                @"fblockloaddistance":        @[@"fBlockLoadDistance",        @"125000.0000"],
                @"fsplitdistancemult":        @[@"fSplitDistanceMult",        @"1.0000"],
                @"fhighblockloaddistancelow": @[@"fHighBlockLoadDistanceLow", @"20000.0000"],
            };
            allSettings[@"grass"] = @{
                @"b30grassvs":              @[@"b30GrassVS",              @1],
                @"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"5000.0000"],
            };
        } else { // Ultra (4)
            displaySettings[@"imaxdecalsperframe"] = @[@"iMaxDecalsPerFrame", @10];
            displaySettings[@"fspecularlodstartfade"] = @[@"fSpecularLODStartFade", @"2000.0000"];
            displaySettings[@"fshadowlodstartfade"] = @[@"fShadowLODStartFade", @"1000.0000"];
            displaySettings[@"flightlodstartfade"] = @[@"fLightLODStartFade", @"3500.0000"];
            allSettings[@"lod"] = @{
                @"flodfadeoutmultactors":  @[@"fLODFadeOutMultActors",  @"15.0000"],
                @"flodfadeoutmultobjects": @[@"fLODFadeOutMultObjects", @"15.0000"],
                @"flodfadeoutmultitems":   @[@"fLODFadeOutMultItems",   @"15.0000"],
            };
            allSettings[@"terrainmanager"] = @{
                @"ftreeloaddistance":         @[@"fTreeLoadDistance",         @"40000.0000"],
                @"fblockloaddistancelow":     @[@"fBlockLoadDistanceLow",     @"50000.0000"],
                @"fblockloaddistance":        @[@"fBlockLoadDistance",        @"125000.0000"],
                @"fsplitdistancemult":        @[@"fSplitDistanceMult",        @"1.5000"],
                @"fhighblockloaddistancelow": @[@"fHighBlockLoadDistanceLow", @"20000.0000"],
            };
            allSettings[@"grass"] = @{
                @"b30grassvs":              @[@"b30GrassVS",              @1],
                @"fgrassstartfadedistance": @[@"fGrassStartFadeDistance", @"7000.0000"],
            };
        }

        // Individual detail settings (either preset defaults or user overrides)
        displaySettings[@"itexmipmapskip"] = @[@"iTexMipMapSkip", @(opts.textureSize)];
        displaySettings[@"itexmipmapminimum"] = @[@"iTexMipMapMinimum", @4];
        displaySettings[@"imaxanisotropy"] = @[@"iMaxAnisotropy", @(opts.anisotropy)];
        displaySettings[@"fdecallod1"] = @[@"fDecalLOD1", @"500.0000"];
        displaySettings[@"fdecallod2"] = @[@"fDecalLOD2", @"1000.0000"];

        if (opts.shadowQuality == 0) {
            displaySettings[@"bdrawshadows"] = @[@"bDrawShadows", @0];
            displaySettings[@"ishadowfilter"] = @[@"iShadowFilter", @0];
            displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @256];
            displaySettings[@"iactorshadowcountint"] = @[@"iActorShadowCountInt", @0];
            displaySettings[@"iactorshadowcountext"] = @[@"iActorShadowCountExt", @0];
        } else if (opts.shadowQuality == 1) {
            displaySettings[@"bdrawshadows"] = @[@"bDrawShadows", @1];
            displaySettings[@"ishadowfilter"] = @[@"iShadowFilter", @1];
            displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @256];
            displaySettings[@"iactorshadowcountint"] = @[@"iActorShadowCountInt", @4];
            displaySettings[@"iactorshadowcountext"] = @[@"iActorShadowCountExt", @2];
        } else if (opts.shadowQuality == 2) {
            displaySettings[@"bdrawshadows"] = @[@"bDrawShadows", @1];
            displaySettings[@"ishadowfilter"] = @[@"iShadowFilter", @2];
            displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @512];
            displaySettings[@"iactorshadowcountint"] = @[@"iActorShadowCountInt", @4];
            displaySettings[@"iactorshadowcountext"] = @[@"iActorShadowCountExt", @2];
        } else {
            displaySettings[@"bdrawshadows"] = @[@"bDrawShadows", @1];
            displaySettings[@"ishadowfilter"] = @[@"iShadowFilter", @2];
            displaySettings[@"ishadowmapresolution"] = @[@"iShadowMapResolution", @1024];
            displaySettings[@"iactorshadowcountint"] = @[@"iActorShadowCountInt", @6];
            displaySettings[@"iactorshadowcountext"] = @[@"iActorShadowCountExt", @6];
        }

        if (opts.waterQuality == 0) {
            allSettings[@"water"] = @{
                @"busewaterrefractions":           @[@"bUseWaterRefractions",           @0],
                @"busewaterreflections":           @[@"bUseWaterReflections",           @0],
                @"busewaterreflectionblur":        @[@"bUseWaterReflectionBlur",        @0],
                @"busewaterdisplacements":         @[@"bUseWaterDisplacements",         @0],
                @"busewaterdepth":                 @[@"bUseWaterDepth",                 @0],
                @"iwaterreflectwidth":             @[@"iWaterReflectWidth",             @256],
                @"iwaterreflectheight":            @[@"iWaterReflectHeight",            @256],
                @"iwaterbluramount":               @[@"iWaterBlurAmount",               @0],
                @"bautowatersilhouettereflections":@[@"bAutoWaterSilhouetteReflections",@1],
                @"bforcehighdetailreflections":    @[@"bForceHighDetailReflections",    @0],
            };
        } else if (opts.waterQuality == 1) {
            allSettings[@"water"] = @{
                @"busewaterrefractions":           @[@"bUseWaterRefractions",           @1],
                @"busewaterreflections":           @[@"bUseWaterReflections",           @1],
                @"busewaterreflectionblur":        @[@"bUseWaterReflectionBlur",        @0],
                @"busewaterdisplacements":         @[@"bUseWaterDisplacements",         @1],
                @"busewaterdepth":                 @[@"bUseWaterDepth",                 @1],
                @"iwaterreflectwidth":             @[@"iWaterReflectWidth",             @512],
                @"iwaterreflectheight":            @[@"iWaterReflectHeight",            @512],
                @"iwaterbluramount":               @[@"iWaterBlurAmount",               @0],
                @"bautowatersilhouettereflections":@[@"bAutoWaterSilhouetteReflections",@1],
                @"bforcehighdetailreflections":    @[@"bForceHighDetailReflections",    @0],
            };
        } else if (opts.waterQuality == 2) {
            allSettings[@"water"] = @{
                @"busewaterrefractions":           @[@"bUseWaterRefractions",           @1],
                @"busewaterreflections":           @[@"bUseWaterReflections",           @1],
                @"busewaterreflectionblur":        @[@"bUseWaterReflectionBlur",        @1],
                @"busewaterdisplacements":         @[@"bUseWaterDisplacements",         @1],
                @"busewaterdepth":                 @[@"bUseWaterDepth",                 @1],
                @"iwaterreflectwidth":             @[@"iWaterReflectWidth",             @512],
                @"iwaterreflectheight":            @[@"iWaterReflectHeight",            @512],
                @"iwaterbluramount":               @[@"iWaterBlurAmount",               @2],
                @"bautowatersilhouettereflections":@[@"bAutoWaterSilhouetteReflections",@1],
                @"bforcehighdetailreflections":    @[@"bForceHighDetailReflections",    @0],
            };
        } else {
            allSettings[@"water"] = @{
                @"busewaterrefractions":           @[@"bUseWaterRefractions",           @1],
                @"busewaterreflections":           @[@"bUseWaterReflections",           @1],
                @"busewaterreflectionblur":        @[@"bUseWaterReflectionBlur",        @1],
                @"busewaterdisplacements":         @[@"bUseWaterDisplacements",         @1],
                @"busewaterdepth":                 @[@"bUseWaterDepth",                 @1],
                @"iwaterreflectwidth":             @[@"iWaterReflectWidth",             @1024],
                @"iwaterreflectheight":            @[@"iWaterReflectHeight",            @1024],
                @"iwaterbluramount":               @[@"iWaterBlurAmount",               @4],
                @"bautowatersilhouettereflections":@[@"bAutoWaterSilhouetteReflections",@0],
                @"bforcehighdetailreflections":    @[@"bForceHighDetailReflections",    @1],
            };
        }

        allSettings[@"blurshaderhdr"] = @{
            @"bdohighdynamicrange": @[@"bDoHighDynamicRange", (opts.screenEffects >= 2) ? @1 : @0],
        };
        allSettings[@"blurshader"] = @{
            @"buseblurshader": @[@"bUseBlurShader", (opts.screenEffects == 1) ? @1 : @0],
        };
        allSettings[@"imagespace"] = @{
            @"bdodepthoffield":  @[@"bDoDepthOfField",  (opts.screenEffects == 3) ? @1 : @0],
            @"iradialblurlevel": @[@"iRadialBlurLevel", (opts.preset >= 4) ? @2 : ((opts.preset == 3) ? @1 : @0)],
        };
    }

    FNVApplyIniSettings(lines, allSettings);

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
    NSString *fovLog = (opts.fovPreset == 0) ? @"keep" : [NSString stringWithFormat:@"%d/%d", opts.worldFov, opts.firstPersonFov];
    fprintf(stderr, "[display] render %ux%u -> output %ux%u (scale=%u%% upscaler=%s aa=%s); preset=%d tex=%d aniso=%d shadow=%d water=%d fx=%d fov=%s mouse_accel=%d; user=%s; preferences=%s\n",
            fnv_display_width(), fnv_display_height(), outW, outH,
            opts.renderScale, opts.upscaler == 1 ? "metalfx" : "bilinear",
            opts.antiAliasing == 1 ? "smaa" : (opts.antiAliasing == 2 ? "fxaa" : "off"),
            opts.preset, opts.textureSize, opts.anisotropy,
            opts.shadowQuality, opts.waterQuality, opts.screenEffects, fovLog.UTF8String,
            opts.disableMouseAccel ? 0 : 1,
            userDir.fileSystemRepresentation,
            prefsOK ? "written" : prefsError.localizedDescription.UTF8String);
}

@end

#endif
