/*
 * Copyright (c) 2026 Anurodh Pokharel. MIT license, see LICENSE.
 */
#import <AppKit/AppKit.h>
#import "GSDMViews.h"

@interface GSDMController : NSObject
{
	GSDMWindow *window;
	GSDMPanelView *panel;
	NSRect panelHome;

	/* user list page */
	NSView *usersPage;
	NSScrollView *scroll;
	GSDMTileGrid *grid;

	/* password page */
	NSView *loginPage;
	GSDMAvatarView *avatar;
	NSTextField *nameLabel;
	NSTextField *nameField;         /* "Other..." only */
	NSSecureTextField *passwordField;
	NSButton *loginButton;
	NSButton *backButton;
	NSTextField *message;

	NSButton *restartButton;
	NSButton *shutdownButton;

	NSString *selectedLogin;        /* nil: "Other..." */
	int fd;                         /* socket to gsdm, -1 in preview mode */
	NSFileHandle *daemon;
	BOOL busy;
	NSTimer *shakeTimer;
	int shakeStep;
}
@end
