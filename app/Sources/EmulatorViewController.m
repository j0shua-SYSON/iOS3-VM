//
//  iOS3-VM — root view controller.
//
//  The screen is now the point. The top of the view is the guest's 320x480
//  framebuffer; underneath it is the guest's UART, which is where an operating
//  system announces itself. Between them is a status line showing that the
//  emulator is doing work rather than being a picture of an emulator.
//
//  The self-tests that used to be this whole screen still run, once, at launch,
//  and print into the console: they are the proof that the ARM core, the MMU,
//  the bus, the UART, the VIC and the timer all work on *this* device, and that
//  is worth keeping even now that there is something to look at.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "EmulatorViewController.h"
#import "VMEngine.h"
#import "VMFramebufferView.h"
#import "VMGuest.h"

#import <QuartzCore/QuartzCore.h>
#import <math.h>
#import <stdlib.h>
#import <string.h>
#import <sys/mman.h>
#import <sys/utsname.h>
#import <unistd.h>

// csops() reports our own code-signing flags. A jailbreak that enables "JIT in
// apps" sets CS_DEBUGGED, which is what permits unsigned executable memory.
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000

// Scrollback kept in the console. The guest prints one short line per frame
// forever, so this cannot be unbounded.
static const NSUInteger kConsoleScrollback = 12000;

@class EmulatorViewController;

/* CADisplayLink retains its target. Keeping only a weak edge back to the view
 * controller lets normal controller teardown reach -dealloc and stop the VM. */
@interface VMDisplayLinkProxy : NSObject
- (instancetype)initWithTarget:(EmulatorViewController *)target;
- (void)tick:(CADisplayLink *)sender;
@end

// Declared up front so every call below is checked against a prototype.
@interface EmulatorViewController ()
- (void)startEmulator;
- (void)tick:(CADisplayLink *)sender;
- (void)append:(NSString *)line;
- (void)appendConsole:(NSString *)text;
- (void)reportEnvironment;
- (void)runUartDemo;
- (void)runInterruptDemo;
- (void)runMmuDemo;
- (void)appWillResignActive:(NSNotification *)notification;
- (void)appDidBecomeActive:(NSNotification *)notification;
@end

@implementation VMDisplayLinkProxy {
    __weak EmulatorViewController *_target;
}

- (instancetype)initWithTarget:(EmulatorViewController *)target {
    self = [super init];
    if (self) _target = target;
    return self;
}

- (void)tick:(CADisplayLink *)sender {
    EmulatorViewController *target = _target;
    if (target) [target tick:sender];
    else [sender invalidate];
}

@end

@implementation EmulatorViewController {
    VMFramebufferView *_screen;
    UILabel           *_stats;
    UITextView        *_console;
    NSMutableString   *_consoleText;

    VMEngine          *_engine;
    CADisplayLink     *_link;
    uint8_t           *_frame;        // main thread's copy of the guest's pixels
    NSUInteger         _ticks;
}

#pragma mark - Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    _consoleText = [NSMutableString string];

    _screen = [[VMFramebufferView alloc] initWithFrame:CGRectZero];
    _screen.layer.borderWidth = 1.0;
    _screen.layer.borderColor = [UIColor colorWithWhite:0.25 alpha:1.0].CGColor;
    [self.view addSubview:_screen];

    _stats = [[UILabel alloc] initWithFrame:CGRectZero];
    _stats.backgroundColor = [UIColor clearColor];
    _stats.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];
    _stats.font = [UIFont fontWithName:@"Menlo" size:11]
                  ?: [UIFont systemFontOfSize:11];
    _stats.text = @"starting…";
    [self.view addSubview:_stats];

    _console = [[UITextView alloc] initWithFrame:CGRectZero];
    _console.backgroundColor = [UIColor blackColor];
    _console.textColor = [UIColor colorWithRed:0.4 green:1.0 blue:0.5 alpha:1.0];
    _console.font = [UIFont fontWithName:@"Menlo" size:10]
                    ?: [UIFont systemFontOfSize:10];
    _console.editable = NO;
    _console.textContainerInset = UIEdgeInsetsMake(6, 12, 12, 12);
    [self.view addSubview:_console];

    [self append:@"iOS3-VM  ·  on-device self-test"];
    [self append:@"================================\n"];
    [self reportEnvironment];
    [self append:@"\n-- emulated S5L8900 --"];
    [self runUartDemo];
    [self runInterruptDemo];
    [self runMmuDemo];
    [self append:@"\n-- guest framebuffer --"];

    [self startEmulator];
}

