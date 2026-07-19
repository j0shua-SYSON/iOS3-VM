//
//  iOS3-VM — root view controller (M0 self-test screen).
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "EmulatorViewController.h"
#import <sys/mman.h>
#import <sys/sysctl.h>
#import <sys/utsname.h>
#import <unistd.h>
#import <string.h>
#import "arm.h"

// csops() lets us read our own code-signing status flags. A jailbreak that
// enables "JIT in apps" sets CS_DEBUGGED on us, which is what allows unsigned
// executable memory — the prerequisite for the future dynarec.
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000

#pragma mark - Flat guest memory (M0 demo bus)

// A trivial little-endian RAM device standing in for the S5L8900 memory map,
// so we can exercise the CPU core end to end. The real machine model arrives
// in milestone M2.
typedef struct { uint8_t *ram; uint32_t size; } flatmem_t;

static uint32_t fm_r32(void *c, uint32_t a){ flatmem_t*m=c; uint32_t v; memcpy(&v,&m->ram[a&(m->size-1)],4); return v; }
static uint16_t fm_r16(void *c, uint32_t a){ flatmem_t*m=c; uint16_t v; memcpy(&v,&m->ram[a&(m->size-1)],2); return v; }
static uint8_t  fm_r8 (void *c, uint32_t a){ flatmem_t*m=c; return m->ram[a&(m->size-1)]; }
static void fm_w32(void *c, uint32_t a, uint32_t v){ flatmem_t*m=c; memcpy(&m->ram[a&(m->size-1)],&v,4); }
static void fm_w16(void *c, uint32_t a, uint16_t v){ flatmem_t*m=c; memcpy(&m->ram[a&(m->size-1)],&v,2); }
static void fm_w8 (void *c, uint32_t a, uint8_t  v){ flatmem_t*m=c; m->ram[a&(m->size-1)]=v; }

#pragma mark - View controller

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
    _log.font = [UIFont fontWithName:@"Menlo" size:13] ?: [UIFont systemFontOfSize:13];
    _log.editable = NO;
    _log.textContainerInset = UIEdgeInsetsMake(60, 16, 40, 16);
    [self.view addSubview:_log];

    [self runSelfTest];
}

- (void)append:(NSString *)line {
    _log.text = [_log.text stringByAppendingString:[line stringByAppendingString:@"\n"]];
}

- (void)runSelfTest {
    [self append:@"iOS3-VM  ·  boot self-test"];
    [self append:@"==============================\n"];

    // --- Environment -------------------------------------------------------
    struct utsname u; uname(&u);
    [self append:[NSString stringWithFormat:@"device   : %s", u.machine]];
    [self append:[NSString stringWithFormat:@"os       : iOS %@",
                  [[UIDevice currentDevice] systemVersion]]];

    // --- JIT / RWX readiness ----------------------------------------------
    int flags = 0;
    BOOL debugged = (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) == 0)
                    && (flags & CS_DEBUGGED);
    [self append:[NSString stringWithFormat:@"CS_DEBUGGED : %@", debugged ? @"YES" : @"no"]];

    // Try to obtain a plain RWX page — on A9 (no APRR) this is all the dynarec
    // needs once the jailbreak has relaxed code-signing enforcement.
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    BOOL rwx = (page != MAP_FAILED);
    [self append:[NSString stringWithFormat:@"RWX mmap    : %@", rwx ? @"YES  (dynarec-ready)" : @"no"]];
    if (rwx) munmap(page, 4096);

    // --- Core execution proof ---------------------------------------------
    [self append:@"\n[core] executing ARM on-device ..."];
    flatmem_t mem; mem.size = 1u << 20; mem.ram = calloc(mem.size, 1);
    const arm_bus_t bus = { &mem, fm_r32, fm_r16, fm_r8, fm_w32, fm_w16, fm_w8 };

    // MOV r1,#40 ; MOV r2,#2 ; ADD r0,r1,r2
    const uint32_t prog[] = { 0xe3a01028u, 0xe3a02002u, 0xe0810002u };
    for (unsigned i = 0; i < sizeof(prog)/4; i++) fm_w32(&mem, i*4, prog[i]);

    arm_cpu_t cpu; arm_reset(&cpu, &bus);
    cpu.cpsr = (cpu.cpsr & ~0x1fu) | ARM_MODE_SYS;
    for (int i = 0; i < 3; i++) arm_step(&cpu);

    [self append:[NSString stringWithFormat:@"[core] r0 = %u  (expected 42)  %@",
                  cpu.r[0], cpu.r[0] == 42 ? @"OK" : @"FAIL"]];
    [self append:[NSString stringWithFormat:@"[core] retired %llu instructions",
                  (unsigned long long)cpu.cycles]];
    free(mem.ram);

    [self append:@"\n----------------------------------------"];
    [self append:@"M0 pipeline verified: same C core, built by"];
    [self append:@"Xcode CI, running on this device."];
    [self append:@"Next: M1 full ARMv6 · M2 S5L8900 boot."];
}

@end
