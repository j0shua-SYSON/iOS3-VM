//
//  iOS3-VM — the emulator run loop. See VMEngine.h.
//
//  THREADING
//
//  One thread owns the machine for its whole lifetime: it allocates it, steps
//  it, and frees it. Nothing else calls into core/ at all. Every few tens of
//  milliseconds that thread takes a mutex and publishes a snapshot — a copy of
//  the guest's framebuffer, whatever the guest printed to the UART, and a
//  couple of counters. The UI takes the same mutex and copies the snapshot out.
//  The lock is therefore held only for two memcpys of a 600 KB buffer, never
//  across interpretation, so the main thread cannot be blocked behind guest
//  execution no matter how slow the guest is.
//
//  MEMORY
//
//  Guest DRAM is 128 MB, matching the hardware. On a 2 GB device that sounds
//  alarming and is not, for one specific reason: core's s5l8900_init() gets it
//  from calloc(), and a request that size goes straight to mmap'd anonymous
//  memory that the kernel fills with zeroes lazily, one page at a time, on
//  first touch. Untouched guest RAM is address space, not footprint. This demo
//  guest touches its one code page and the 600 KB framebuffer, so the resident
//  cost is well under a megabyte of the 128.
//
//  That is a claim about the allocator, not a measurement, so the app measures
//  it: physFootprintBytes reads phys_footprint from TASK_VM_INFO, which is the
//  exact counter jetsam compares against the per-process limit, and the app
//  prints the before/after delta on screen. Do not trust the paragraph above
//  over the number on the phone.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMEngine.h"

#import <mach/mach.h>
#import <pthread.h>
#import <time.h>
#import <unistd.h>
#import <stdlib.h>
#import <string.h>

// How many instructions to interpret between checks of the stop/pause flags.
// At a few million instructions a second this is roughly 15-30 ms, which keeps
// pause latency short without paying the flag check too often.
static const unsigned kVMChunkInstructions = 100000;

// Publish a snapshot at most this often. The UI redraws at 30 Hz; going faster
// would only copy the same pixels twice.
static const double kVMPublishInterval = 1.0 / 30.0;

// Cap on UART bytes held for the UI. The view controller drains this every
// frame and keeps the real scrollback; this bound only matters if the UI
// stops draining (backgrounded, say) while the guest keeps printing.
static const NSUInteger kVMConsoleLimit = 16000;

static double vm_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Declared up front so every call below is checked against a prototype.
@interface VMEngine ()
- (void)threadMain;
- (void)publishRetired:(uint64_t)retired
                  rate:(double)instantRate
                status:(arm_status_t)status;
- (void)appendConsole:(NSString *)text;
@end

@implementation VMEngine {
    s5l8900_t        _machine;
    BOOL             _machineReady;

    NSThread        *_thread;
    pthread_mutex_t  _lock;

    // Everything below is guarded by _lock.
    uint8_t         *_snapshot;      // VM_FB_BYTES, last published frame
    BOOL             _snapshotFresh;
    BOOL             _snapshotARGB;  // byte order of the snapshot's pixels
    NSMutableString *_pending;
    uint64_t         _retired;
    double           _rate;          // instructions per second, smoothed
    NSString        *_status;
    BOOL             _stopRequested;
    BOOL             _paused;
}

+ (uint64_t)physFootprintBytes {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                 (task_info_t)&info, &count);
    if (kr != KERN_SUCCESS) return 0;
    return (uint64_t)info.phys_footprint;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    pthread_mutex_init(&_lock, NULL);
    _pending = [NSMutableString string];
    _status  = @"idle";
    return self;
}

- (void)dealloc {
    // Safe without any handshake: NSThread holds a strong reference to its
    // target for as long as the thread is alive, so -dealloc cannot possibly
    // run while -threadMain is still using the snapshot buffer or the lock.
    free(_snapshot);
    pthread_mutex_destroy(&_lock);
}

#pragma mark - Lifecycle

