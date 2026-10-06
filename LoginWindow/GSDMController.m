/*
 * LoginWindow's controller: builds the full-screen login window, lists the
 * users, and talks to gsdm over the socket it passes on GSDM_FD (see
 * daemon/gsdm.c serve_greeter for the protocol). Without GSDM_FD it runs
 * in preview mode: everything works except that no login, restart or shut
 * down actually happens.
 *
 * Copyright (c) 2026 Anurodh Pokharel. MIT license, see LICENSE.
 */
#import "GSDMController.h"

#include <sys/utsname.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static const CGFloat kTileW = 124, kTileH = 118;

/* A login shell: executable, not false/nologin, and in /etc/shells if there is one. */
static BOOL
realShell(const char *sh)
{
	const char *base;
	char line[1024];
	FILE *f;
	BOOL listed = NO;

	if (sh == NULL || *sh == '\0' || access(sh, X_OK) != 0)
		return NO;
	base = strrchr(sh, '/');
	base = base ? base + 1 : sh;
	if (strcmp(base, "false") == 0 || strcmp(base, "nologin") == 0)
		return NO;
	f = fopen("/etc/shells", "r");
	if (f == NULL)
		return YES;
	while (!listed && fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, " \t\r\n")] = '\0';
		if (line[0] != '#' && strcmp(line, sh) == 0)
			listed = YES;
	}
	fclose(f);
	return listed;
}

/* The GECOS full name (first field, BSD '&' = capitalized login). */
static NSString *
fullNameOf(struct passwd *p)
{
	NSString *login = [NSString stringWithUTF8String: p->pw_name];
	NSString *g, *cap;

	if (p->pw_gecos == NULL || p->pw_gecos[0] == '\0' || p->pw_gecos[0] == ',')
		return login;
	g = [NSString stringWithUTF8String: p->pw_gecos];
	if (g == nil)
		return login;
	g = [[g componentsSeparatedByString: @","] objectAtIndex: 0];
	cap = [[[login substringToIndex: 1] uppercaseString]
	    stringByAppendingString: [login substringFromIndex: 1]];
	g = [g stringByReplacingOccurrencesOfString: @"&" withString: cap];
	g = [g stringByTrimmingCharactersInSet: [NSCharacterSet whitespaceCharacterSet]];
	return [g length] ? g : login;
}

static NSComparisonResult
byName(id a, id b, void *ctx)
{
	return [[a objectAtIndex: 1] caseInsensitiveCompare: [b objectAtIndex: 1]];
}

/* [login, full name] for every account with uid >= GSDM_MIN_UID and a real shell. */
static NSArray *
listUsers(void)
{
	NSMutableArray *users = [NSMutableArray array];
	const char *e = getenv("GSDM_MIN_UID");
	long minUid = e ? atol(e) : 500;
	struct passwd *p;

	setpwent();
	while ((p = getpwent()) != NULL) {
		if ((long)p->pw_uid < minUid || p->pw_name[0] == '_' || !realShell(p->pw_shell))
			continue;
		[users addObject: [NSArray arrayWithObjects:
		    [NSString stringWithUTF8String: p->pw_name], fullNameOf(p), nil]];
	}
	endpwent();
	[users sortUsingFunction: byName context: NULL];
	return users;
}

static NSTextField *
label(NSRect frame, NSFont *font, NSColor *color)
{
	NSTextField *t = [[NSTextField alloc] initWithFrame: frame];

	[t setEditable: NO];
	[t setSelectable: NO];
	[t setBezeled: NO];
	[t setBordered: NO];
	[t setDrawsBackground: NO];
	[t setAlignment: NSCenterTextAlignment];
	[t setFont: font];
	[t setTextColor: color];
	return [t autorelease];
}

static NSButton *
button(NSString *title, NSRect frame, id target, SEL action)
{
	NSButton *b = [[NSButton alloc] initWithFrame: frame];

	[b setTitle: title];
	[b setButtonType: NSMomentaryPushInButton];
	[b setBezelStyle: NSRoundedBezelStyle];
	[b setTarget: target];
	[b setAction: action];
	return [b autorelease];
}

