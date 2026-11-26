#import <AppKit/AppKit.h>

// Hosts the Metal view inside the game window. While fullscreen it keeps the image aspect-exact and
// anchored to the bottom edge, so a frame rendered shorter than the display leaves the reserved
// camera-housing strip black at the top instead of covering the game image. Windowed mode and
// displays without a housing simply fill the window.
@interface FNVGameView : NSView

// Pixel size the renderer produces. Fullscreen layout derives the on-screen height from it, so the
// image always fits without letterboxing; ignored while windowed.
@property (nonatomic) CGSize contentPixelSize;

// Set while the window is in native fullscreen.
@property (nonatomic) BOOL fullscreenMode;

// The Metal view (or any view) that should fill the content area below the reserved strip.
@property (nonatomic, weak) NSView *gameView;

// Positions gameView; called on resize, on state changes and by tests.
- (void)layoutGameView;

@end