- (void)startEmulator {
    _frame = calloc(1, VM_FB_BYTES);
    if (!_frame) { [self append:@"[vm] out of memory for the frame buffer"]; return; }

    _engine = [[VMEngine alloc] init];
    if (![_engine start]) {
        [self append:@"[vm] emulator failed to start"];
        [self appendConsole:[_engine takePendingConsoleText]];
        _engine = nil;
        free(_frame);
        _frame = NULL;
        return;
    }

    // 30 Hz is plenty: the guest cannot repaint 320x480 anywhere near that
    // fast, so a higher rate would only re-upload identical pixels.
    //
    VMDisplayLinkProxy *proxy = [[VMDisplayLinkProxy alloc] initWithTarget:self];
    _link = [CADisplayLink displayLinkWithTarget:proxy selector:@selector(tick:)];
    _link.preferredFramesPerSecond = 30;
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

    // Interpreting flat out in the background is a good way to be terminated,
    // and nobody is looking at the screen anyway.
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(appWillResignActive:)
               name:UIApplicationWillResignActiveNotification object:nil];
    [nc addObserver:self selector:@selector(appDidBecomeActive:)
               name:UIApplicationDidBecomeActiveNotification object:nil];

    /* A foreground transition can race the relatively expensive VM startup
     * before the observers above exist. Reconcile with current state so a
     * missed resign-active notification cannot leave the interpreter burning
     * CPU in the background on a memory-constrained phone. */
    if ([UIApplication sharedApplication].applicationState != UIApplicationStateActive)
        [self appWillResignActive:nil];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_link invalidate];
    [_engine stop];
    free(_frame);
}

- (void)appWillResignActive:(NSNotification *)notification {
    (void)notification;
    [_engine setPaused:YES];
    _link.paused = YES;
}

- (void)appDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    _link.paused = NO;
    [_engine setPaused:NO];
}

#pragma mark - Layout

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGRect b = self.view.bounds;
    UIEdgeInsets safe = self.view.safeAreaInsets;
    CGFloat top    = safe.top + 8.0;
    CGFloat bottom = safe.bottom + 8.0;
    CGFloat available = b.size.height - top - bottom;
    if (available < 120.0) available = 120.0;

    // Give the guest's screen the top ~60%, then fit 320x480 inside that
    // without distortion. The layer's contentsGravity would do this anyway;
    // doing it here too means the view's own aspect is already correct, so
    // there is no interpretation of contentsScale that can stretch the image.
    CGFloat band  = floor(available * 0.60);
    CGFloat scale = fmin(b.size.width / (CGFloat)VM_FB_WIDTH,
                         band / (CGFloat)VM_FB_HEIGHT);
    CGFloat w = floor((CGFloat)VM_FB_WIDTH  * scale);
    CGFloat h = floor((CGFloat)VM_FB_HEIGHT * scale);
    _screen.frame = CGRectMake(floor((b.size.width - w) * 0.5),
                               top + floor((band - h) * 0.5), w, h);

    CGFloat y = top + band + 6.0;
    _stats.frame = CGRectMake(14.0, y, b.size.width - 28.0, 16.0);
    y += 20.0;

    CGFloat consoleH = b.size.height - bottom - y;
    if (consoleH < 40.0) consoleH = 40.0;
    _console.frame = CGRectMake(0.0, y, b.size.width, consoleH);
}

#pragma mark - Presentation

