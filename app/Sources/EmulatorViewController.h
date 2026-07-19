//
//  iOS3-VM — root view controller.
//
//  For milestone M0 this screen is a self-test: it links the platform-
//  independent emulator core, executes real ARM code through it *on the
//  device*, and reports the runtime environment the later milestones depend
//  on (device model, iOS version, and whether the process has JIT / RWX
//  memory available via the jailbreak). Later milestones replace the text
//  report with the emulated framebuffer.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@interface EmulatorViewController : UIViewController
@end
