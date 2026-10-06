/*
 * LoginWindow: gsdm's greeter.
 *
 * Copyright (c) 2026 Anurodh Pokharel. MIT license, see LICENSE.
 */
#import <AppKit/AppKit.h>
#import "GSDMController.h"

int
main(int argc, const char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	GSDMController *controller;

	/* No application icon window: nothing but the login screen. */
	[[NSUserDefaults standardUserDefaults] registerDefaults:
	    [NSDictionary dictionaryWithObjectsAndKeys:
		@"YES", @"GSSuppressAppIcon", nil]];
	[NSApplication sharedApplication];
	controller = [[GSDMController alloc] init];
	[NSApp setDelegate: controller];
	[NSApp run];
	[controller release];
	[pool release];
	return 0;
}
