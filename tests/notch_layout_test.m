// The camera-housing reserve must keep the image aspect-exact and anchored to the bottom, so the
// unused height stays black at the TOP of the window (issue #6 follow-up). This exercises the
// layout without a notched display: same width, shorter height, no side bars.
#import <AppKit/AppKit.h>
#import <assert.h>
#import <math.h>
#import <stdio.h>
#import "FNVGameView.h"

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];

        FNVGameView *container = [[FNVGameView alloc] initWithFrame:NSMakeRect(0, 0, 800, 500)];
        NSView *game = [[NSView alloc] initWithFrame:NSZeroRect];
        container.gameView = game;
        [container addSubview:game];

        // windowed: the image fills the window
        container.contentPixelSize = CGSizeMake(2000, 1000);
        container.fullscreenMode = NO;
        assert(NSEqualRects(game.frame, NSMakeRect(0, 0, 800, 500)));

        // fullscreen with a 100px tall housing reserved: 2:1 at 800pt wide is 400pt tall, bottom-anchored
        container.fullscreenMode = YES;
        assert(NSEqualRects(game.frame, NSMakeRect(0, 0, 800, 400)));
        assert(game.frame.origin.y == 0);            // strip is above, not below
        assert(container.bounds.size.height - game.frame.size.height == 100);

        // a wider window: the height follows the same aspect and the width is always full
        container.frame = NSMakeRect(0, 0, 1000, 500);
        assert(NSEqualRects(game.frame, NSMakeRect(0, 0, 1000, 500)));

        // taller window keeps the exact aspect instead of stretching
        container.frame = NSMakeRect(0, 0, 1000, 800);
        assert(NSEqualRects(game.frame, NSMakeRect(0, 0, 1000, 500)));

        // unknown drawable size: fall back to filling the window rather than a zero-height frame
        container.contentPixelSize = CGSizeZero;
        assert(NSEqualRects(game.frame, NSMakeRect(0, 0, 1000, 800)));

        // the strip is painted black by the container itself
        container.contentPixelSize = CGSizeMake(2000, 1000);
        container.frame = NSMakeRect(0, 0, 1000, 600);
        assert(container.layer.backgroundColor != NULL);

        // Proportional notch scaling across HiDPI desktop scales (issue #6):
        // MBP 14" "More Space" (3024x1964 panel, 1800x1169 desktop, 39pt notch -> 1800x1130 fullscreen)
        unsigned moreSpaceH = FNVNotchAdjustedPanelHeight(1964, 1169.0, 39.0);
        assert(moreSpaceH == 1898);
        container.fullscreenMode = YES;
        container.contentPixelSize = CGSizeMake(3024, moreSpaceH);
        container.frame = NSMakeRect(0, 0, 1800, 1130);
        assert(game.frame.origin.x == 0 && game.frame.origin.y == 0);
        assert(game.frame.size.width == 1800);
        assert(fabs(game.frame.size.height - 1130.0) < 1.0);

        // Standard notched MacBook defaults (exact 16:10 below the housing):
        assert(FNVNotchAdjustedPanelHeight(1964, 982.0, 37.0) == 1890);  // MBP 14" (3024x1964)
        assert(FNVNotchAdjustedPanelHeight(2234, 1117.0, 37.0) == 2160); // MBP 16" (3456x2234)
        assert(FNVNotchAdjustedPanelHeight(1664, 832.0, 32.0) == 1600);  // MBA 13.6" (2560x1664)

        // Non-notched and invalid/clamped inputs leave panelH unchanged:
        assert(FNVNotchAdjustedPanelHeight(1600, 900.0, 0.0) == 1600);
        assert(FNVNotchAdjustedPanelHeight(1600, 0.0, 32.0) == 1600);
        assert(FNVNotchAdjustedPanelHeight(1600, 900.0, 900.0) == 1600);
        assert(FNVNotchAdjustedPanelHeight(480, 900.0, 32.0) == 480);

        puts("notch_layout_test: PASS");
    }
    return 0;
}
