//
//  iOS3-VM — the guest's screen.
//
//  Displays a 32-bit guest framebuffer, scaled to fit while preserving the
//  original iPhone's 320x480 aspect ratio, with nearest-neighbour filtering so
//  a 2009 panel looks like a 2009 panel rather than a smeared upscale.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@interface VMFramebufferView : UIView

/*
 * Present one frame. `pixels` is `stride`-bytes-per-row, 32 bits per pixel.
 * When `argb` is NO the bytes are B,G,R,A (the S5L8900 framebuffer's native
 * order, see tools/bootkernel.c); when YES they are A,R,G,B. VMGuest reports
 * the validated host interpretation; the current CLCD model exposes only its
 * evidence-backed BGRA memory layout. The bytes are copied before returning,
 * so the caller may reuse the buffer immediately.
 */
- (void)presentPixels:(const void *)pixels
                width:(size_t)w
               height:(size_t)h
               stride:(size_t)stride
                 argb:(BOOL)argb;

@end
