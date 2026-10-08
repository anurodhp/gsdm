/*
 * Copyright (c) 2026 Anurodh Pokharel. MIT license, see LICENSE.
 */
#import "GSDMViews.h"

static NSColor *
rgb(CGFloat r, CGFloat g, CGFloat b, CGFloat a)
{
	return [NSColor colorWithCalibratedRed: r green: g blue: b alpha: a];
}

static void
drawCentered(NSString *s, NSDictionary *attrs, NSRect r)
{
	NSSize sz = [s sizeWithAttributes: attrs];

	[s drawAtPoint: NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2)
	    withAttributes: attrs];
}

/* ------------------------------------------------------------------ */

@implementation GSDMWindow
/*
 * Tab and Shift-Tab. The field editor swallows Tab, and under this X server
 * Shift-Tab arrives with no characters at all, so both are taken here by
 * key code (23 is Tab in X) or character and moved along the key view loop.
 */
- (void) sendEvent: (NSEvent *)e
{
	if ([e type] == NSKeyDown) {
		NSString *c = [e characters];
		BOOL back = ([e modifierFlags] & NSShiftKeyMask) != 0;
		unichar ch = [c length] ? [c characterAtIndex: 0] : 0;

		if (ch == NSTabCharacter || ch == NSBackTabCharacter || [e keyCode] == 23) {
			id from = [self firstResponder];
			NSView *to;

			if ([from isKindOfClass: [NSText class]] && [from isFieldEditor])
				from = [from delegate];
			if ([from isKindOfClass: [NSView class]]) {
				to = (back || ch == NSBackTabCharacter) ?
				    [from previousValidKeyView] : [from nextValidKeyView];
				if (to != nil && to != from)
					[self makeFirstResponder: to];
			}
			return;
		}
	}
	[super sendEvent: e];
}

- (BOOL) canBecomeKeyWindow
{
	return YES;
}

- (BOOL) canBecomeMainWindow
{
	return YES;
}
@end

/* ------------------------------------------------------------------ */

@implementation GSDMBackgroundView
- (void) dealloc
{
	[image release];
	[super dealloc];
}

/* No window manager runs here to give the pointer a cursor. */
- (void) resetCursorRects
{
	[self addCursorRect: [self bounds] cursor: [NSCursor arrowCursor]];
}

- (void) setImage: (NSImage *)anImage
{
	ASSIGN(image, anImage);
	[self setNeedsDisplay: YES];
}

- (BOOL) isOpaque
{
	return YES;
}

- (void) drawRect: (NSRect)rect
{
	NSRect b = [self bounds];
	NSGradient *g;

	if (image != nil) {
		NSSize is = [image size];
		CGFloat scale = MAX(b.size.width / is.width, b.size.height / is.height);
		NSRect dst = NSMakeRect(0, 0, is.width * scale, is.height * scale);

		dst.origin.x = NSMidX(b) - dst.size.width / 2;
		dst.origin.y = NSMidY(b) - dst.size.height / 2;
		[image drawInRect: dst fromRect: NSZeroRect
		    operation: NSCompositeCopy fraction: 1.0];
		return;
	}
	/* Mac "Aqua blue": light at the top, deep blue at the bottom. */
	g = [[NSGradient alloc] initWithColorsAndLocations:
	    rgb(0.40, 0.65, 0.95, 1.0), 0.0,
	    rgb(0.16, 0.42, 0.82, 1.0), 0.45,
	    rgb(0.04, 0.20, 0.55, 1.0), 1.0, nil];
	[g drawInRect: b angle: -90];
	[g release];
}
@end

/* ------------------------------------------------------------------ */

@implementation GSDMPanelView
+ (CGFloat) headerHeight
{
	return 74;
}

- (void) dealloc
{
	[title release];
	[subtitle release];
	[super dealloc];
}

- (void) setTitle: (NSString *)aTitle subtitle: (NSString *)aSubtitle
{
	ASSIGN(title, aTitle);
	ASSIGN(subtitle, aSubtitle);
	[self setNeedsDisplay: YES];
}

