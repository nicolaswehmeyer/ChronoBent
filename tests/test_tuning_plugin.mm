// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "plugin_host_fixture.hpp"
#include <stdexcept>
#include <functional>
#include "tuning_state_fixture.hpp"
using namespace tuning_fixture;
static NSDictionary *save(Host &host) {
    CFPropertyListRef state=nullptr;UInt32 size=sizeof(state);check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&size),"save tuning project");return (__bridge_transfer NSDictionary *)state;
}
static OSStatus restore(Host &host,NSDictionary *state) {
    CFPropertyListRef value=(__bridge CFPropertyListRef)state;return AudioUnitSetProperty(host.unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&value,sizeof(value));
}
static NSDictionary *replace(NSDictionary *state,NSData *data) {NSMutableDictionary *copy=[state mutableCopy];copy[@"data"]=data;return copy;}
static NSView *labeled(NSView *view,NSString *label) {
    if([view.accessibilityLabel isEqualToString:label])return view;
    for(NSView *child in view.subviews)if(auto found=labeled(child,label))return found;return nil;
}
static NSButton *button_named(NSView *view,NSString *name) {
    if([view isKindOfClass:NSButton.class] && [((NSButton *)view).title isEqualToString:name])return (NSButton *)view;
    for(NSView *child in view.subviews)if(auto found=button_named(child,name))return found;return nil;
}
static bool until(const std::function<bool()> &predicate,unsigned count=3000) {for(unsigned i=0;i<count;++i){if(predicate())return true;pump(.01);}return false;}
static std::vector<float> output(Host &host,bool effect) {
    check(AudioUnitReset(host.unit,kAudioUnitScope_Global,0),"reset tuned test source");if(!effect)host.note(60,100);
    std::vector<float> audio;for(int i=0;i<145;++i){host.render();audio.insert(audio.end(),host.left.begin(),host.left.end());}return audio;
}
int main(int argc,char **argv) {
    @autoreleasepool {
        require(argc>=3,"pass Instrument and FX AU bundles");[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];NSApp.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];[NSApp finishLaunching];
        for(bool effect:{false,true}) {
            NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[effect?2:1]]];
            CFBundleRef bundle=CFBundleCreate(nullptr,(__bridge CFURLRef)url);require(bundle && CFBundleLoadExecutable(bundle),"load tuning AU");
            auto factory=reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle,effect?CFSTR("ChronoBentFX_Factory"):CFSTR("ChronoBent_Factory")));require(factory,"tuning AU factory");
            AudioComponentDescription description{OSType(effect?'aufx':'aumu'),OSType(effect?'ChFx':'ChBn'),'NWeh',0,0};
            const auto component=AudioComponentRegister(&description,CFSTR("ChronoBent tuning test"),1536,factory);require(component,"register tuning AU");
            {
                Host host(component,48000,effect?2:0,2);if(!effect){host.note(60,100);host.render();}
                auto state=save(host);auto old=parse(state[@"data"]);old.metadata[@"version"]=@1;[old.metadata removeObjectForKey:@"tuning"];
                old.metadata[@"rate"]=@48000;old.metadata[@"frames"]=@96000;old.metadata[@"name"]=@"Original vocal é";
                if(effect){NSMutableArray *parameters=[old.metadata[@"params"] mutableCopy];parameters[5]=@2;old.metadata[@"params"]=parameters;}
                std::vector<float> original(96000*2);for(size_t i=0;i<96000;++i){original[i*2]=float(.35*std::sin(6.283185307179586*225*i/48000));original[i*2+1]=-original[i*2];}
                old.audio=[NSData dataWithBytes:original.data() length:original.size()*4];
                check(restore(host,replace(state,encode(old))),"0.5 state remains loadable");output(host,effect);state=save(host);
                auto fresh=parse(state[@"data"]);require([fresh.metadata[@"version"] intValue]==2 && [fresh.audio isEqualToData:old.audio],"0.6 preserves legacy source PCM");
                AudioUnitCocoaViewInfo info{};UInt32 size=sizeof(info);check(AudioUnitGetProperty(host.unit,kAudioUnitProperty_CocoaUI,kAudioUnitScope_Global,0,&info,&size),"tuning editor metadata");
                NSBundle *viewBundle=[NSBundle bundleWithURL:(__bridge NSURL *)info.mCocoaAUViewBundleLocation];require([viewBundle load],"load tuning UI bundle");
                Class cls=NSClassFromString((__bridge NSString *)info.mCocoaAUViewClass[0]);id<AUCocoaUIBase> provider=[[cls alloc] init];
                NSView *view=[provider uiViewForAudioUnit:host.unit withSize:NSMakeSize(1040,720)];require(view,"create host editor for tuning");
                NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1040,720) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];window.releasedWhenClosed=NO;window.contentView=view;
                WKWebView *web=nil;require(until([&]{web=webview(view);return web!=nil;}),"tuning WebKit view");pump(1);
                require(until([&]{return ![js(web,@"document.getElementById('tune').disabled") boolValue];}),"Note Studio button admitted current source");
                js(web,@"document.getElementById('tune').click()");NSWindow *studio=nil;
                require(until([&]{for(NSWindow *candidate in NSApp.windows)if([candidate.title isEqualToString:@"ChronoBent · Note Studio"] && candidate.visible){studio=candidate;return true;}return false;}),"native Note Studio opened from plugin");
                auto analyze=button_named(studio.contentView,@"Analyze melody");require(analyze && analyze.enabled,"native analyze action");[analyze performClick:nil];
                NSButton *use=nil;require(until([&]{use=button_named(studio.contentView,@"Use tuned audio");return use && use.enabled;}),"asynchronous analysis completed");
                auto graph=labeled(studio.contentView,@"Detected pitch and editable target notes");require(graph && [studio makeFirstResponder:graph],"keyboard note selection");
                auto target=(NSTextField *)labeled(studio.contentView,@"Selected target MIDI note");require(target,"native target control");target.doubleValue=58;[target sendAction:target.action to:target.target];
                require(until([&]{return use.enabled;}),"manual note render completed");[use performClick:nil];
                require(until([&]{auto blob=parse(save(host)[@"data"]);return [blob.metadata[@"tuning"] isKindOfClass:NSDictionary.class] && [blob.metadata[@"tuning"][@"corrected"] boolValue];}),"tuned audio committed to plugin");
                output(host,effect);auto tuned=save(host);auto blob=parse(tuned[@"data"]);const size_t bytes=96000*8;
                require(blob.audio.length==bytes*2,"project contains exact original and corrected PCM");
                require([[blob.audio subdataWithRange:NSMakeRange(bytes,bytes)] isEqualToData:old.audio],"embedded original remains bit exact");
                require(std::abs(frequency(blob.audio,0,48000,96000)-220*std::exp2(1./12))<.5,"manual target changes rendered source frequency");
                require([blob.metadata[@"tuning"][@"edits"] count]==1 && [blob.metadata[@"tuning"][@"edits"][0][2] doubleValue]==58,"manual edit persisted");
                {
                    Host restored(component,48000,effect?2:0,2);check(restore(restored,tuned),"restore tuned project without analysis");const auto a=output(restored,effect);
                    Host duplicate(component,48000,effect?2:0,2);check(restore(duplicate,tuned),"repeat tuned project restore");require(output(duplicate,effect)==a,"tuned project restores deterministic host audio");
                    Host changed_rate(component,96000,effect?2:0,2);check(restore(changed_rate,tuned),"restore tuned project at another host sample rate");output(changed_rate,effect);
                    require([parse(save(changed_rate)[@"data"]).audio isEqualToData:blob.audio],"host-rate change retains canonical editable PCM");
                }
                auto baseline=save(host);
                for(int invalid=0;invalid<4;++invalid) {
                    auto bad=parse(baseline[@"data"]);
                    if(invalid==0){NSMutableDictionary *tuning=bad.metadata[@"tuning"];NSMutableArray *options=[tuning[@"options"] mutableCopy];options[0]=@0;tuning[@"options"]=options;}
                    if(invalid==1){NSMutableDictionary *tuning=bad.metadata[@"tuning"];tuning[@"edits"]=@[@[@1,@0,@58]];}
                    if(invalid==2)bad.audio=[bad.audio subdataWithRange:NSMakeRange(0,bad.audio.length-8)];
                    if(invalid==3){NSMutableData *audio=[bad.audio mutableCopy];const float nan=NAN;std::memcpy(static_cast<char *>(audio.mutableBytes)+bytes+100,&nan,4);bad.audio=audio;}
                    require(restore(host,replace(baseline,encode(bad)))!=noErr,"invalid tuning project rejected");
                    require([save(host)[@"data"] isEqualToData:baseline[@"data"]],"rejected tuning state preserves entire previous project");
                }
                NSButton *original_button=nil;require(until([&]{original_button=button_named(studio.contentView,@"Use original");return original_button!=nil;}),"original A/B control");[original_button performClick:nil];
                require(until([&]{auto b=parse(save(host)[@"data"]);return ![b.metadata[@"tuning"][@"corrected"] boolValue];}),"original restored through plugin A/B");
                auto original_state=parse(save(host)[@"data"]);require([[original_state.audio subdataWithRange:NSMakeRange(0,bytes)] isEqualToData:old.audio],"A/B returns exact original source");
                [studio close];[view removeFromSuperview];window.contentView=nil;[window close];pump(.1);
                CFRelease(info.mCocoaAUViewBundleLocation);CFRelease(info.mCocoaAUViewClass[0]);
            }
            CFRelease(bundle);
        }
        std::puts("tuning plugins: native analysis/edit/A-B, legacy state, exact dual PCM, transactional rejection, deterministic restore and host-rate changes passed");
    }return 0;
}
