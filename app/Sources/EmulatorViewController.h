//
//  iOS3-VM — root view controller.
//
//  This screen shows the emulated machine: the guest's 320x480 framebuffer at
//  the top, the guest's UART underneath, and a status line in between. The
//  emulator runs on its own thread (see VMEngine) and the UI only ever reads a
//  snapshot of guest memory, so a slow guest cannot stall the interface.
//
//  It also still runs, once at launch, the M0 self-tests that print into the
//  console: they link the platform-independent emulator core, execute real ARM
//  code through it *on the device*, and report the runtime environment the
//  later milestones depend on (device model, iOS version, and whether the
//  process has JIT / RWX memory available via the jailbreak).
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@interface EmulatorViewController : UIViewController
@end