- (void)tick:(CADisplayLink *)sender {
    (void)sender;

    BOOL argb = NO;
    if (_frame && [_engine copyFrameInto:_frame capacity:VM_FB_BYTES argb:&argb])
        [_screen presentPixels:_frame
                         width:VM_FB_WIDTH
                        height:VM_FB_HEIGHT
                        stride:VM_FB_WIDTH * VM_FB_BPP
                          argb:argb];

    [self appendConsole:[_engine takePendingConsoleText]];

    // The status line reads as noise if it changes 30 times a second.
    if ((++_ticks % 8) == 0) _stats.text = [_engine statusLine];
}

- (void)append:(NSString *)line {
    [self appendConsole:[line stringByAppendingString:@"\n"]];
}

- (void)appendConsole:(NSString *)text {
    if (!text.length) return;

    // Only follow the tail if the user has not scrolled up to read something.
    CGFloat slack = _console.contentSize.height - _console.contentOffset.y
                  - _console.bounds.size.height;
    BOOL followTail = (slack < 40.0);

    [_consoleText appendString:text];
    if (_consoleText.length > kConsoleScrollback) {
        [_consoleText deleteCharactersInRange:
            NSMakeRange(0, _consoleText.length - kConsoleScrollback)];
    }
    _console.text = _consoleText;

    if (followTail && _consoleText.length)
        [_console scrollRangeToVisible:NSMakeRange(_consoleText.length - 1, 1)];
}

#pragma mark - Environment

