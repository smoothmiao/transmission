// This file Copyright © 2008-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h> // tr_getRatio()

#import "TorrentGroup.h"
#import "GroupsController.h"
#import "Torrent.h"

@implementation TorrentGroup

- (instancetype)initWithGroup:(NSInteger)group
{
    if ((self = [super init]))
    {
        fGroup = group;
        fTorrents = [[NSMutableArray alloc] init];
    }
    return self;
}

- (NSString*)description
{
    return [NSString stringWithFormat:@"Torrent Group %ld: %@", fGroup, fTorrents];
}

- (NSInteger)groupIndex
{
    return fGroup;
}

- (NSInteger)groupOrderValue
{
    return [GroupsController.groups rowValueForIndex:fGroup];
}

- (NSMutableArray*)torrents
{
    return fTorrents;
}

- (CGFloat)ratio
{
    uint64_t uploaded = 0, downloaded = 0;
    for (Torrent* torrent in fTorrents)
    {
        uploaded += torrent.uploadedTotal;
        downloaded += torrent.downloadedTotal;
    }

    return tr_getRatio(uploaded, downloaded);
}

- (CGFloat)uploadRate
{
    CGFloat rate = 0.0;
    for (Torrent* torrent in fTorrents)
    {
        rate += torrent.uploadRate;
    }

    return rate;
}

- (CGFloat)downloadRate
{
    CGFloat rate = 0.0;
    for (Torrent* torrent in fTorrents)
    {
        rate += torrent.downloadRate;
    }

    return rate;
}

@end
