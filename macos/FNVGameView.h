#import <AppKit/AppKit.h>

// Hosts the Metal view inside the game window. While in native fullscreen on a notched MacBook,
// AppKit confines the window below the camera housing; matching the usable aspect keeps the image
// aspect-exact, bottom-anchored, and free of side pillarbox bars at any HiDPI scaling setting.
// Windowed mode and displays without a housing simply fill the window.
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

// Scales the physical panel height down by the exact fraction of the screen reserved by the
// camera housing in native macOS fullscreen (safeAreaInsets.top / screen.frame.size.height),
// rounded to an even pixel height so the drawable fills 100% of the fullscreen window width
// without side pillarbox bars at any HiDPI scaling setting.
unsigned FNVNotchAdjustedPanelHeight(unsigned panelH, CGFloat screenPointsH, CGFloat notchPoints);
