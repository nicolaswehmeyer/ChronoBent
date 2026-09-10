// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
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
    double time=0;
    std::array<float,512> left{},right{};
    explicit Host(AudioComponent component,double rate=48000) {
        check(AudioComponentInstanceNew(component,&unit),"instantiate AU");
        AudioStreamBasicDescription format{}; format.mSampleRate=rate; format.mFormatID=kAudioFormatLinearPCM;
        format.mFormatFlags=kAudioFormatFlagsNativeFloatPacked|kAudioFormatFlagIsNonInterleaved;
        format.mBytesPerPacket=format.mBytesPerFrame=4; format.mFramesPerPacket=1; format.mChannelsPerFrame=2; format.mBitsPerChannel=32;
        check(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,sizeof(format)),"set stereo format");
        UInt32 frames=512,offline=1;
        check(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&frames,sizeof(frames)),"reserve host block");
        check(AudioUnitSetProperty(unit,kAudioUnitProperty_OfflineRender,kAudioUnitScope_Global,0,&offline,sizeof(offline)),"set offline render");
        check(AudioUnitInitialize(unit),"initialize AU");
    }
    ~Host() { AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); }
    void render(UInt32 frames=512) {
        struct StereoList { UInt32 count; AudioBuffer buffers[2]; } list{2,{{1,frames*4,left.data()},{1,frames*4,right.data()}}};
        AudioTimeStamp timestamp{}; timestamp.mSampleTime=time; timestamp.mFlags=kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags=0;
        check(AudioUnitRender(unit,&flags,&timestamp,0,frames,reinterpret_cast<AudioBufferList *>(&list)),"render AU");
        time+=frames;
        for(UInt32 i=0;i<frames;++i) require(std::isfinite(left[i]) && std::isfinite(right[i]) && std::abs(left[i])<16,"finite bounded plugin audio");
    }
    void note(int note,int velocity,UInt32 offset=0) { check(MusicDeviceMIDIEvent(unit,velocity?0x90:0x80,UInt32(note),UInt32(velocity),offset),"sample-offset MIDI"); }
};
int main(int argc,char **argv) {
    CFBundleRef bundle=nullptr;
    @autoreleasepool {
        require(argc>=2,"pass exact ChronoBent.component path");
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        bundle=CFBundleCreate(kCFAllocatorDefault,(__bridge CFURLRef)url);
        require(bundle && CFBundleLoadExecutable(bundle),"load exact AU bundle");
        auto factory=reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle,CFSTR("ChronoBent_Factory")));
        require(factory,"AU factory symbol");
        AudioComponentDescription description{'aumu','ChBn','NWeh',0,0};
        const auto component=AudioComponentRegister(&description,CFSTR("Nicolas Wehmeyer: ChronoBent test"),1280,factory);
        require(component,"in-process AU registration");
        {
            Host host(component);
            host.note(60,100,127); host.render();
            double energy=0; for(unsigned i=0;i<512;++i) { if(i<127) require(host.left[i]==0 && host.right[i]==0,"MIDI offset preserves preceding silence"); energy+=host.left[i]*host.left[i]; }
            require(energy>1e-6,"first offline note waits for prepared source");
            const auto first=host.left;
            CFPropertyListRef state=nullptr; UInt32 size=sizeof(state);
            check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&size),"save complete project state");
            require(state && CFGetTypeID(state)==CFDictionaryGetTypeID(),"state dictionary");
            {
                check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,7.25,0),"set value before rejected state");
                CFMutableDictionaryRef corrupt=CFDictionaryCreateMutableCopy(nullptr,0,static_cast<CFDictionaryRef>(state));
                const uint8_t invalid[]{1,2,3}; CFDataRef data=CFDataCreate(nullptr,invalid,sizeof(invalid));
                CFDictionarySetValue(corrupt,CFSTR("data"),data); CFRelease(data);
                CFPropertyListRef broken=corrupt;
                require(AudioUnitSetProperty(host.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&broken,sizeof(broken))!=noErr,"truncated state rejection");
                AudioUnitParameterValue preserved=0;
                check(AudioUnitGetParameter(host.unit,0,kAudioUnitScope_Global,0,&preserved),"read preserved parameter");
                require(std::abs(preserved-7.25)<.001,"rejected state preserves current parameters");
                CFRelease(corrupt);
                check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,0,0),"restore pitch");
            }
            {
                Host restored(component);
                check(AudioUnitSetProperty(restored.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,sizeof(state)),"restore complete project state");
                restored.note(60,100,127); restored.render();
                for(unsigned i=0;i<512;++i) require(std::abs(restored.left[i]-first[i])<1e-7,"state roundtrip audio identity");
            }
            CFRelease(state);
            for(int i=0;i<160;++i) host.render();
            check(AudioUnitReset(host.unit,kAudioUnitScope_Global,0),"transport reset");
            host.render(); for(float v:host.left) require(v==0,"reset clears voices and tails");
            check(AudioUnitSetParameter(host.unit,6,kAudioUnitScope_Global,0,1,0),"loop parameter");
            host.note(60,100); double loopEnergy=0;
            for(int i=0;i<180;++i) { host.render(); if(i>150) for(float v:host.left) loopEnergy+=v*v; }
            require(loopEnergy>.01,"native plugin loops beyond sample end");
            check(AudioUnitSetParameter(host.unit,4,kAudioUnitScope_Global,0,1,0),"release automation");
            host.note(60,0,31); host.render(); host.render();
            for(float v:host.left) require(v==0,"native note-off release");
            if(argc>=3) {
                check(AudioUnitSetParameter(host.unit,4,kAudioUnitScope_Global,0,240,0),"restore release control");
                check(AudioUnitSetParameter(host.unit,6,kAudioUnitScope_Global,0,0,0),"restore loop control");
                AudioUnitCocoaViewInfo info{}; size=sizeof(info);
                check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_CocoaUI,kAudioUnitScope_Global,0,&info,&size),"native editor info");
                NSBundle *viewBundle=[NSBundle bundleWithURL:(__bridge NSURL *)info.mCocoaAUViewBundleLocation];
                require([viewBundle load],"load native editor bundle");
                Class viewClass=NSClassFromString((__bridge NSString *)info.mCocoaAUViewClass[0]);
                id<AUCocoaUIBase> provider=[[viewClass alloc] init];
                NSView *editor=[provider uiViewForAudioUnit:host.unit withSize:NSMakeSize(1040,720)];
                require(editor,"create native AU editor");
                NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1040,720) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                window.releasedWhenClosed=NO;
                window.contentView=editor;
                WKWebView *web=nil;
                for(int i=0;i<1000 && !web;++i) { pump(.01); web=webview(editor); }
                require(web,"native WKWebView exists");
                pump(2);
                require([js(web,@"document.querySelectorAll('.knob').length") intValue]==5,"bundled UI loaded");
                require([js(web,@"document.getElementById('sample-name').textContent") isEqual:@"Glass Circuit"],"native status bridge");
                require([js(web,@"document.getElementById('root-name').textContent") isEqual:@"C4"],"integer parameter normalization");
                require(![js(web,@"document.body.innerText.includes('NaN')") boolValue],"finite native display values");
                js(web,@"gesture(0,true);send(0,7.25);gesture(0,false)");
                AudioUnitParameterValue pitch=0; check(AudioUnitGetParameter(host.unit,0,kAudioUnitScope_Global,0,&pitch),"read UI pitch");
                require(std::abs(pitch-7.25)<.001,"UI normalized parameter bridge");
                js(web,@"gesture(0,true);send(0,0);gesture(0,false);document.getElementById('apply').click()");
                host.note(60,100); host.render(); pump(.3);
                js(web,@"document.getElementById('preset-menu').hidden=false");
                require([js(web,@"document.querySelectorAll('[data-preset]').length") intValue]==3,"three native presets");
                js(web,@"document.getElementById('preset-menu').hidden=true");
                __block bool captured=false;
                WKSnapshotConfiguration *configuration=[WKSnapshotConfiguration new]; configuration.rect=NSMakeRect(0,0,1040,720);
                [web takeSnapshotWithConfiguration:configuration completionHandler:^(NSImage *image,NSError *error) {
                    require(image && !error,"capture native editor");
                    NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc] initWithData:image.TIFFRepresentation];
                    NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                    require([png writeToFile:[NSString stringWithUTF8String:argv[2]] atomically:YES],"write native editor image"); captured=true;
                }];
                for(int i=0;i<1000 && !captured;++i) pump(.01);
                require(captured,"native editor snapshot completed");
                // Detach synchronously while the AU is alive. Closing an
                // NSWindow alone can defer its view teardown until pool drain.
                [editor removeFromSuperview];
                window.contentView=nil;
                [window close];
                for(unsigned reopen=0;reopen<3;++reopen) {
                    @autoreleasepool {
                        NSView *again=[provider uiViewForAudioUnit:host.unit withSize:NSMakeSize(1040,720)];
                        require(again,"reopen native editor"); pump(.3);
                        require(webview(again),"reopened native WebKit view");
                        [again removeFromSuperview];
                    }
                }
                CFRelease(info.mCocoaAUViewBundleLocation); CFRelease(info.mCocoaAUViewClass[0]);
            }
        }
        std::puts("AU host: exact-bundle load, sample-offset MIDI, offline preparation, state/audio roundtrip, loop, automation and reset passed");
    }
    CFRelease(bundle);
}