- (void)reportEnvironment {
    struct utsname u; uname(&u);
    [self append:[NSString stringWithFormat:@"device : %s", u.machine]];
    [self append:[NSString stringWithFormat:@"os     : iOS %@",
                  [[UIDevice currentDevice] systemVersion]]];

    int flags = 0;
    BOOL debugged = (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) == 0)
                    && (flags & CS_DEBUGGED);
    [self append:[NSString stringWithFormat:@"CS_DEBUGGED : %@", debugged ? @"YES" : @"no"]];

    /* Requesting an RWX page is a useful capability hint on A9, but it is not
     * proof that branching to unsigned memory is safe. Never execute probe
     * code automatically during viewDidLoad: a jailbreak policy mismatch
     * would turn every app launch into the same crash loop. The eventual JIT
     * diagnostics screen can run an explicit, recoverable execution test. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    BOOL rwx = (page != MAP_FAILED);
    [self append:[NSString stringWithFormat:@"RWX mmap    : %@",
                  rwx ? @"YES" : @"no"]];
    [self append:[NSString stringWithFormat:
        @"JIT execute : not run at startup (%@)",
        (rwx && debugged) ? @"manual diagnostic required"
                          : @"capability preflight failed"]];
    if (rwx) munmap(page, 4096);

    [self append:[NSString stringWithFormat:@"footprint   : %.1f MB at launch",
                  [VMEngine physFootprintBytes] / 1048576.0]];
}

#pragma mark - Emulator self-tests

// A bare-metal payload: load the UART base from a literal and print "HI\n".
- (void)runUartDemo {
    s5l8900_t m;
    if (!s5l8900_init(&m, 0, 1u << 20)) { [self append:@"[soc] init failed"]; return; }

    const uint32_t payload[] = {
        0xe59f0018u,          // LDR r0,[pc,#24]
        0xe3a01048u,          // MOV r1,#'H'
        0xe5801020u,          // STR r1,[r0,#0x20]   UTXH
        0xe3a01049u,          // MOV r1,#'I'
        0xe5801020u,          // STR r1,[r0,#0x20]
        0xe3a0100au,          // MOV r1,#'\n'
        0xe5801020u,          // STR r1,[r0,#0x20]
        0xeafffffeu,          // B .
        S5L8900_UART0_BASE    // literal
    };
    s5l8900_load(&m, 0, payload, sizeof payload);
    m.cpu.r[15] = 0;

    arm_status_t st = ARM_OK;
    unsigned n = s5l8900_run(&m, 32, &st);
    m.uart0.tx[m.uart0.tx_len] = '\0';

    [self append:[NSString stringWithFormat:@"[uart] guest said: %s", m.uart0.tx]];
    [self append:[NSString stringWithFormat:@"[uart] %u instructions, status %d", n, (int)st]];
    s5l8900_free(&m);
}

// Timer -> VIC -> CPU IRQ -> guest handler -> SUBS pc,lr,#4 back to the loop.
- (void)runInterruptDemo {
    s5l8900_t m;
    if (!s5l8900_init(&m, 0, 1u << 20)) { [self append:@"[soc] init failed"]; return; }

    const uint32_t branch = 0xea000000u | (((0x40u - 0x18u - 8u) / 4u) & 0x00ffffffu);
    s5l8900_load(&m, 0x18, &branch, 4);          // IRQ vector -> 0x40

    const uint32_t handler[] = {
        0xe3a01054u,   // MOV r1,#'T'
        0xe5801020u,   // STR r1,[r0,#0x20]
        0xe3a01000u,   // MOV r1,#0
        0xe58210a4u,   // STR r1,[r2,#0xa4]   stop timer 4
        0xe3a01803u,   // MOV r1,#0x00030000
        0xe58210f4u,   // STR r1,[r2,#0xf4]   acknowledge, as the kernel does
        0xe25ef004u    // SUBS pc,lr,#4
    };
    s5l8900_load(&m, 0x40, handler, sizeof handler);

    const uint32_t spin = 0xeafffffeu;
    s5l8900_load(&m, 0x100, &spin, 4);

    // This self-test exercises device -> controller -> CPU, not the clock
    // ratio, so run the timebase at one tick per instruction to keep it quick.
    m.cpu_hz = m.tb_hz = 1;

    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << S5L8900_IRQ_TIMER);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER4_COUNTBUF, 4);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER4_STATE,
                  TIMER4_STATE_START | TIMER4_STATE_UPDATE);

    m.cpu.r[15] = 0x100;
    m.cpu.r[0]  = S5L8900_UART0_BASE;
    m.cpu.r[2]  = S5L8900_TIMER_BASE;
    m.cpu.cpsr  = ARM_MODE_SYS;                  // IRQs unmasked

    arm_status_t st = ARM_OK;
    s5l8900_run(&m, 200, &st);
    m.uart0.tx[m.uart0.tx_len] = '\0';

    BOOL ok = (strcmp(m.uart0.tx, "T") == 0)
              && ((m.cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS)
              && (m.cpu.r[15] == 0x100);
    [self append:[NSString stringWithFormat:@"[irq]  handler printed \"%s\", resumed pc=%08x  %@",
                  m.uart0.tx, m.cpu.r[15], ok ? @"OK" : @"FAIL"]];
    s5l8900_free(&m);
}

// Prove the MMU translates on-device: map one 1 MB section and walk it.
- (void)runMmuDemo {
    s5l8900_t m;
    if (!s5l8900_init(&m, 0, 1u << 20)) { [self append:@"[soc] init failed"]; return; }

    const uint32_t l1 = 0x4000;
    uint32_t entry = (0x00200000u & 0xfff00000u) | (3u << 10) | 2u;   // section, AP=11
    s5l8900_load(&m, l1 + ((0x80000000u >> 20) << 2), &entry, 4);
    m.cpu.cp15.ttbr0 = l1;
    m.cpu.cp15.dacr  = 1u;
    m.cpu.cp15.sctlr |= ARM_SCTLR_M;

    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(&m.cpu, 0x80001234u, ARM_ACCESS_READ, true, &pa);
    [self append:[NSString stringWithFormat:@"[mmu]  0x80001234 -> 0x%08x (fsr %u)  %@",
                  pa, fsr, (fsr == 0 && pa == 0x00201234u) ? @"OK" : @"FAIL"]];
    s5l8900_free(&m);
}

@end