- (void) drawRect: (NSRect)rect
{
	NSRect b = NSInsetRect([self bounds], 8, 8);
	CGFloat radius = 14, hh = [GSDMPanelView headerHeight];
	NSBezierPath *p;
	NSGradient *g;
	NSRect header;
	NSMutableDictionary *ta;
	int i;

	/* A soft shadow: a few widening, fading outlines. */
	for (i = 8; i >= 1; i--) {
		NSRect s = NSOffsetRect(NSInsetRect(b, -i, -i), 0, -3);

		[rgb(0, 0, 0, 0.035) set];
		[[NSBezierPath bezierPathWithRoundedRect: s
		    xRadius: radius + i yRadius: radius + i] fill];
	}
	p = [NSBezierPath bezierPathWithRoundedRect: b xRadius: radius yRadius: radius];
	[rgb(0.93, 0.93, 0.94, 1.0) set];
	[p fill];

	/* Title band with the panel's rounded top: a rounded rect reaching
	 * one radius below the band, whose lower part is painted back over. */
	header = NSMakeRect(b.origin.x, NSMaxY(b) - hh, b.size.width, hh);
	g = [[NSGradient alloc] initWithStartingColor: rgb(0.99, 0.99, 1.0, 1.0)
	    endingColor: rgb(0.86, 0.88, 0.92, 1.0)];
	[g drawInBezierPath: [NSBezierPath bezierPathWithRoundedRect:
	    NSMakeRect(header.origin.x, header.origin.y - radius, header.size.width, hh + radius)
	    xRadius: radius yRadius: radius] angle: -90];
	[g release];
	[rgb(0.93, 0.93, 0.94, 1.0) set];
	NSRectFill(NSMakeRect(header.origin.x, header.origin.y - radius, header.size.width, radius));
	[rgb(0.70, 0.72, 0.76, 1.0) set];
	NSRectFill(NSMakeRect(header.origin.x, header.origin.y, header.size.width, 1));

	[rgb(0.55, 0.57, 0.62, 1.0) set];
	[p setLineWidth: 1];
	[p stroke];

	ta = [NSMutableDictionary dictionary];
	[ta setObject: [NSFont boldSystemFontOfSize: 26] forKey: NSFontAttributeName];
	[ta setObject: rgb(0.15, 0.17, 0.22, 1.0) forKey: NSForegroundColorAttributeName];
	if (title != nil)
		drawCentered(title, ta, NSMakeRect(header.origin.x, header.origin.y + 26,
		    header.size.width, 40));
	if (subtitle != nil) {
		[ta setObject: [NSFont systemFontOfSize: 12] forKey: NSFontAttributeName];
		[ta setObject: rgb(0.35, 0.37, 0.42, 1.0) forKey: NSForegroundColorAttributeName];
		drawCentered(subtitle, ta, NSMakeRect(header.origin.x, header.origin.y + 6,
		    header.size.width, 20));
	}
}
@end

/* ------------------------------------------------------------------ */

@implementation GSDMAvatarView
- (void) dealloc
{
	[name release];
	[super dealloc];
}

- (void) setName: (NSString *)aName
{
	ASSIGN(name, aName);
	[self setNeedsDisplay: YES];
}

- (void) setGeneric: (BOOL)flag
{
	generic = flag;
	[self setNeedsDisplay: YES];
}

- (NSString *) initials
{
	NSArray *words = [name componentsSeparatedByString: @" "];
	NSMutableString *s = [NSMutableString string];
	NSUInteger i;

	for (i = 0; i < [words count] && [s length] < 2; i++) {
		NSString *w = [words objectAtIndex: i];

		if ([w length] > 0)
			[s appendString: [[w substringToIndex: 1] uppercaseString]];
	}
	return s;
}

- (void) drawRect: (NSRect)rect
{
	NSRect b = [self bounds];
	CGFloat d = MIN(b.size.width, b.size.height) - 4;
	NSRect c = NSMakeRect(NSMidX(b) - d / 2, NSMidY(b) - d / 2, d, d);
	NSBezierPath *circle = [NSBezierPath bezierPathWithOvalInRect: c];
	NSGradient *g;
	CGFloat hue;
	NSUInteger h = [name hash];

	hue = generic ? 0.6 : (CGFloat)(h % 360) / 360.0;
	g = [[NSGradient alloc]
	    initWithStartingColor: [NSColor colorWithCalibratedHue: hue saturation: generic ? 0.05 : 0.35
		brightness: generic ? 0.80 : 0.92 alpha: 1]
	    endingColor: [NSColor colorWithCalibratedHue: hue saturation: generic ? 0.08 : 0.55
		brightness: generic ? 0.55 : 0.68 alpha: 1]];
	[g drawInBezierPath: circle angle: -90];
	[g release];
	[rgb(1, 1, 1, 0.9) set];
	[circle setLineWidth: 2];
	[circle stroke];

	if (generic) {
		/* Head and shoulders. */
		NSRect head = NSMakeRect(NSMidX(c) - d * 0.17, NSMidY(c) - d * 0.02, d * 0.34, d * 0.34);
		NSBezierPath *body;

		[NSGraphicsContext saveGraphicsState];
		[circle addClip];
		[rgb(1, 1, 1, 0.92) set];
		[[NSBezierPath bezierPathWithOvalInRect: head] fill];
		body = [NSBezierPath bezierPathWithOvalInRect:
		    NSMakeRect(NSMidX(c) - d * 0.32, c.origin.y - d * 0.30, d * 0.64, d * 0.62)];
		[body fill];
		[NSGraphicsContext restoreGraphicsState];
	} else {
		NSMutableDictionary *a = [NSMutableDictionary dictionary];

		[a setObject: [NSFont boldSystemFontOfSize: d * 0.38] forKey: NSFontAttributeName];
		[a setObject: [NSColor whiteColor] forKey: NSForegroundColorAttributeName];
		drawCentered([self initials], a, c);
	}
}
@end

/* ------------------------------------------------------------------ */

