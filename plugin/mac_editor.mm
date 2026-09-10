// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <Cocoa/Cocoa.h>
#include "host.hpp"
#include <array>
#include <cstring>
#include <vector>
using namespace iplug;
void *ChronoBentHost::OpenWindow(void *parent) {
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
OSStatus ChronoBentHost::SetState(CFPropertyListRef state) {
    if(!state || CFGetTypeID(state)!=CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
    const auto dictionary=static_cast<CFDictionaryRef>(state);
    const auto matches=[&](CFStringRef key,int32_t expected) {
        const auto value=static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary,key)); int32_t number=0;
        return value && CFGetTypeID(value)==CFNumberGetTypeID() && CFNumberGetValue(value,kCFNumberSInt32Type,&number) && number==expected;
    };
    if(!matches(CFSTR("type"),PLUG_TYPE==1?'aumu':'aufx') || !matches(CFSTR("subtype"),GetUniqueID()) || !matches(CFSTR("manufacturer"),GetMfrID()))
        return kAudioUnitErr_InvalidPropertyValue;
    const auto data=static_cast<CFDataRef>(CFDictionaryGetValue(dictionary,CFSTR("data")));
    if(!data || CFGetTypeID(data)!=CFDataGetTypeID() || CFDataGetLength(data)<8 || CFDataGetLength(data)>StateLimit())
        return kAudioUnitErr_InvalidPropertyValue;
    IByteChunk chunk; chunk.PutBytes(CFDataGetBytePtr(data),int(CFDataGetLength(data)));
    if(UnserializeState(chunk,0)!=chunk.Size()) return kAudioUnitErr_InvalidPropertyValue;
    OnRestoreState(); return noErr;
}
#endif
bool ChronoBentHost::CanNavigateToURL(const char *url) {
    return url && (std::strncmp(url,"file://",7)==0 || std::strcmp(url,"about:blank")==0);
}
#ifdef VST3_API
Steinberg::tresult PLUGIN_API ChronoBentHost::setState(Steinberg::IBStream *stream) {
    if(!stream) return Steinberg::kResultFalse;
    try {
        std::vector<uint8_t> data; std::array<uint8_t,4096> block{};
        for(;;) {
            Steinberg::int32 got=0;
            const auto status=stream->read(block.data(),int(block.size()),&got);
            if(got<0 || got>int(block.size()) || data.size()+std::size_t(got)>std::size_t(StateLimit())) return Steinberg::kResultFalse;
            data.insert(data.end(),block.begin(),block.begin()+got);
            if(!got || status!=Steinberg::kResultOk) break;
        }
        if(data.size()<12) return Steinberg::kResultFalse;
        int32_t bypass=0; std::memcpy(&bypass,data.data()+data.size()-4,4);
        if(bypass!=0 && bypass!=1) return Steinberg::kResultFalse;
        IByteChunk chunk; chunk.PutBytes(data.data(),int(data.size()));
        if(UnserializeState(chunk,0)!=int(data.size())-4) return Steinberg::kResultFalse;
        UpdateParams(this,bypass); OnRestoreState(); return Steinberg::kResultOk;
    } catch(...) { return Steinberg::kResultFalse; }
}
#endif
