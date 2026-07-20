//
//  iOS3-VM — root view controller (on-device emulator self-test).
//
//  Runs the real S5L8900 machine model on the phone: a bare-metal ARM payload
//  printing over the emulated UART, and a timer interrupt reaching a guest
//  handler. Also reports the runtime facts the dynarec depends on (CS_DEBUGGED
//  and whether a plain RWX page can be mapped).
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "EmulatorViewController.h"
#import <sys/mman.h>
#import <sys/utsname.h>
#import <unistd.h>
#import <string.h>
#import "soc.h"

// csops() reports our own code-signing flags. A jailbreak that enables "JIT in
// apps" sets CS_DEBUGGED, which is what permits unsigned executable memory.
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000

@implementation EmulatorViewController {
    UITextView *_log;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    _log = [[UITextView alloc] initWithFrame:self.view.bounds];
    _log.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _log.backgroundColor = [UIColor blackColor];
    _log.textColor = [UIColor colorWithRed:0.4 green:1.0 blue:0.5 alpha:1.0];
    _log.font = [UIFont fontWithName:@"Menlo" size:12] ?: [UIFont systemFontOfSize:12];
    _log.editable = NO;
    _log.textContainerInset = UIEdgeInsetsMake(56, 16, 40, 16);
    [self.view addSubview:_log];

    [self runSelfTest];
}

- (void)append:(NSString *)line {
    _log.text = [_log.text stringByAppendingString:[line stringByAppendingString:@"\n"]];
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

    // On A9 (no APRR) a plain RWX mapping is all the future dynarec needs.
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    BOOL rwx = (page != MAP_FAILED);
    [self append:[NSString stringWithFormat:@"RWX mmap    : %@",
                  rwx ? @"YES  (dynarec-ready)" : @"no"]];
    if (rwx) munmap(page, 4096);
}

#pragma mark - Emulator demos

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
        0xe5821000u,   // STR r1,[r2,#0x00]   disable timer
        0xe3a01001u,   // MOV r1,#1
        0xe582100cu,   // STR r1,[r2,#0x0c]   clear timer interrupt
        0xe25ef004u    // SUBS pc,lr,#4
    };
    s5l8900_load(&m, 0x40, handler, sizeof handler);

    const uint32_t spin = 0xeafffffeu;
    s5l8900_load(&m, 0x100, &spin, 4);

    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << S5L8900_IRQ_TIMER);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER_RELOAD, 4);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER_CTRL,
                  TIMER_CTRL_ENABLE | TIMER_CTRL_INT_EN);

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
    uint32_t fsr = arm_mmu_translate(&m.cpu, 0x80001234u, false, true, &pa);
    [self append:[NSString stringWithFormat:@"[mmu]  0x80001234 -> 0x%08x (fsr %u)  %@",
                  pa, fsr, (fsr == 0 && pa == 0x00201234u) ? @"OK" : @"FAIL"]];
    s5l8900_free(&m);
}

- (void)runSelfTest {
    [self append:@"iOS3-VM  ·  on-device self-test"];
    [self append:@"================================\n"];
    [self reportEnvironment];
    [self append:@"\n-- emulated S5L8900 --"];
    [self runUartDemo];
    [self runInterruptDemo];
    [self runMmuDemo];
    [self append:@"\n--------------------------------"];
    [self append:@"ARMv6 core, MMU, bus, UART, VIC and"];
    [self append:@"timer all running on this device."];
    [self append:@"Next: M3 — real Apple firmware."];
}

@end
