//
//  iOS3-VM — the guest's screen.
//
//  WHY A CGImage ON A CALayer AND NOT METAL
//
//  The panel is 320x480 at 32 bpp: 600 KB a frame, at most 60 times a second.
//  That is nothing. The expensive part of a display path at this size is not
//  pixel throughput, it is the amount of machinery that has to be right before
//  a single pixel appears — and a CAMetalLayer costs a device, a queue, a
//  pipeline state, a shader source file in the build, and a drawable lifecycle,
//  every piece of which is another thing that can be wrong on a phone I cannot
//  attach a debugger to. Handing CoreAnimation an immutable CGImage is two
//  calls, and the compositor already does the scaling on the GPU for free.
//
//  Nearest-neighbour comes from layer.magnificationFilter; aspect-correct
//  scaling from contentsGravity. The view controller *also* lays this view out
//  at the exact 320:480 aspect, so the two agree and no interpretation of
//  contentsScale can stretch the picture.
//
//  If the guest ever runs fast enough that per-frame texture upload matters,
//  this is the one file to replace. It is not the bottleneck today: the ARM
//  interpreter is, by four orders of magnitude.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMFramebufferView.h"
#import <QuartzCore/QuartzCore.h>
#import <stdlib.h>
#import <string.h>

// CoreGraphics owns the copy until the CGImage dies; then it hands it back.
static void vm_fb_release_data(void *info, const void *data, size_t size) {
    (void)info;
    (void)size;
    free((void *)data);
}

@implementation VMFramebufferView {
    CGColorSpaceRef _colorSpace;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    _colorSpace = CGColorSpaceCreateDeviceRGB();

    self.backgroundColor = [UIColor blackColor];
    self.opaque = YES;
    self.userInteractionEnabled = NO;
    self.clearsContextBeforeDrawing = NO;

    self.layer.magnificationFilter = kCAFilterNearest;
    self.layer.minificationFilter  = kCAFilterNearest;
    self.layer.contentsGravity     = kCAGravityResizeAspect;
    self.layer.needsDisplayOnBoundsChange = NO;
    self.layer.backgroundColor = [UIColor blackColor].CGColor;
    return self;
}

- (void)dealloc {
    if (_colorSpace) CGColorSpaceRelease(_colorSpace);
}

- (void)presentPixels:(const void *)pixels
                width:(size_t)w
               height:(size_t)h
               stride:(size_t)stride
                 argb:(BOOL)argb {
    if (!pixels || w == 0 || h == 0 || stride < w * 4 || !_colorSpace) return;

    const size_t bytes = stride * h;

    // The CGImage must own immutable pixels: the emulator thread is free to
    // repaint its framebuffer the instant this method returns.
    void *copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, pixels, bytes);

    CGDataProviderRef provider =
        CGDataProviderCreateWithData(NULL, copy, bytes, vm_fb_release_data);
    if (!provider) { free(copy); return; }

    // Alpha is always skipped, never honoured: the guest's alpha byte is not
    // trustworthy (XNU's console leaves it zero), and composited it would erase
    // the whole screen. That leaves only the component order to get right, and
    // it is the same "alpha first" layout either way, differing only in endian:
    //   B,G,R,A in memory  == 0xAARRGGBB little-endian  -> ByteOrder32Little
    //   A,R,G,B in memory  == 0xAARRGGBB big-endian     -> ByteOrder32Big
    CGBitmapInfo info = (CGBitmapInfo)(kCGImageAlphaNoneSkipFirst
        | (argb ? kCGBitmapByteOrder32Big : kCGBitmapByteOrder32Little));

    CGImageRef image = CGImageCreate(
        w, h, 8, 32, stride, _colorSpace, info,
        provider, NULL, false, kCGRenderingIntentDefault);

    CGDataProviderRelease(provider);   // frees `copy` too, if the image failed
    if (!image) return;

    self.layer.contents = (__bridge id)image;
    CGImageRelease(image);
}

@end
