// This file Copyright © 2008-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "AddWindowController.h"
#import "Controller.h"
#import "ExpandedPathToIconTransformer.h"
#import "FileOutlineController.h"
#import "GroupsController.h"
#import "NSStringAdditions.h"
#import "Torrent.h"

#define UPDATE_SECONDS 1.0

#define POPUP_PRIORITY_HIGH 0
#define POPUP_PRIORITY_NORMAL 1
#define POPUP_PRIORITY_LOW 2

@interface AddWindowController ()

- (void)updateFiles;

- (void)confirmAdd;

- (void)setDestinationPath:(NSString*)destination determinationType:(TorrentDeterminationType)determinationType;

- (void)setGroupsMenu;
- (void)changeGroupValue:(id)sender;

@end

@implementation AddWindowController

- (instancetype)initWithTorrent:(Torrent*)torrent
                          destination:(NSString*)path
                      lockDestination:(BOOL)lockDestination
                           controller:(Controller*)controller
                          torrentFile:(NSString*)torrentFile
    deleteTorrentCheckEnableInitially:(BOOL)deleteTorrent
                      canToggleDelete:(BOOL)canToggleDelete
{
    if ((self = [super initWithWindowNibName:@"AddWindow"]))
    {
        fTorrent = torrent;
        fDestination = path.stringByExpandingTildeInPath;
        fLockDestination = lockDestination;

        fController = controller;

        fTorrentFile = torrentFile.stringByExpandingTildeInPath;

        fDeleteTorrentEnableInitially = deleteTorrent;
        fCanToggleDelete = canToggleDelete;

        fGroupValue = torrent.groupValue;
        fGroupValueDetermination = TorrentDeterminationAutomatic;

        fVerifyIndicator.usesThreadedAnimation = YES;
    }
    return self;
}

- (void)awakeFromNib
{
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateCheckButtons:) name:@"TorrentFileCheckChange"
                                             object:fTorrent];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateGroupMenu:) name:@"UpdateGroups" object:nil];

    [fFileController setTorrent:fTorrent];

    NSString* name = fTorrent.name;
    self.window.title = name;
    fNameField.stringValue = name;
    fNameField.toolTip = name;

    fIconView.image = fTorrent.icon;

    if (!fTorrent.folder)
    {
        fFileFilterField.hidden = YES;
        fCheckAllButton.hidden = YES;
        fUncheckAllButton.hidden = YES;

        NSRect scrollFrame = fFileScrollView.frame;
        CGFloat const diff = NSMinY(fFileScrollView.frame) - NSMinY(fFileFilterField.frame);
        scrollFrame.origin.y -= diff;
        scrollFrame.size.height += diff;
        fFileScrollView.frame = scrollFrame;
    }
    else
    {
        [self updateCheckButtons:nil];
    }

    [self setGroupsMenu];
    [fGroupPopUp selectItemWithTag:fGroupValue];

    NSInteger priorityIndex;
    switch (fTorrent.priority)
    {
    case TR_PRI_HIGH:
        priorityIndex = POPUP_PRIORITY_HIGH;
        break;
    case TR_PRI_NORMAL:
        priorityIndex = POPUP_PRIORITY_NORMAL;
        break;
    case TR_PRI_LOW:
        priorityIndex = POPUP_PRIORITY_LOW;
        break;
    default:
        NSAssert1(NO, @"Unknown priority for adding torrent: %d", fTorrent.priority);
        priorityIndex = POPUP_PRIORITY_NORMAL;
    }
    [fPriorityPopUp selectItemAtIndex:priorityIndex];

    fStartCheck.state = [NSUserDefaults.standardUserDefaults boolForKey:@"AutoStartDownload"] ? NSControlStateValueOn
                                                                                              : NSControlStateValueOff;

    fDeleteCheck.state = fDeleteTorrentEnableInitially ? NSControlStateValueOn : NSControlStateValueOff;
    fDeleteCheck.enabled = fCanToggleDelete;

    if (fDestination)
    {
        [self setDestinationPath:fDestination
               determinationType:(fLockDestination ? TorrentDeterminationUserSpecified : TorrentDeterminationAutomatic)];
    }
    else
    {
        fLocationField.stringValue = @"";
        fLocationImageView.image = nil;
    }

    fTimer = [NSTimer scheduledTimerWithTimeInterval:UPDATE_SECONDS target:self selector:@selector(updateFiles) userInfo:nil
                                             repeats:YES];
    [self updateFiles];
}

