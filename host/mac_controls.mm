// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import "mac_controls.hpp"
#include <algorithm>
#define CBNativeSliderCell CB_JOIN(CHRONOBENT_TUNE_OBJC_PREFIX,NativeSliderCell)
@interface CBNativeSliderCell : NSSliderCell
@end
@implementation CBNativeSliderCell
- (void)drawBarInside:(NSRect)rect flipped:(BOOL)flipped {
    (void)flipped;rect.origin.y=NSMidY(rect)-2;rect.size.height=4;
    [[NSColor colorWithCalibratedWhite:.25 alpha:self.enabled?1:.5] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:2 yRadius:2] fill];
    const double span=self.maxValue-self.minValue;
    rect.size.width*=span>0?std::clamp((self.doubleValue-self.minValue)/span,0.,1.):0;
    [[NSColor colorWithCalibratedRed:.48 green:.91 blue:.77 alpha:self.enabled?1:.4] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:2 yRadius:2] fill];
}
- (void)drawKnob:(NSRect)rect {
    rect=NSMakeRect(NSMidX(rect)-6,NSMidY(rect)-6,12,12);
    [[NSColor colorWithCalibratedRed:.81 green:.88 blue:.87 alpha:self.enabled?1:.4] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:rect] fill];
}
@end
@implementation CBNativeSlider
+ (Class)cellClass{return CBNativeSliderCell.class;}
@end