- (BOOL)start {
    if (_thread) return YES;

    _snapshot = calloc(1, VM_FB_BYTES);
    if (!_snapshot) return NO;

    // Measured either side of the allocation so the guest DRAM's real cost is
    // a number on the screen rather than an assertion in a comment.
    uint64_t before = [VMEngine physFootprintBytes];

    if (!s5l8900_init(&_machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE)) {
        free(_snapshot);
        _snapshot = NULL;
        [self appendConsole:@"[vm] could not allocate 128 MB of guest DRAM\n"];
        return NO;
    }
    if (!vm_guest_install(&_machine)) {
        s5l8900_free(&_machine);
        free(_snapshot);
        _snapshot = NULL;
        [self appendConsole:@"[vm] could not install the guest payload\n"];
        return NO;
    }
    _machineReady = YES;

    uint64_t after = [VMEngine physFootprintBytes];
    [self appendConsole:[NSString stringWithFormat:
        @"[vm] guest DRAM: %u MB at 0x%08x, framebuffer at 0x%08x\n"
        @"[vm] footprint before/after allocating it: %.1f / %.1f MB\n",
        VM_GUEST_RAM_SIZE / (1024u * 1024u), VM_GUEST_RAM_BASE,
        vm_guest_fb_pa(VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE),
        before / 1048576.0, after / 1048576.0]];

    _thread = [[NSThread alloc] initWithTarget:self
                                      selector:@selector(threadMain)
                                        object:nil];
    _thread.name = @"iOS3-VM emulator";
    _thread.qualityOfService = NSQualityOfServiceUserInitiated;
    _thread.stackSize = 512 * 1024;
    [_thread start];
    return YES;
}

- (void)stop {
    pthread_mutex_lock(&_lock);
    _stopRequested = YES;
    _paused = NO;
    pthread_mutex_unlock(&_lock);
}

- (void)setPaused:(BOOL)paused {
    pthread_mutex_lock(&_lock);
    _paused = paused;
    pthread_mutex_unlock(&_lock);
}

#pragma mark - Emulator thread

- (void)threadMain {
    double lastPublish = vm_now();
    uint64_t retired = 0, retiredAtLastPublish = 0;
    arm_status_t status = ARM_OK;

    while (YES) {
        @autoreleasepool {
            pthread_mutex_lock(&_lock);
            BOOL stop = _stopRequested;
            BOOL paused = _paused;
            pthread_mutex_unlock(&_lock);

            if (stop) break;
            if (paused) {
                usleep(50 * 1000);
                lastPublish = vm_now();
                retiredAtLastPublish = retired;
                continue;
            }

            retired += s5l8900_run(&_machine, kVMChunkInstructions, &status);

            double now = vm_now();
            double elapsed = now - lastPublish;
            if (elapsed >= kVMPublishInterval || status != ARM_OK) {
                double instantRate = elapsed > 0
                    ? (double)(retired - retiredAtLastPublish) / elapsed : 0.0;
                [self publishRetired:retired rate:instantRate status:status];
                lastPublish = now;
                retiredAtLastPublish = retired;
            }

            // A non-OK status means the guest hit an encoding this core does
            // not implement, or halted. Stopping is right: spinning on the same
            // faulting instruction would burn the battery and tell us nothing.
            if (status != ARM_OK) break;
        }
    }

    if (_machineReady) {
        s5l8900_free(&_machine);
        _machineReady = NO;
    }
}