- (void)windowDidLoad
{
    //if there is no destination, prompt for one right away
    if (!fDestination)
    {
        [self setDestination:nil];
    }
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];

    [fTimer invalidate];
}

- (Torrent*)torrent
{
    return fTorrent;
}

- (void)setDestination:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];

    panel.prompt = NSLocalizedString(@"Select", "Open torrent -> prompt");
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;

    panel.message = [NSString stringWithFormat:NSLocalizedString(@"Select the download folder for \"%@\"", "Add -> select destination folder"),
                                               fTorrent.name];

    [panel beginSheetModalForWindow:self.window completionHandler:^(NSInteger result) {
        if (result == NSModalResponseOK)
        {
            fLockDestination = YES;
            [self setDestinationPath:panel.URLs[0].path determinationType:TorrentDeterminationUserSpecified];
        }
        else
        {
            if (!fDestination)
            {
                [self performSelectorOnMainThread:@selector(cancelAdd:) withObject:nil waitUntilDone:NO];
            }
        }
    }];
}

- (void)add:(id)sender
{
    if ([fDestination.lastPathComponent isEqualToString:fTorrent.name] &&
        [NSUserDefaults.standardUserDefaults boolForKey:@"WarningFolderDataSameName"])
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NSLocalizedString(@"The destination directory and root data directory have the same name.", "Add torrent -> same name -> title");
        alert.informativeText = NSLocalizedString(
            @"If you are attempting to use already existing data,"
             " the root data directory should be inside the destination directory.",
            "Add torrent -> same name -> message");
        alert.alertStyle = NSAlertStyleWarning;
        [alert addButtonWithTitle:NSLocalizedString(@"Cancel", "Add torrent -> same name -> button")];
        [alert addButtonWithTitle:NSLocalizedString(@"Add", "Add torrent -> same name -> button")];
        alert.showsSuppressionButton = YES;

        [alert beginSheetModalForWindow:[self window] completionHandler:^(NSModalResponse returnCode) {
            if (alert.suppressionButton.state == NSControlStateValueOn)
            {
                [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"WarningFolderDataSameName"];
            }

            if (returnCode == NSAlertSecondButtonReturn)
            {
                [self performSelectorOnMainThread:@selector(confirmAdd) withObject:nil waitUntilDone:NO];
            }
        }];
    }
    else
    {
        [self confirmAdd];
    }
}

- (void)cancelAdd:(id)sender
{
    [self.window performClose:sender];
}

//only called on cancel
- (BOOL)windowShouldClose:(id)window
{
    [fTimer invalidate];
    fTimer = nil;

    [fFileController setTorrent:nil]; //avoid a crash when window tries to update

    [fController askOpenConfirmed:self add:NO];
    return YES;
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

- (void)verifyLocalData:(id)sender
{
    [fTorrent resetCache];
    [self updateFiles];
}

- (void)changePriority:(id)sender
{
    tr_priority_t priority;
    switch ([sender indexOfSelectedItem])
    {
    case POPUP_PRIORITY_HIGH:
        priority = TR_PRI_HIGH;
        break;
    case POPUP_PRIORITY_NORMAL:
        priority = TR_PRI_NORMAL;
        break;
    case POPUP_PRIORITY_LOW:
        priority = TR_PRI_LOW;
        break;
    default:
        NSAssert1(NO, @"Unknown priority tag for adding torrent: %ld", [sender tag]);
        priority = TR_PRI_NORMAL;
    }
    fTorrent.priority = priority;
}

- (void)updateCheckButtons:(NSNotification*)notification
{
    NSString* statusString = [NSString stringForFileSize:fTorrent.size];
    if (fTorrent.folder)
    {
        //check buttons
        //keep synced with identical code in InfoFileViewController.m
        NSInteger const filesCheckState = [fTorrent
            checkForFiles:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, fTorrent.fileCount)]];
        fCheckAllButton.enabled = filesCheckState != NSControlStateValueOn; //if anything is unchecked
        fUncheckAllButton.enabled = !fTorrent.allDownloaded; //if there are any checked files that aren't finished

        //status field
        NSString* fileString;
        NSInteger count = fTorrent.fileCount;
        if (count != 1)
        {
            fileString = [NSString
                stringWithFormat:NSLocalizedString(@"%@ files", "Add torrent -> info"), [NSString formattedUInteger:count]];
        }
        else
        {
            fileString = NSLocalizedString(@"1 file", "Add torrent -> info");
        }

        NSString* selectedString = [NSString stringWithFormat:NSLocalizedString(@"%@ selected", "Add torrent -> info"),
                                                              [NSString stringForFileSize:fTorrent.totalSizeSelected]];

        statusString = [NSString stringWithFormat:@"%@, %@ (%@)", fileString, statusString, selectedString];
    }

    fStatusField.stringValue = statusString;
}

