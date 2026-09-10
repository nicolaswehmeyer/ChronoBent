// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#pragma once
#import <Cocoa/Cocoa.h>
#import <AudioUnit/AudioUnit.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
static void require(bool ok,const char *message) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
static void check(OSStatus status,const char *message) {
    if(status) { std::fprintf(stderr,"FAIL: %s (%d)\n",message,int(status)); std::exit(1); }
}
static void pump(double seconds) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(end.timeIntervalSinceNow>0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
static WKWebView *webview(NSView *view) {
    if([view isKindOfClass:WKWebView.class]) return (WKWebView *)view;
    for(NSView *child in view.subviews) if(auto result=webview(child)) return result;
    return nil;
}
static id js(WKWebView *view,NSString *script) {
    __block bool done=false; __block id result=nil; __block NSError *failure=nil;
    [view evaluateJavaScript:script completionHandler:^(id value,NSError *error) { result=value; failure=error; done=true; }];
    for(int i=0;i<1000 && !done;++i) pump(.01);
    if(failure) std::fprintf(stderr,"JavaScript: %s\n",failure.localizedDescription.UTF8String);
    require(done && !failure,"native WebKit script"); return result;
}
struct Host {
    AudioUnit unit=nullptr;
    double time=0,rate=48000;
    int input_channels=0,output_channels=2;
    float (*signal)(double,unsigned,void *)=nullptr;
    void *signal_context=nullptr;
    std::array<float,512> left{},right{};
    static OSStatus input(void *context,AudioUnitRenderActionFlags *,const AudioTimeStamp *time,
                          UInt32,UInt32 frames,AudioBufferList *buffers) {
        auto &host=*static_cast<Host *>(context);
        for(UInt32 c=0;c<buffers->mNumberBuffers;++c) {
            if(!buffers->mBuffers[c].mData) return kAudioUnitErr_InvalidPropertyValue;
            auto samples=static_cast<float *>(buffers->mBuffers[c].mData);
            for(UInt32 i=0;i<frames;++i) samples[i]=host.signal?host.signal(time->mSampleTime+i,c,host.signal_context):0;
        }
        return noErr;
    }
    explicit Host(AudioComponent component,double sample_rate=48000,int incoming=0,int outgoing=2):
        rate(sample_rate),input_channels(incoming),output_channels(outgoing) {
        check(AudioComponentInstanceNew(component,&unit),"instantiate AU");
        AudioStreamBasicDescription format{}; format.mSampleRate=rate; format.mFormatID=kAudioFormatLinearPCM;
        format.mFormatFlags=kAudioFormatFlagsNativeFloatPacked|kAudioFormatFlagIsNonInterleaved;
        format.mBytesPerPacket=format.mBytesPerFrame=4; format.mFramesPerPacket=1; format.mChannelsPerFrame=UInt32(outgoing); format.mBitsPerChannel=32;
        check(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,sizeof(format)),"set stereo format");
        if(incoming) {
            format.mChannelsPerFrame=UInt32(incoming);
            check(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format)),"set input format");
            AURenderCallbackStruct callback{input,this};
            check(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback)),"connect host input");
        }
        UInt32 frames=512,offline=1;
        check(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&frames,sizeof(frames)),"reserve host block");
        check(AudioUnitSetProperty(unit,kAudioUnitProperty_OfflineRender,kAudioUnitScope_Global,0,&offline,sizeof(offline)),"set offline render");
        check(AudioUnitInitialize(unit),"initialize AU");
    }
    ~Host() { AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); }
    void render(UInt32 frames=512) {
        struct StereoList { UInt32 count; AudioBuffer buffers[2]; } list{UInt32(output_channels),{{1,frames*4,left.data()},{1,frames*4,right.data()}}};
        AudioTimeStamp timestamp{}; timestamp.mSampleTime=time; timestamp.mFlags=kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags=0;
        check(AudioUnitRender(unit,&flags,&timestamp,0,frames,reinterpret_cast<AudioBufferList *>(&list)),"render AU");
        time+=frames;
        for(UInt32 i=0;i<frames;++i) require(std::isfinite(left[i]) && std::isfinite(right[i]) && std::abs(left[i])<16,"finite bounded plugin audio");
    }
    void note(int note,int velocity,UInt32 offset=0) { check(MusicDeviceMIDIEvent(unit,velocity?0x90:0x80,UInt32(note),UInt32(velocity),offset),"sample-offset MIDI"); }
};
