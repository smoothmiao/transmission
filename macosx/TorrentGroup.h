// This file Copyright © 2008-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

@interface TorrentGroup : NSObject
{
    NSInteger fGroup;
    NSMutableArray* fTorrents;
}

- (instancetype)initWithGroup:(NSInteger)group;

@property(nonatomic, readonly) NSInteger groupIndex;
@property(nonatomic, readonly) NSInteger groupOrderValue;
@property(nonatomic, readonly) NSMutableArray* torrents;

@property(nonatomic, readonly) CGFloat ratio;
@property(nonatomic, readonly) CGFloat uploadRate;
@property(nonatomic, readonly) CGFloat downloadRate;

@end