- (void)updateGroupMenu:(NSNotification*)notification
{
    [self setGroupsMenu];
    if (![fGroupPopUp selectItemWithTag:fGroupValue])
    {
        fGroupValue = -1;
        fGroupValueDetermination = TorrentDeterminationAutomatic;
        [fGroupPopUp selectItemWithTag:fGroupValue];
    }
}

- (void)updateFiles
{
    [fTorrent update];

    [fFileController refresh];

    [self updateCheckButtons:nil]; //call in case button state changed by checking

    if (fTorrent.checking)
    {
        BOOL const waiting = fTorrent.checkingWaiting;
        fVerifyIndicator.indeterminate = waiting;
        if (waiting)
        {
            [fVerifyIndicator startAnimation:self];
        }
        else
        {
            fVerifyIndicator.doubleValue = fTorrent.checkingProgress;
        }
    }
    else
    {
        fVerifyIndicator.indeterminate = YES; //we want to hide when stopped, which only applies when indeterminate
        [fVerifyIndicator stopAnimation:self];
    }
}

- (void)confirmAdd
{
    [fTimer invalidate];
    fTimer = nil;
    [fTorrent setGroupValue:fGroupValue determinationType:fGroupValueDetermination];

    if (fTorrentFile && fCanToggleDelete && fDeleteCheck.state == NSControlStateValueOn)
    {
        [Torrent trashFile:fTorrentFile error:nil];
    }

    if (fStartCheck.state == NSControlStateValueOn)
    {
        [fTorrent startTransfer];
    }

    [fFileController setTorrent:nil]; //avoid a crash when window tries to update

    [self close];
    [fController askOpenConfirmed:self add:YES];
}

- (void)setDestinationPath:(NSString*)destination determinationType:(TorrentDeterminationType)determinationType
{
    destination = destination.stringByExpandingTildeInPath;
    if (!fDestination || ![fDestination isEqualToString:destination])
    {
        fDestination = destination;

        [fTorrent changeDownloadFolderBeforeUsing:fDestination determinationType:determinationType];
    }

    fLocationField.stringValue = fDestination.stringByAbbreviatingWithTildeInPath;
    fLocationField.toolTip = fDestination;

    ExpandedPathToIconTransformer* iconTransformer = [[ExpandedPathToIconTransformer alloc] init];
    fLocationImageView.image = [iconTransformer transformedValue:fDestination];
}

- (void)setGroupsMenu
{
    NSMenu* groupMenu = [GroupsController.groups groupMenuWithTarget:self action:@selector(changeGroupValue:) isSmall:NO];
    fGroupPopUp.menu = groupMenu;
}

- (void)changeGroupValue:(id)sender
{
    NSInteger previousGroup = fGroupValue;
    fGroupValue = [sender tag];
    fGroupValueDetermination = TorrentDeterminationUserSpecified;

    if (!fLockDestination)
    {
        if ([GroupsController.groups usesCustomDownloadLocationForIndex:fGroupValue])
        {
            [self setDestinationPath:[GroupsController.groups customDownloadLocationForIndex:fGroupValue]
                   determinationType:TorrentDeterminationAutomatic];
        }
        else if ([fDestination isEqualToString:[GroupsController.groups customDownloadLocationForIndex:previousGroup]])
        {
            [self setDestinationPath:[NSUserDefaults.standardUserDefaults stringForKey:@"DownloadFolder"]
                   determinationType:TorrentDeterminationAutomatic];
        }
    }
}

@end
