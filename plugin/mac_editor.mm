// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <Cocoa/Cocoa.h>
#include "ChronoBentPlugin.h"
void *ChronoBentPlugin::OpenWindow(void *parent) {
    // iPlug2 retains its helper until destruction. Balance that ownership on
    // reopen before its OpenWindow replaces the retained pointer.
    if(WebViewEditorDelegate::mView) {
        NSView *previous=(__bridge_transfer NSView *)WebViewEditorDelegate::mView;
        WebViewEditorDelegate::mView=nullptr;
        [previous removeFromSuperview];
    }
    return WebViewEditorDelegate::OpenWindow(parent);
}
#ifdef AU_API
OSStatus ChronoBentPlugin::SetState(CFPropertyListRef state) {
    if(!state || CFGetTypeID(state)!=CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
    const auto dictionary=static_cast<CFDictionaryRef>(state);
    const auto matches=[&](CFStringRef key,int32_t expected) {
        const auto value=static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary,key)); int32_t number=0;
        return value && CFGetTypeID(value)==CFNumberGetTypeID() && CFNumberGetValue(value,kCFNumberSInt32Type,&number) && number==expected;
    };
    if(!matches(CFSTR("type"),'aumu') || !matches(CFSTR("subtype"),GetUniqueID()) || !matches(CFSTR("manufacturer"),GetMfrID()))
        return kAudioUnitErr_InvalidPropertyValue;
    const auto data=static_cast<CFDataRef>(CFDictionaryGetValue(dictionary,CFSTR("data")));
    if(!data || CFGetTypeID(data)!=CFDataGetTypeID() || CFDataGetLength(data)<8 || CFDataGetLength(data)>185000000)
        return kAudioUnitErr_InvalidPropertyValue;
    IByteChunk chunk; chunk.PutBytes(CFDataGetBytePtr(data),int(CFDataGetLength(data)));
    if(UnserializeState(chunk,0)!=chunk.Size()) return kAudioUnitErr_InvalidPropertyValue;
    OnRestoreState(); return noErr;
}
#endif