@implementation GSDMController

- (id) init
{
	const char *e = getenv("GSDM_FD");

	self = [super init];
	fd = (e != NULL && *e) ? atoi(e) : -1;
	return self;
}

/* ---- building the window ------------------------------------------ */

- (NSString *) title
{
	const char *t = getenv("GSDM_TITLE");
	struct utsname u;

	if (t != NULL && *t)
		return [NSString stringWithUTF8String: t];
	if (uname(&u) == 0)
		return [NSString stringWithUTF8String: u.sysname];
	return @"Login";
}

- (NSString *) hostName
{
	char h[256];

	if (gethostname(h, sizeof(h)) != 0)
		return @"";
	h[sizeof(h) - 1] = '\0';
	return [NSString stringWithUTF8String: h];
}

- (void) buildUsersPage: (NSRect)frame
{
	NSArray *users = listUsers();
	NSMutableArray *tiles = [NSMutableArray array];
	NSTextField *hint;
	NSRect sf;
	NSUInteger i, n, cols, rows;
	CGFloat gw, gh;

	usersPage = [[NSView alloc] initWithFrame: frame];
	hint = label(NSMakeRect(0, frame.size.height - 24, frame.size.width, 20),
	    [NSFont systemFontOfSize: 13], [NSColor colorWithCalibratedWhite: 0.25 alpha: 1]);
	[hint setStringValue: @"Select your name to log in."];
	[usersPage addSubview: hint];

	sf = NSMakeRect(0, 0, frame.size.width, frame.size.height - 30);
	scroll = [[NSScrollView alloc] initWithFrame: sf];
	[scroll setHasVerticalScroller: YES];
	[scroll setAutohidesScrollers: YES];
	[scroll setBorderType: NSNoBorder];
	[scroll setDrawsBackground: NO];
	[usersPage addSubview: scroll];

	n = [users count] + 1;      /* + Other... */
	gw = [scroll contentSize].width;
	cols = MAX((NSUInteger)1, (NSUInteger)(gw / kTileW));
	rows = (n + cols - 1) / cols;
	gh = MAX(rows * kTileH, [scroll contentSize].height);
	grid = [[GSDMTileGrid alloc] initWithFrame: NSMakeRect(0, 0, gw, gh)];
	for (i = 0; i < n; i++) {
		NSUInteger row = i / cols, col = i % cols;
		NSUInteger inRow = (row == rows - 1) ? n - row * cols : cols;
		CGFloat x0 = (gw - inRow * kTileW) / 2;
		/* Center short lists vertically. */
		CGFloat y0 = (rows * kTileH < gh) ? (gh - rows * kTileH) / 2 : 0;
		NSRect tf = NSMakeRect(x0 + col * kTileW, y0 + row * kTileH, kTileW, kTileH);
		GSDMUserTile *t;

		if (i < [users count]) {
			NSArray *u = [users objectAtIndex: i];

			t = [[GSDMUserTile alloc] initWithFrame: tf login: [u objectAtIndex: 0]
			    fullName: [u objectAtIndex: 1]];
		} else {
			t = [[GSDMUserTile alloc] initWithFrame: tf login: nil fullName: @"Other…"];
		}
		[t setTarget: self action: @selector(pickUser:)];
		[tiles addObject: t];
		[t release];
	}
	[grid setTiles: tiles];
	[grid setTarget: self action: @selector(pickUser:)];
	[scroll setDocumentView: grid];
}

