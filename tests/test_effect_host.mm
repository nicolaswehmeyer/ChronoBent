// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "plugin_host_fixture.hpp"
static float input_wave(double frame,unsigned channel,void *context) {
    const double rate=*static_cast<double *>(context);
    const double envelope=.35+.65*(.5+.5*std::cos(6.283185307179586*3.7*frame/rate));
    return float((channel?.13:.2)*envelope*std::sin(6.283185307179586*(channel?220:440)*frame/rate));
}
static double frequency(const std::vector<float> &samples,std::size_t first,std::size_t end,double rate) {
    double begin=0,last=0; unsigned crossings=0;
    for(std::size_t i=first+1;i<end;++i) if(samples[i-1]<=0 && samples[i]>0) {
        const double at=double(i-1)-samples[i-1]/double(samples[i]-samples[i-1]);
        if(!crossings) begin=at; last=at; ++crossings;
    }
    require(crossings>5,"audible effect signal"); return double(crossings-1)*rate/(last-begin);
}
static std::vector<float> render(Host &host,unsigned frames) {
    std::vector<float> output; output.reserve(frames);
    while(frames) { const auto count=std::min(frames,UInt32(512)); host.render(count); output.insert(output.end(),host.left.begin(),host.left.begin()+count); frames-=count; }
    return output;
}
static unsigned latency(Host &host) {
    Float64 seconds=0; UInt32 size=sizeof(seconds);
    check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_Latency,kAudioUnitScope_Global,0,&seconds,&size),"read host latency");
    require(seconds>0 && seconds<1,"bounded declared effect latency"); return unsigned(std::llround(seconds*host.rate));
}
int main(int argc,char **argv) {
    CFBundleRef bundle=nullptr;
    @autoreleasepool {
        require(argc>=2,"pass exact ChronoBentFX.component path");
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        bundle=CFBundleCreate(kCFAllocatorDefault,(__bridge CFURLRef)url);
        require(bundle && CFBundleLoadExecutable(bundle),"load exact FX bundle");
        auto factory=reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle,CFSTR("ChronoBentFX_Factory")));
        require(factory,"effect factory symbol");
        AudioComponentDescription description{'aufx','ChFx','NWeh',0,0};
        const auto component=AudioComponentRegister(&description,CFSTR("Nicolas Wehmeyer: ChronoBent FX test"),1280,factory);
        require(component,"effect type registration");
        for(int channels:{1,2}) {
            Host host(component,48000,channels,channels); host.signal=input_wave; host.signal_context=&host.rate;
            const auto delay=latency(host);
            for(unsigned first=0;first<delay+4096;first+=512) {
                host.render();
                for(unsigned i=0;i<512;++i) {
                    const auto frame=first+i;
                    require(host.left[i]==(frame<delay?0:input_wave(frame-delay,0,&host.rate)),"mono/stereo input preserves exact unity and latency");
                    if(channels==2) require(host.right[i]==(frame<delay?0:input_wave(frame-delay,1,&host.rate)),"stereo channels remain independent");
                }
            }
            UInt32 bypass=1;
            check(AudioUnitSetProperty(host.unit,kAudioUnitProperty_BypassEffect,kAudioUnitScope_Global,0,&bypass,sizeof(bypass)),"host bypass on");
            render(host,delay+4096);
            bypass=0; host.signal=nullptr;
            check(AudioUnitSetProperty(host.unit,kAudioUnitProperty_BypassEffect,kAudioUnitScope_Global,0,&bypass,sizeof(bypass)),"host bypass off");
            const auto resumed=render(host,delay+4096);
            for(float value:resumed) require(value==0,"bypass resume clears paused source audio");
        }
        {
            Host host(component,48000,2,2); host.signal=input_wave; host.signal_context=&host.rate;
            const auto delay=latency(host);
            check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,12,0),"live pitch parameter");
            const auto pitched=render(host,delay+48000);
            require(std::abs(frequency(pitched,delay+12000,pitched.size()-512,host.rate)-880)<3,"native live pitch frequency");
            check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,0,0),"capture unity pitch");
            check(AudioUnitSetParameter(host.unit,5,kAudioUnitScope_Global,0,1,0),"start capture");
            render(host,48000);
            check(AudioUnitSetParameter(host.unit,5,kAudioUnitScope_Global,0,2,0),"stop capture and loop");
            Float64 tail=0; UInt32 tail_size=sizeof(tail);
            check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_TailTime,kAudioUnitScope_Global,0,&tail,&tail_size),"captured loop tail metadata");
            require(std::isinf(tail) && tail>0,"captured loop declares an infinite tail");
            check(AudioUnitSetParameter(host.unit,1,kAudioUnitScope_Global,0,.75,0),"captured time parameter");
            host.signal=nullptr; const auto captured=render(host,delay+48000);
            require(std::abs(frequency(captured,delay+12000,captured.size()-512,host.rate)-440)<3,"capture keeps pitch independent of time");
            CFPropertyListRef state=nullptr; UInt32 size=sizeof(state);
            check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&size),"save captured effect project");
            {
                Host restored(component,48000,2,2);
                check(AudioUnitSetProperty(restored.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,sizeof(state)),"restore captured effect project");
                const auto playback=render(restored,delay+48000);
                require(std::abs(frequency(playback,delay+12000,playback.size()-512,restored.rate)-440)<3,"embedded capture plays without original input");
                Host duplicate(component,48000,2,2);
                check(AudioUnitSetProperty(duplicate.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,sizeof(state)),"second state restore");
                require(render(duplicate,delay+48000)==playback,"restored effect state has deterministic audio");
            }
            check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,7.25,0),"parameter before malformed state");
            CFMutableDictionaryRef corrupt=CFDictionaryCreateMutableCopy(nullptr,0,static_cast<CFDictionaryRef>(state));
            const uint8_t bad[]{1,2,3}; CFDataRef bytes=CFDataCreate(nullptr,bad,sizeof(bad));
            CFDictionarySetValue(corrupt,CFSTR("data"),bytes); CFRelease(bytes);
            CFPropertyListRef broken=corrupt;
            require(AudioUnitSetProperty(host.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&broken,sizeof(broken))!=noErr,"malformed effect state refusal");
            AudioUnitParameterValue preserved=0;
            check(AudioUnitGetParameter(host.unit,0,kAudioUnitScope_Global,0,&preserved),"preserved effect parameter");
            require(std::abs(preserved-7.25)<.001,"malformed effect state is transactional");
            CFRelease(corrupt); CFRelease(state);
            check(AudioUnitSetParameter(host.unit,5,kAudioUnitScope_Global,0,0,0),"return to live input");
            check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,0,0),"reset pitch");
            check(AudioUnitReset(host.unit,kAudioUnitScope_Global,0),"reset effect transport");
            for(float value:render(host,delay+4096)) require(value==0,"reset clears every wet and dry tail");
            if(argc>=3) {
                for(int reopen=0;reopen<3;++reopen) {
                    AudioUnitCocoaViewInfo info{}; size=sizeof(info);
                    check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_CocoaUI,kAudioUnitScope_Global,0,&info,&size),"effect editor metadata");
                    NSBundle *viewBundle=[NSBundle bundleWithURL:(__bridge NSURL *)info.mCocoaAUViewBundleLocation];
                    require([viewBundle load],"load native effect editor");
                    Class cls=NSClassFromString((__bridge NSString *)info.mCocoaAUViewClass[0]);
                    id<AUCocoaUIBase> provider=[[cls alloc] init];
                    NSView *editor=[provider uiViewForAudioUnit:host.unit withSize:NSMakeSize(1040,720)];
                    require(editor,"open effect editor");
                    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1040,720) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                    window.releasedWhenClosed=NO; window.contentView=editor;
                    WKWebView *web=nil;
                    for(int i=0;i<1000 && !web;++i) { pump(.01); web=webview(editor); }
                    require(web,"native effect WebKit view"); pump(1);
                    require([js(web,@"document.querySelectorAll('.knob').length") intValue]==5,"five effect controls loaded");
                    require([js(web,@"document.getElementById('source-name').textContent") isEqual:@"Live input"],"real effect status bridge");
                    require([js(web,@"document.getElementById('time-control').classList.contains('inactive')") boolValue],"time control explains capture requirement");
                    js(web,@"gesture(0,true);send(0,7.25);gesture(0,false)");
                    check(AudioUnitGetParameter(host.unit,0,kAudioUnitScope_Global,0,&preserved),"read effect UI parameter");
                    require(std::abs(preserved-7.25)<.001,"effect normalized control bridge");
                    require(![js(web,@"document.body.innerText.includes('NaN')") boolValue],"finite effect display");
                    js(web,@"gesture(0,true);send(0,0);gesture(0,false)");
                    if(reopen==0) {
                        js(web,@"document.getElementById('loop').click()");
                        render(host,delay+4096); pump(.3);
                        require([js(web,@"document.getElementById('source-name').textContent") isEqual:@"Captured passage"],"effect loop UI and engine agree");
                        require(![js(web,@"document.getElementById('time-control').classList.contains('inactive')") boolValue],"capture time control becomes active");
                        __block bool done=false; __block NSImage *image=nil;
                        [web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *value,NSError *error) { image=value; done=true; }];
                        for(int i=0;i<1000 && !done;++i) pump(.01);
                        require(done && image,"native effect screenshot");
                        NSBitmapImageRep *bitmap=[NSBitmapImageRep imageRepWithData:image.TIFFRepresentation];
                        require([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:argv[2]] atomically:YES],"write effect UI review image");
                        js(web,@"document.getElementById('live').click()"); render(host,delay+4096); pump(.2);
                    }
                    [editor removeFromSuperview]; window.contentView=nil; [window close]; pump(.1);
                    CFRelease(info.mCocoaAUViewBundleLocation); CFRelease(info.mCocoaAUViewClass[0]);
                }
            }
        }
    }
    if(bundle) CFRelease(bundle);
    std::puts("FX AU host: exact latency, mono/stereo, live pitch, capture time, embedded state, malformed-state refusal, bypass and reset passed");
}
