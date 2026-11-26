// The camera-housing reserve must keep the image aspect-exact and anchored to the bottom, so the
// unused height stays black at the TOP of the window (issue #6 follow-up). This exercises the
// layout without a notched display: same width, shorter height, no side bars.
#import <AppKit/AppKit.h>
#import <assert.h>
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

        puts("notch_layout_test: PASS");
    }
    return 0;
}