- (void) buildLoginPage: (NSRect)frame
{
	CGFloat w = frame.size.width, h = frame.size.height, fw = 230;

	loginPage = [[NSView alloc] initWithFrame: frame];
	avatar = [[GSDMAvatarView alloc] initWithFrame: NSMakeRect((w - 100) / 2, h - 104, 100, 100)];
	[loginPage addSubview: avatar];
	nameLabel = [label(NSMakeRect(0, h - 136, w, 26), [NSFont boldSystemFontOfSize: 18],
	    [NSColor colorWithCalibratedWhite: 0.1 alpha: 1]) retain];
	[loginPage addSubview: nameLabel];

	nameField = [[NSTextField alloc] initWithFrame: NSMakeRect((w - fw) / 2, h - 144, fw, 24)];
	[[nameField cell] setPlaceholderString: @"Name"];
	[nameField setTarget: self];
	[nameField setAction: @selector(nameEntered:)];
	[loginPage addSubview: nameField];

	passwordField = [[NSSecureTextField alloc] initWithFrame:
	    NSMakeRect((w - fw) / 2, h - 172, fw, 24)];
	[[passwordField cell] setPlaceholderString: @"Password"];
	[passwordField setTarget: self];
	[passwordField setAction: @selector(logIn:)];
	[loginPage addSubview: passwordField];

	loginButton = [button(@"Log In", NSMakeRect((w + fw) / 2 + 8, h - 174, 84, 28),
	    self, @selector(logIn:)) retain];
	[loginButton setKeyEquivalent: @"\r"];
	[loginPage addSubview: loginButton];

	message = [label(NSMakeRect(0, h - 206, w, 20), [NSFont boldSystemFontOfSize: 12],
	    [NSColor colorWithCalibratedRed: 0.75 green: 0.05 blue: 0.05 alpha: 1]) retain];
	[loginPage addSubview: message];

	backButton = [button(@"Back", NSMakeRect(4, 0, 90, 28), self, @selector(back:)) retain];
	[backButton setKeyEquivalent: @"\033"];
	[loginPage addSubview: backButton];
}

- (void) applicationDidFinishLaunching: (NSNotification *)n
{
	NSRect screen = [[NSScreen mainScreen] frame];
	GSDMBackgroundView *bg;
	const char *img = getenv("GSDM_BACKGROUND_IMAGE");
	CGFloat pw = MIN(620, screen.size.width - 24), ph = MIN(460, screen.size.height - 24);
	CGFloat hh = [GSDMPanelView headerHeight];
	NSRect page;

	window = [[GSDMWindow alloc] initWithContentRect: screen
	    styleMask: NSBorderlessWindowMask backing: NSBackingStoreBuffered defer: NO];
	[window setReleasedWhenClosed: NO];
	[window setTitle: @"Login"];
	bg = [[GSDMBackgroundView alloc] initWithFrame: NSMakeRect(0, 0, screen.size.width, screen.size.height)];
	if (img != NULL && *img) {
		NSImage *i = [[NSImage alloc] initWithContentsOfFile:
		    [NSString stringWithUTF8String: img]];

		if (i != nil && [i isValid])
			[bg setImage: i];
		else
			NSLog(@"LoginWindow: cannot load background image %s", img);
		[i release];
	}
	[window setContentView: bg];
	[bg release];

	panelHome = NSMakeRect(floor((screen.size.width - pw) / 2),
	    floor((screen.size.height - ph) / 2), pw, ph);
	panel = [[GSDMPanelView alloc] initWithFrame: panelHome];
	[panel setTitle: [self title] subtitle: [self hostName]];
	[bg addSubview: panel];

	/* Pages sit between the bottom button bar and the title band. */
	page = NSMakeRect(24, 64, pw - 48, ph - 64 - hh - 20);
	[self buildUsersPage: page];
	[self buildLoginPage: page];

	restartButton = [button(@"Restart", NSMakeRect(pw / 2 - 116, 22, 108, 28),
	    self, @selector(restart:)) retain];
	shutdownButton = [button(@"Shut Down", NSMakeRect(pw / 2 + 8, 22, 108, 28),
	    self, @selector(shutDown:)) retain];
	[panel addSubview: restartButton];
	[panel addSubview: shutdownButton];

	if (fd >= 0) {
		NSRunLoop *rl = [NSRunLoop currentRunLoop];

		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		pollTimer = [NSTimer timerWithTimeInterval: 0.1 target: self
		    selector: @selector(pollDaemon:) userInfo: nil repeats: YES];
		[rl addTimer: pollTimer forMode: NSDefaultRunLoopMode];
		[rl addTimer: pollTimer forMode: NSEventTrackingRunLoopMode];
		[rl addTimer: pollTimer forMode: NSModalPanelRunLoopMode];
	} else {
		NSLog(@"LoginWindow: GSDM_FD not set, preview mode (no logins)");
	}
	[self showUsers];
	[window makeKeyAndOrderFront: nil];
	[window makeFirstResponder: grid];
	[NSApp activateIgnoringOtherApps: YES];
}

