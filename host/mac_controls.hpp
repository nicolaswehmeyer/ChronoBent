// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_MAC_CONTROLS_HPP
#define CHRONOBENT_MAC_CONTROLS_HPP
#import <Cocoa/Cocoa.h>
#ifndef CHRONOBENT_TUNE_OBJC_PREFIX
#define CHRONOBENT_TUNE_OBJC_PREFIX ChronoBentLab
#endif
#define CB_JOIN0(a,b) a##b
#define CB_JOIN(a,b) CB_JOIN0(a,b)
#define CBNativeSlider CB_JOIN(CHRONOBENT_TUNE_OBJC_PREFIX,NativeSlider)
// Native AppKit tracking, keyboard and accessibility with deterministic drawing
// shared by Lab and Note Studio. Each bundle uses a distinct Objective-C prefix.
@interface CBNativeSlider : NSSlider
@end
#endif
