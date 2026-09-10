// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "plugin_host_fixture.hpp"
// Fundamental period in samples: the first normalized autocorrelation peak above the steady part of a note.
static double period(const std::vector<float> &s,double rate) {
    const std::size_t begin=s.size()/4; double best=0; std::size_t at=0; bool inside=false;
    for(std::size_t lag=std::size_t(rate/1500);lag<=std::size_t(rate/100);++lag) {
        double dot=0,e1=0,e2=0;
        for(std::size_t i=begin;i+lag<s.size();++i) { dot+=s[i]*s[i+lag]; e1+=s[i]*s[i]; e2+=s[i+lag]*s[i+lag]; }
        const double r=dot/std::sqrt(e1*e2+1e-30);
        if(r>.6) { inside=true; if(r>best) { best=r; at=lag; } } else if(inside) break;
    }
    require(at,"periodic instrument tone"); return double(at);
}
static std::vector<float> capture(Host &host,int blocks) {
    std::vector<float> out; for(int i=0;i<blocks;++i) { host.render(); out.insert(out.end(),host.left.begin(),host.left.end()); } return out;
}
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
            {
                // Preparation controls apply themselves: a host-side pitch change becomes audible without an Apply message.
                host.note(60,100); const auto plain=capture(host,8); host.note(60,0);
                check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,12,0),"host pitch change");
                pump(.6);
                check(AudioUnitReset(host.unit,kAudioUnitScope_Global,0),"silence before the shifted note");
                host.note(60,100); const auto shifted=capture(host,8); host.note(60,0);
                require(std::abs(period(plain,host.rate)/period(shifted,host.rate)-2)<.06,"self-applied host pitch change doubles the fundamental");
                check(AudioUnitSetParameter(host.unit,0,kAudioUnitScope_Global,0,0,0),"restore pitch");
                pump(.6);
                check(AudioUnitReset(host.unit,kAudioUnitScope_Global,0),"silence before the restored note");
                host.note(60,100); const auto restored=capture(host,8); host.note(60,0);
                require(std::abs(period(plain,host.rate)-period(restored,host.rate))<1.5,"restored host pitch prepares itself again");
                check(AudioUnitReset(host.unit,kAudioUnitScope_Global,0),"silence after the pitch checks");
            }
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
                require([js(web,@"/^(SOUND CHANGED · UPDATING|PREPARING YOUR SOUND)/.test(document.getElementById('status').textContent)") boolValue],"editor reports the pending update");
                bool applied=false;
                for(int i=0;i<600 && !applied;++i) { pump(.02); applied=[js(web,@"document.getElementById('apply').textContent.startsWith('UP TO DATE')&&document.getElementById('status').textContent.startsWith('READY TO PLAY')") boolValue]; }
                require(applied,"editor knob release applies the sound without a click");
                js(web,@"gesture(0,true);send(0,7.25);gesture(0,false)"); pump(.4);
                require([js(web,@"document.getElementById('apply').textContent.startsWith('UP TO DATE')&&document.getElementById('status').textContent.startsWith('READY TO PLAY')") boolValue],"unchanged value resend stays up to date");
                js(web,@"gesture(0,true);send(0,0);gesture(0,false);document.getElementById('apply').click()");
                host.note(60,100); host.render(); pump(.3);
                js(web,@"document.getElementById('preset-menu').hidden=false");
                require([js(web,@"document.querySelectorAll('[data-preset]').length") intValue]==3,"three native presets");
                js(web,@"document.getElementById('preset-menu').hidden=true");
                js(web,@"document.getElementById('presets').click()");
                require([js(web,@"[...document.querySelectorAll('[data-preset]')].every(b=>{const r=b.getBoundingClientRect();return b.contains(document.elementFromPoint(r.left+r.width/2,r.top+r.height/2));})") boolValue],"open preset entries receive pointer hits");
                js(web,@"(()=>{const b=document.querySelector('[data-preset=\"1\"]');const r=b.getBoundingClientRect();const o={bubbles:true,cancelable:true,clientX:r.left+r.width/2,clientY:r.top+r.height/2,pointerId:1,isPrimary:true,button:0,buttons:1};const t=document.elementFromPoint(o.clientX,o.clientY);t.dispatchEvent(new PointerEvent('pointerdown',o));t.dispatchEvent(new MouseEvent('mousedown',o));o.buttons=0;t.dispatchEvent(new PointerEvent('pointerup',o));t.dispatchEvent(new MouseEvent('mouseup',o));t.dispatchEvent(new MouseEvent('click',o));})()");
                require([js(web,@"document.getElementById('preset-menu').hidden") boolValue],"preset choice closes the menu");
                bool swapped=false;
                for(int i=0;i<600 && !swapped;++i) { pump(.02); swapped=[js(web,@"document.getElementById('sample-name').textContent==='Soft Current'&&document.getElementById('status').textContent.startsWith('READY TO PLAY')") boolValue]; }
                require(swapped,"editor preset choice loads the factory sound");
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
