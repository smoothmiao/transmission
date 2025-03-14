// This file Copyright © 2008-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "TrackerTableView.h"
#import "Torrent.h"
#import "TrackerNode.h"

@implementation TrackerTableView

- (void)mouseDown:(NSEvent*)event
{
    [self.window makeKeyWindow];
    [super mouseDown:event];
}

- (void)setTorrent:(Torrent*)torrent
{
    fTorrent = torrent;
}

- (void)setTrackers:(NSArray*)trackers
{
    fTrackers = trackers;
}

- (void)copy:(id)sender
{
    NSMutableArray* addresses = [NSMutableArray arrayWithCapacity:fTrackers.count];
    NSIndexSet* indexes = self.selectedRowIndexes;
    for (NSUInteger i = indexes.firstIndex; i != NSNotFound; i = [indexes indexGreaterThanIndex:i])
    {
        id item = fTrackers[i];
        if (![item isKindOfClass:[TrackerNode class]])
        {
            for (++i; i < fTrackers.count && [fTrackers[i] isKindOfClass:[TrackerNode class]]; ++i)
            {
                [addresses addObject:((TrackerNode*)fTrackers[i]).fullAnnounceAddress];
            }
            --i;
        }
        else
        {
            [addresses addObject:((TrackerNode*)item).fullAnnounceAddress];
        }
    }

    NSString* text = [addresses componentsJoinedByString:@"\n"];

    NSPasteboard* pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb writeObjects:@[ text ]];
}

- (void)paste:(id)sender
{
    NSAssert(fTorrent != nil, @"no torrent but trying to paste; should not be able to call this method");

    BOOL added = NO;

    NSArray* items = [NSPasteboard.generalPasteboard readObjectsForClasses:@[ [NSString class] ] options:nil];
    NSAssert(items != nil, @"no string items to paste; should not be able to call this method");

    for (NSString* pbItem in items)
    {
        for (NSString* item in [pbItem componentsSeparatedByString:@"\n"])
        {
            if ([fTorrent addTrackerToNewTier:item])
            {
                added = YES;
            }
        }
    }

    //none added
    if (!added)
    {
        NSBeep();
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
    SEL const action = menuItem.action;

    if (action == @selector(copy:))
    {
        return self.numberOfSelectedRows > 0;
    }

    if (action == @selector(paste:))
    {
        return fTorrent && [NSPasteboard.generalPasteboard canReadObjectForClasses:@[ [NSString class] ] options:nil];
    }

    return YES;
}

@end