@implementation GSDMUserTile
- (id) initWithFrame: (NSRect)frame login: (NSString *)aLogin fullName: (NSString *)aName
{
	CGFloat d = 72;

	self = [super initWithFrame: frame];
	if (self == nil)
		return nil;
	login = [aLogin copy];
	fullName = [aName copy];
	avatar = [[GSDMAvatarView alloc] initWithFrame:
	    NSMakeRect((frame.size.width - d) / 2, frame.size.height - d - 8, d, d)];
	[avatar setName: fullName];
	[avatar setGeneric: login == nil];
	[self addSubview: avatar];
	return self;
}

- (void) dealloc
{
	[avatar release];
	[login release];
	[fullName release];
	[super dealloc];
}

- (NSString *) login
{
	return login;
}

- (NSString *) fullName
{
	return fullName;
}

- (void) setSelected: (BOOL)flag
{
	selected = flag;
	[self setNeedsDisplay: YES];
}

- (void) setTarget: (id)aTarget action: (SEL)anAction
{
	target = aTarget;
	action = anAction;
}

- (void) drawRect: (NSRect)rect
{
	NSRect b = [self bounds];
	NSMutableDictionary *a = [NSMutableDictionary dictionary];
	NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];

	if (selected) {
		NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect: NSInsetRect(b, 3, 3)
		    xRadius: 10 yRadius: 10];

		[rgb(0.22, 0.47, 0.86, 0.22) set];
		[p fill];
		[rgb(0.22, 0.47, 0.86, 0.75) set];
		[p setLineWidth: 1.5];
		[p stroke];
	}
	[ps setAlignment: NSCenterTextAlignment];
	[ps setLineBreakMode: NSLineBreakByTruncatingTail];
	[a setObject: ps forKey: NSParagraphStyleAttributeName];
	[a setObject: [NSFont systemFontOfSize: 13] forKey: NSFontAttributeName];
	[a setObject: rgb(0.10, 0.11, 0.14, 1.0) forKey: NSForegroundColorAttributeName];
	[fullName drawInRect: NSMakeRect(4, 8, b.size.width - 8, 20) withAttributes: a];
	[ps release];
}

- (BOOL) acceptsFirstMouse: (NSEvent *)e
{
	return YES;
}

- (NSView *) hitTest: (NSPoint)p
{
	/* Clicks on the avatar belong to the tile. */
	return NSMouseInRect([self convertPoint: p fromView: [self superview]], [self bounds], NO)
	    ? self : nil;
}

- (void) mouseDown: (NSEvent *)e
{
	GSDMTileGrid *grid = (GSDMTileGrid *)[self superview];

	if ([grid isKindOfClass: [GSDMTileGrid class]])
		[grid selectTile: self];
	if (target != nil && action != NULL)
		[target performSelector: action withObject: self];
}
@end

/* ------------------------------------------------------------------ */

@implementation GSDMTileGrid
- (void) dealloc
{
	[tiles release];
	[super dealloc];
}

- (BOOL) isFlipped
{
	return YES;
}

- (BOOL) acceptsFirstResponder
{
	return YES;
}

- (void) setTarget: (id)aTarget action: (SEL)anAction
{
	target = aTarget;
	action = anAction;
}

- (void) setTiles: (NSArray *)someTiles
{
	NSUInteger i;

	for (i = 0; i < [tiles count]; i++)
		[[tiles objectAtIndex: i] removeFromSuperview];
	ASSIGN(tiles, [NSMutableArray arrayWithArray: someTiles]);
	for (i = 0; i < [tiles count]; i++)
		[self addSubview: [tiles objectAtIndex: i]];
	selection = 0;
	[self selectTile: [tiles count] ? [tiles objectAtIndex: 0] : nil];
}

- (GSDMUserTile *) selectedTile
{
	return selection < [tiles count] ? [tiles objectAtIndex: selection] : nil;
}

- (void) selectTile: (GSDMUserTile *)tile
{
	NSUInteger i;

	for (i = 0; i < [tiles count]; i++) {
		GSDMUserTile *t = [tiles objectAtIndex: i];

		[t setSelected: t == tile];
		if (t == tile)
			selection = i;
	}
}

- (void) keyDown: (NSEvent *)e
{
	NSString *chars = [e charactersIgnoringModifiers];
	unichar c = [chars length] ? [chars characterAtIndex: 0] : 0;
	NSUInteger n = [tiles count];

	if (n == 0) {
		[super keyDown: e];
		return;
	}
	switch (c) {
	case NSLeftArrowFunctionKey:
	case NSUpArrowFunctionKey:
		[self selectTile: [tiles objectAtIndex: (selection + n - 1) % n]];
		break;
	case NSRightArrowFunctionKey:
	case NSDownArrowFunctionKey:
	case NSTabCharacter:
		[self selectTile: [tiles objectAtIndex: (selection + 1) % n]];
		break;
	case NSCarriageReturnCharacter:
	case NSEnterCharacter:
	case '\n':
	case ' ':
		if (target != nil && action != NULL)
			[target performSelector: action withObject: [self selectedTile]];
		break;
	default:
		[super keyDown: e];
	}
}
@end
