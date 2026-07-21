//
//  iOS3-VM — the emulator run loop.
//
//  The core in core/ is single-threaded and has no locks in it, by design. So
//  exactly one thread ever calls into it: the one this class owns. The UI never
//  touches the machine; it reads a snapshot this class publishes under a mutex.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>
#import "VMGuest.h"

@interface VMEngine : NSObject

/* Allocate the machine, install the demo guest, and start interpreting on a
 * background thread. Returns YES when already running, and NO if startup fails
 * or a start/stop transition is already in progress. */
- (BOOL)start;

/* Ask the emulator thread to finish and release the machine. */
- (void)stop;

/* Suspend interpretation (used when the app leaves the foreground: burning a
 * core in the background is the fastest way to be terminated). */
- (void)setPaused:(BOOL)paused;

/*
 * Copy the most recent framebuffer snapshot into `dst`, and report through
 * `outARGB` whether its bytes are A,R,G,B (YES) or B,G,R,A (NO). Returns NO —
 * without copying — if nothing new has been published since the last call, so
 * the UI can skip rebuilding an identical image. `outARGB` may be NULL.
 */
- (BOOL)copyFrameInto:(void *)dst capacity:(size_t)capacity argb:(BOOL *)outARGB;

/* Everything the guest has written to the UART since the last call, or nil. */
- (NSString *)takePendingConsoleText;

/* One line for the status bar: work done, rate, and memory footprint. */
- (NSString *)statusLine;

/* This process's phys_footprint — the number jetsam actually judges — in
 * bytes, or 0 if the kernel would not tell us. */
+ (uint64_t)physFootprintBytes;

@end
