#import "FNVGameView.h"

@implementation FNVGameView

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = NSColor.blackColor.CGColor;
        self.autoresizesSubviews = NO;
    }
    return self;
}

- (void)layoutGameView {
    NSRect bounds = self.bounds;
    NSRect frame = bounds;
    CGSize content = self.contentPixelSize;
    if (self.fullscreenMode && content.width > 0 && content.height > 0) {
        // Same pixel aspect as the drawable: fit the width and anchor to the bottom, leaving any
        // unused height (the camera-housing strip) black at the top.
        CGFloat height = bounds.size.width * (content.height / content.width);
        if (!(height > 0)) height = bounds.size.height;
        if (height > bounds.size.height) height = bounds.size.height;
        frame = NSMakeRect(0, 0, bounds.size.width, height);
    }
    self.gameView.frame = frame;
}

- (void)layout {
    [super layout];
    [self layoutGameView];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutGameView];
}

- (void)setContentPixelSize:(CGSize)contentPixelSize {
    _contentPixelSize = contentPixelSize;
    [self layoutGameView];
}

- (void)setFullscreenMode:(BOOL)fullscreenMode {
    _fullscreenMode = fullscreenMode;
    [self layoutGameView];
}

@end
