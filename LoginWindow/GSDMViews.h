/*
 * The views LoginWindow draws itself: the full-screen background, the
 * centered panel, user avatars and the user tiles. Controls (buttons and
 * text fields) are plain AppKit ones so they come from the GNUstep theme.
 *
 * Copyright (c) 2026 Anurodh Pokharel. MIT license, see LICENSE.
 */
#import <AppKit/AppKit.h>

/* A borderless window that can still take the keyboard. */
@interface GSDMWindow : NSWindow
@end

/*
 * The Aqua-blue background. If an image is set it is drawn instead,
 * scaled to fill the screen (the configurable background hook).
 */
@interface GSDMBackgroundView : NSView
{
	NSImage *image;
	NSImage *cache;
}
- (void) paint;
- (void) setImage: (NSImage *)anImage;
@end

/* The light, rounded login panel with its title band. */
@interface GSDMPanelView : NSView
{
	NSString *title;
	NSString *subtitle;
	BOOL shadowHidden;
}
- (void) setShadowHidden: (BOOL)flag;
- (void) setTitle: (NSString *)aTitle subtitle: (NSString *)aSubtitle;
+ (CGFloat) headerHeight;
@end

/* A round picture with the user's initials (no account pictures yet). */
@interface GSDMAvatarView : NSView
{
	NSString *name;
	BOOL generic;
}
- (void) setName: (NSString *)aName;
- (void) setGeneric: (BOOL)flag;
@end

/* One user in the list: avatar and name; selectable, clickable. */
@interface GSDMUserTile : NSView
{
	GSDMAvatarView *avatar;
	NSString *login;
	NSString *fullName;
	BOOL selected;
	id target;
	SEL action;
}
- (id) initWithFrame: (NSRect)frame login: (NSString *)aLogin fullName: (NSString *)aName;
- (NSString *) login;
- (NSString *) fullName;
- (void) setSelected: (BOOL)flag;
- (void) setTarget: (id)aTarget action: (SEL)anAction;
@end

/* Holds the tiles; arrow keys move the selection, Return picks it. */
@interface GSDMTileGrid : NSView
{
	NSMutableArray *tiles;
	NSUInteger selection;
	id target;
	SEL action;
}
- (void) setTiles: (NSArray *)someTiles;
- (GSDMUserTile *) selectedTile;
- (void) selectTile: (GSDMUserTile *)tile;
- (void) setTarget: (id)aTarget action: (SEL)anAction;
@end
