// This file Copyright © 2010-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "InfoFileViewController.h"
#import "FileListNode.h"
#import "FileOutlineController.h"
#import "FileOutlineView.h"
#import "Torrent.h"

@interface InfoFileViewController ()

- (void)setupInfo;

- (BOOL)canQuickLookFile:(FileListNode*)item;

@end

@implementation InfoFileViewController

- (instancetype)init
{
    if ((self = [super initWithNibName:@"InfoFileView" bundle:nil]))
    {
        self.title = NSLocalizedString(@"Files", "Inspector view -> title");
    }

    return self;
}

- (void)awakeFromNib
{
    CGFloat const height = [NSUserDefaults.standardUserDefaults floatForKey:@"InspectorContentHeightFiles"];
    if (height != 0.0)
    {
        NSRect viewRect = self.view.frame;
        viewRect.size.height = height;
        self.view.frame = viewRect;
    }

    [fFileFilterField.cell setPlaceholderString:NSLocalizedString(@"Filter", "inspector -> file filter")];

    //localize and place all and none buttons
    fCheckAllButton.title = NSLocalizedString(@"All", "inspector -> check all");
    fUncheckAllButton.title = NSLocalizedString(@"None", "inspector -> check all");

    NSRect checkAllFrame = fCheckAllButton.frame;
    NSRect uncheckAllFrame = fUncheckAllButton.frame;
    CGFloat const oldAllWidth = checkAllFrame.size.width;
    CGFloat const oldNoneWidth = uncheckAllFrame.size.width;

    [fCheckAllButton sizeToFit];
    [fUncheckAllButton sizeToFit];
    CGFloat const newWidth = MAX(fCheckAllButton.bounds.size.width, fUncheckAllButton.bounds.size.width);

    CGFloat const uncheckAllChange = newWidth - oldNoneWidth;
    uncheckAllFrame.size.width = newWidth;
    uncheckAllFrame.origin.x -= uncheckAllChange;
    fUncheckAllButton.frame = uncheckAllFrame;

    CGFloat const checkAllChange = newWidth - oldAllWidth;
    checkAllFrame.size.width = newWidth;
    checkAllFrame.origin.x -= (checkAllChange + uncheckAllChange);
    fCheckAllButton.frame = checkAllFrame;
}

- (void)setInfoForTorrents:(NSArray*)torrents
{
    //don't check if it's the same in case the metadata changed
    fTorrents = torrents;

    fSet = NO;
}

- (void)updateInfo
{
    if (!fSet)
    {
        [self setupInfo];
    }

    if (fTorrents.count == 1)
    {
        [fFileController refresh];

#warning use TorrentFileCheckChange notification as well
        Torrent* torrent = fTorrents[0];
        if (torrent.folder)
        {
            NSInteger const filesCheckState = [torrent
                checkForFiles:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, torrent.fileCount)]];
            fCheckAllButton.enabled = filesCheckState != NSControlStateValueOn; //if anything is unchecked
            fUncheckAllButton.enabled = !torrent.allDownloaded; //if there are any checked files that aren't finished
        }
    }
}

- (void)saveViewSize
{
    [NSUserDefaults.standardUserDefaults setFloat:NSHeight(self.view.frame) forKey:@"InspectorContentHeightFiles"];
}

- (void)setFileFilterText:(id)sender
{
    [fFileController setFilterText:[sender stringValue]];
}

- (IBAction)checkAll:(id)sender
{
    [fFileController checkAll];
}

- (IBAction)uncheckAll:(id)sender
{
    [fFileController uncheckAll];
}

- (NSArray*)quickLookURLs
{
    FileOutlineView* fileOutlineView = fFileController.outlineView;
    Torrent* torrent = fTorrents[0];
    NSIndexSet* indexes = fileOutlineView.selectedRowIndexes;
    NSMutableArray* urlArray = [NSMutableArray arrayWithCapacity:indexes.count];

    for (NSUInteger i = indexes.firstIndex; i != NSNotFound; i = [indexes indexGreaterThanIndex:i])
    {
        FileListNode* item = [fileOutlineView itemAtRow:i];
        if ([self canQuickLookFile:item])
        {
            [urlArray addObject:[NSURL fileURLWithPath:[torrent fileLocation:item]]];
        }
    }

    return urlArray;
}

- (BOOL)canQuickLook
{
    if (fTorrents.count != 1)
    {
        return NO;
    }

    Torrent* torrent = fTorrents[0];
    if (!torrent.folder)
    {
        return NO;
    }

    FileOutlineView* fileOutlineView = fFileController.outlineView;
    NSIndexSet* indexes = fileOutlineView.selectedRowIndexes;

    for (NSUInteger i = indexes.firstIndex; i != NSNotFound; i = [indexes indexGreaterThanIndex:i])
    {
        if ([self canQuickLookFile:[fileOutlineView itemAtRow:i]])
        {
            return YES;
        }
    }

    return NO;
}

- (NSRect)quickLookSourceFrameForPreviewItem:(id<QLPreviewItem>)item
{
    FileOutlineView* fileOutlineView = fFileController.outlineView;

    NSString* fullPath = ((NSURL*)item).path;
    Torrent* torrent = fTorrents[0];
    NSRange visibleRows = [fileOutlineView rowsInRect:fileOutlineView.bounds];

    for (NSUInteger row = visibleRows.location; row < NSMaxRange(visibleRows); row++)
    {
        FileListNode* rowItem = [fileOutlineView itemAtRow:row];
        if ([[torrent fileLocation:rowItem] isEqualToString:fullPath])
        {
            NSRect frame = [fileOutlineView iconRectForRow:row];

            if (!NSIntersectsRect(fileOutlineView.visibleRect, frame))
            {
                return NSZeroRect;
            }

            frame.origin = [fileOutlineView convertPoint:frame.origin toView:nil];
            frame = [self.view.window convertRectToScreen:frame];
            frame.origin.y -= frame.size.height;
            return frame;
        }
    }

    return NSZeroRect;
}

- (void)setupInfo
{
    fFileFilterField.stringValue = @"";

    if (fTorrents.count == 1)
    {
        Torrent* torrent = fTorrents[0];

        [fFileController setTorrent:torrent];

        BOOL const isFolder = torrent.folder;
        fFileFilterField.enabled = isFolder;

        if (!isFolder)
        {
            fCheckAllButton.enabled = NO;
            fUncheckAllButton.enabled = NO;
        }
    }
    else
    {
        [fFileController setTorrent:nil];

        fFileFilterField.enabled = NO;

        fCheckAllButton.enabled = NO;
        fUncheckAllButton.enabled = NO;
    }

    fSet = YES;
}

- (BOOL)canQuickLookFile:(FileListNode*)item
{
    Torrent* torrent = fTorrents[0];
    return (item.isFolder || [torrent fileProgress:item] >= 1.0) && [torrent fileLocation:item];
}

@end