// Called on the emulator thread only.
- (void)publishRetired:(uint64_t)retired
                  rate:(double)instantRate
                status:(arm_status_t)status {
    // Ask the display controller where the framebuffer is and how it is laid
    // out, rather than assuming. vm_guest_display() validates that the reported
    // buffer is inside DRAM and no larger than VM_FB_BYTES before returning it.
    uint32_t fbStride = 0, fbW = 0, fbH = 0;
    vm_pixel_order_t order = VM_ORDER_BGRA;
    const uint8_t *fb = vm_guest_display(&_machine, &fbW, &fbH, &fbStride, &order);
    size_t fbBytes = 0;
    if (!fb || fbW == 0 || fbH == 0 || fbW > SIZE_MAX / VM_FB_BPP ||
        fbStride < (size_t)fbW * VM_FB_BPP || fbStride > SIZE_MAX / fbH) {
        fb = NULL;
    } else {
        fbBytes = (size_t)fbStride * fbH;
        if (fbBytes == 0 || fbBytes > VM_FB_BYTES) fb = NULL;
    }

    // Drain the UART before taking the lock's contents out of the machine:
    // core's tx buffer is a fixed 8 KB that stops accepting bytes when full, so
    // whoever is watching has to empty it or the guest goes quiet.
    NSString *fresh = nil;
    if (_machine.uart0.tx_len > 0) {
        fresh = [[NSString alloc] initWithBytes:_machine.uart0.tx
                                         length:_machine.uart0.tx_len
                                       encoding:NSISOLatin1StringEncoding];
        _machine.uart0.tx_len = 0;
    }

    NSString *statusText;
    switch (status) {
        case ARM_OK:        statusText = @"running";        break;
        case ARM_UNDEFINED: statusText = @"undefined insn"; break;
        case ARM_HALT:      statusText = @"halted";         break;
        default:            statusText = @"?";              break;
    }

    pthread_mutex_lock(&_lock);
    if (fb && _snapshot) {
        /* A future guest mode may use less than the fixed publication buffer.
         * Clear the unused tail so a geometry change cannot expose pixels from
         * the previous frame. */
        if (fbBytes < VM_FB_BYTES)
            memset(_snapshot + fbBytes, 0, VM_FB_BYTES - fbBytes);
        memcpy(_snapshot, fb, fbBytes);
        _snapshotARGB = (order == VM_ORDER_ARGB);
        _snapshotFresh = YES;
    }
    if (fresh.length) [_pending appendString:fresh];
    if (_pending.length > kVMConsoleLimit) {
        [_pending deleteCharactersInRange:
            NSMakeRange(0, _pending.length - kVMConsoleLimit)];
    }
    _retired = retired;
    // Smooth the rate: a per-chunk figure jitters too much to read.
    _rate = (_rate > 0.0) ? (_rate * 0.8 + instantRate * 0.2) : instantRate;
    _status = statusText;
    pthread_mutex_unlock(&_lock);
}

- (void)appendConsole:(NSString *)text {
    if (!text.length) return;
    pthread_mutex_lock(&_lock);
    [_pending appendString:text];
    pthread_mutex_unlock(&_lock);
}

#pragma mark - Snapshot readers (main thread)

- (BOOL)copyFrameInto:(void *)dst capacity:(size_t)capacity argb:(BOOL *)outARGB {
    if (!dst || capacity < VM_FB_BYTES) return NO;
    BOOL copied = NO;
    pthread_mutex_lock(&_lock);
    if (_snapshotFresh && _snapshot) {
        memcpy(dst, _snapshot, VM_FB_BYTES);
        if (outARGB) *outARGB = _snapshotARGB;
        _snapshotFresh = NO;
        copied = YES;
    }
    pthread_mutex_unlock(&_lock);
    return copied;
}

- (NSString *)takePendingConsoleText {
    NSString *out = nil;
    pthread_mutex_lock(&_lock);
    if (_pending.length) {
        out = [_pending copy];
        [_pending setString:@""];
    }
    pthread_mutex_unlock(&_lock);
    return out;
}

- (NSString *)statusLine {
    pthread_mutex_lock(&_lock);
    uint64_t retired = _retired;
    double rate = _rate;
    NSString *status = _status;
    pthread_mutex_unlock(&_lock);

    double footprintMB = [VMEngine physFootprintBytes] / 1048576.0;
    return [NSString stringWithFormat:
            @"%@  ·  %.1f M insn  ·  %.2f M insn/s  ·  %.0f MB",
            status, retired / 1.0e6, rate / 1.0e6, footprintMB];
}

@end