/* ---- pages --------------------------------------------------------- */

- (void) showUsers
{
	[loginPage removeFromSuperview];
	if ([usersPage superview] == nil)
		[panel addSubview: usersPage];
	[window makeFirstResponder: grid];
	[panel setNeedsDisplay: YES];
}

- (void) showLoginFor: (GSDMUserTile *)tile
{
	BOOL other = [tile login] == nil;
	CGFloat h = [loginPage frame].size.height;
	NSRect pf = [passwordField frame], bf = [loginButton frame], mf = [message frame];

	ASSIGN(selectedLogin, [tile login]);
	[avatar setName: [tile fullName]];
	[avatar setGeneric: other];
	[nameLabel setStringValue: other ? @"" : [tile fullName]];
	[nameLabel setHidden: other];
	[nameField setHidden: !other];
	[nameField setStringValue: @""];
	[passwordField setStringValue: @""];
	[message setStringValue: @""];
	/* "Other..." has a name field where the name label is. */
	pf.origin.y = h - 178;
	bf.origin.y = pf.origin.y - 2;
	mf.origin.y = pf.origin.y - 34;
	[passwordField setFrame: pf];
	[loginButton setFrame: bf];
	[message setFrame: mf];

	[usersPage removeFromSuperview];
	[panel addSubview: loginPage];
	[panel setNeedsDisplay: YES];
	[window makeFirstResponder: other ? (NSView *)nameField : (NSView *)passwordField];
}

/* ---- actions ------------------------------------------------------- */

- (void) pickUser: (id)sender
{
	if ([sender isKindOfClass: [GSDMUserTile class]])
		[self showLoginFor: sender];
}

- (void) back: (id)sender
{
	if (busy)
		return;
	[passwordField setStringValue: @""];
	[self showUsers];
}

- (void) nameEntered: (id)sender
{
	[window makeFirstResponder: passwordField];
}

- (void) setBusy: (BOOL)flag
{
	busy = flag;
	[passwordField setEnabled: !flag];
	[nameField setEnabled: !flag];
	[loginButton setEnabled: !flag];
	[backButton setEnabled: !flag];
	[restartButton setEnabled: !flag];
	[shutdownButton setEnabled: !flag];
}

- (void) send: (NSArray *)fields
{
	NSMutableData *d = [NSMutableData data];
	NSUInteger i;
	const char *p;
	size_t left;

	for (i = 0; i < [fields count]; i++) {
		const char *s = [[fields objectAtIndex: i] UTF8String];

		[d appendBytes: s length: strlen(s) + 1];
	}
	p = [d bytes];
	left = [d length];
	while (left > 0) {
		ssize_t w = write(fd, p, left);

		if (w <= 0)
			break;
		p += w;
		left -= (size_t)w;
	}
	[d resetBytesInRange: NSMakeRange(0, [d length])];
}

