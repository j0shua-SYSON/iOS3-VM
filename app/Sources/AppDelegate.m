//
//  iOS3-VM — application delegate.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "AppDelegate.h"
#import "EmulatorViewController.h"

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[EmulatorViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