- (void) logIn: (id)sender
{
	NSString *user = selectedLogin;

	if (busy || [loginPage superview] == nil)
		return;
	if (user == nil) {
		id fr = [window firstResponder];

		user = [[nameField stringValue] stringByTrimmingCharactersInSet:
		    [NSCharacterSet whitespaceCharacterSet]];
		if ([user length] == 0) {
			[window makeFirstResponder: nameField];
			return;
		}
		/* Return in the name field (it also reaches the default
		 * button) moves on to the password instead of logging in. */
		if ([[passwordField stringValue] length] == 0 &&
		    (fr == nameField || ([fr isKindOfClass: [NSText class]] &&
		    [(NSText *)fr delegate] == (id)nameField))) {
			[window makeFirstResponder: passwordField];
			return;
		}
	}
	[message setStringValue: @""];
	[self setBusy: YES];
	if (fd < 0) {
		[self performSelector: @selector(loginFailed:) withObject:
		    @"Preview mode: no display manager to log in with." afterDelay: 0.5];
		return;
	}
	[self send: [NSArray arrayWithObjects: @"LOGIN", user, [passwordField stringValue], nil]];
}

- (void) loginFailed: (NSString *)why
{
	[self setBusy: NO];
	[passwordField setStringValue: @""];
	[message setStringValue: why];
	[self shake];
	[window makeFirstResponder: passwordField];
}

/*
 * gsdm's replies, polled from a timer that runs in every run loop mode the
 * window uses. (An NSFileHandle background read, which only fires in the
 * default mode, was seen under QEMU to never deliver a FAIL reply that
 * gsdm had sent, leaving the window disabled; the cause was not pinned
 * down, so the reply is read directly instead.)
 */
- (void) pollDaemon: (NSTimer *)t
{
	char buf[64];
	ssize_t n = read(fd, buf, sizeof(buf));

	if (n < 0)
		return;         /* EAGAIN: nothing yet */
	if (n == 0) {
		NSLog(@"LoginWindow: gsdm closed the connection");
		[pollTimer invalidate];
		pollTimer = nil;
		[NSApp terminate: nil];
		return;
	}
	if (n >= 2 && strncmp(buf, "OK", 2) == 0) {
		/* gsdm starts the session once we are gone. */
		[pollTimer invalidate];
		pollTimer = nil;
		[window orderOut: nil];
		[NSApp terminate: nil];
		return;
	}
	[self loginFailed: selectedLogin == nil
	    ? @"Incorrect name or password."
	    : @"Incorrect password."];
}

- (BOOL) confirm: (NSString *)what message: (NSString *)msg
{
	return NSRunAlertPanel(what, @"%@", what, @"Cancel", nil, msg) == NSAlertDefaultReturn;
}

- (void) restart: (id)sender
{
	if (busy || ![self confirm: @"Restart"
	    message: @"Are you sure you want to restart the computer now?"])
		return;
	if (fd < 0) {
		NSLog(@"LoginWindow: preview mode, not restarting");
		return;
	}
	[self setBusy: YES];
	[self send: [NSArray arrayWithObject: @"RESTART"]];
}

- (void) shutDown: (id)sender
{
	if (busy || ![self confirm: @"Shut Down"
	    message: @"Are you sure you want to shut down the computer now?"])
		return;
	if (fd < 0) {
		NSLog(@"LoginWindow: preview mode, not shutting down");
		return;
	}
	[self setBusy: YES];
	[self send: [NSArray arrayWithObject: @"SHUTDOWN"]];
}

/* ---- wrong password: shake the panel ------------------------------- */

- (void) shake
{
	shakeStep = 0;
	[shakeTimer invalidate];
	shakeTimer = [NSTimer scheduledTimerWithTimeInterval: 0.035 target: self
	    selector: @selector(shakeTick:) userInfo: nil repeats: YES];
}

- (void) shakeTick: (NSTimer *)t
{
	static const CGFloat dx[] = { -14, 14, -11, 11, -7, 7, -3, 3, 0 };
	NSRect old = [panel frame], now = panelHome;

	now.origin.x += dx[shakeStep];
	[panel setFrame: now];
	[[window contentView] setNeedsDisplayInRect: NSUnionRect(NSInsetRect(old, -2, -2), now)];
	if (++shakeStep >= (int)(sizeof(dx) / sizeof(dx[0]))) {
		[shakeTimer invalidate];
		shakeTimer = nil;
	}
}

- (BOOL) applicationShouldTerminateAfterLastWindowClosed: (NSApplication *)app
{
	return NO;
}
@end
