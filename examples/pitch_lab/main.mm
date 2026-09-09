// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#include "player.hpp"
#include "chronobent/chronobent.h"
#include <cmath>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <memory>
#include <string>

static AVAudioSourceNode *sourceNode(std::shared_ptr<audition::Player> player, AVAudioFormat *format) {
    return [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:^OSStatus(BOOL *silent,const AudioTimeStamp *time,AVAudioFrameCount frames,AudioBufferList *output) {
        (void)time;
        if(output->mNumberBuffers!=2 || !output->mBuffers[0].mData || !output->mBuffers[1].mData) return kAudio_ParamError;
        player->pull(static_cast<float *>(output->mBuffers[0].mData),static_cast<float *>(output->mBuffers[1].mData),frames);
        *silent=NO; return noErr;
    }];
}

// Exercise the actual system audio graph without opening an output device.
static int audioSelfTest() {
    std::vector<float> source(48000*2);
    for(size_t i=0;i<48000;++i) {
        source[i*2]=float(0.2*std::sin(6.283185307179586*440*double(i)/48000));
        source[i*2+1]=-source[i*2];
    }
    auto player=std::make_shared<audition::Player>(std::move(source),48000);
    AVAudioFormat *format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
    AVAudioEngine *engine=[AVAudioEngine new];
    AVAudioSourceNode *node=sourceNode(player,format);
    [engine attachNode:node]; [engine connect:node to:engine.mainMixerNode format:format];
    NSError *error=nil;
    if(![engine enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline format:format maximumFrameCount:256 error:&error] ||
       ![engine startAndReturnError:&error]) {
        std::fprintf(stderr,"Audio graph failed: %s\n",error.localizedDescription.UTF8String); return 1;
    }
    AVAudioPCMBuffer *buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:256];
    player->pause(false);
    double energy=0;
    for(unsigned block=0;block<160;++block) {
        if(block==60) player->request(audition::Settings{5.7,1.25,true,false,true});
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(player->queued()<256 && !player->error()) {
            if(std::chrono::steady_clock::now()>deadline) return 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(player->error()) return 3;
        const auto status=[engine renderOffline:256 toBuffer:buffer error:&error];
        if(status!=AVAudioEngineManualRenderingStatusSuccess || buffer.frameLength!=256) return 4;
        for(unsigned i=0;i<256;++i) {
            const float left=buffer.floatChannelData[0][i], right=buffer.floatChannelData[1][i];
            if(!std::isfinite(left) || std::abs(left+right)>1e-5) return 5;
            energy+=double(left)*left;
        }
    }
    [engine stop];
    if(energy<100 || player->underruns()!=0 || player->played()!=160*256) return 6;
    std::puts("AVFoundation offline graph: stereo, pitch/tempo transition, duration and zero underruns passed");
    return 0;
}

@interface PitchLab : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow *_window;
    NSTextField *_fileLabel, *_pitchLabel, *_tempoLabel, *_timeLabel, *_statusLabel;
    NSSlider *_pitch, *_tempo;
    NSButton *_play, *_bypass, *_formants, *_open, *_master;
    AVAudioEngine *_engine;
    AVAudioSourceNode *_source;
    NSTimer *_timer;
    std::shared_ptr<audition::Player> _player;
    std::vector<float> _decoded;
    double _sampleRate;
    BOOL _playing;
}
@end

static NSTextField *label(NSString *text, CGFloat size, NSFontWeight weight) {
    NSTextField *field = [NSTextField labelWithString:text];
    field.font = [NSFont systemFontOfSize:size weight:weight];
    field.textColor = NSColor.labelColor;
    return field;
}
static NSButton *button(NSString *text, id owner, SEL action) {
    NSButton *result = [NSButton buttonWithTitle:text target:owner action:action];
    result.bezelStyle = NSBezelStyleRounded;
    return result;
}

@implementation PitchLab
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu *menu = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new]; [menu addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit ChronoBent Lab" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu; NSApp.mainMenu = menu;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,740,670)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"ChronoBent Lab"; _window.delegate = self;
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _window.backgroundColor = [NSColor colorWithCalibratedRed:0.075 green:0.09 blue:0.12 alpha:1];
    NSView *view = _window.contentView;
    NSTextField *heading = label(@"ChronoBent Lab", 32, NSFontWeightSemibold);
    heading.frame = NSMakeRect(36,585,500,45); [view addSubview:heading];
    NSTextField *sub = label([NSString stringWithFormat:@"ChronoBent · pitch & time engine · %s",chronobent_version()],12,NSFontWeightRegular);
    sub.textColor = NSColor.secondaryLabelColor; sub.frame = NSMakeRect(38,561,660,22); [view addSubview:sub];
    _open = button(@"Open audio…",self,@selector(openAudio:));
    _open.frame = NSMakeRect(34,504,130,34); [view addSubview:_open];
    _fileLabel = label(@"Choose a track to begin",14,NSFontWeightMedium);
    _fileLabel.lineBreakMode = NSLineBreakByTruncatingMiddle; _fileLabel.frame = NSMakeRect(182,509,516,24); [view addSubview:_fileLabel];
    _pitchLabel = label(@"+0.00 st",44,NSFontWeightLight);
    _pitchLabel.alignment = NSTextAlignmentCenter; _pitchLabel.frame = NSMakeRect(180,423,380,58); [view addSubview:_pitchLabel];
    _pitch = [NSSlider sliderWithValue:0 minValue:-12 maxValue:12 target:self action:@selector(pitchChanged:)];
    _pitch.continuous = YES; _pitch.frame = NSMakeRect(40,389,660,26); _pitch.enabled = NO;
    _pitch.toolTip = @"Continuous pitch, minus to plus one octave. Click Reset pitch for unity.";
    [_pitch setAccessibilityLabel:@"Pitch in semitones"]; [view addSubview:_pitch];
    NSTextField *low = label(@"−12  /  DOWN ONE OCTAVE",10,NSFontWeightMedium); low.textColor=NSColor.secondaryLabelColor;
    low.frame=NSMakeRect(40,367,230,18); [view addSubview:low];
    NSTextField *high = label(@"+12  /  UP ONE OCTAVE",10,NSFontWeightMedium); high.textColor=NSColor.secondaryLabelColor;
    high.alignment=NSTextAlignmentRight; high.frame=NSMakeRect(470,367,230,18); [view addSubview:high];
    _tempoLabel=label(@"100.00% speed · original key",28,NSFontWeightLight);
    _tempoLabel.alignment=NSTextAlignmentCenter; _tempoLabel.frame=NSMakeRect(40,298,660,44); [view addSubview:_tempoLabel];
    _tempo=[NSSlider sliderWithValue:1 minValue:0.5 maxValue:2 target:self action:@selector(pitchChanged:)];
    _tempo.continuous=YES; _tempo.enabled=NO; _tempo.frame=NSMakeRect(40,264,660,26);
    [_tempo setAccessibilityLabel:@"Playback speed ratio"]; _tempo.toolTip=@"50% to 200% playback speed. Master Tempo preserves the selected musical key."; [view addSubview:_tempo];
    NSTextField *speedLow=label(@"50%  /  HALF SPEED",10,NSFontWeightMedium); speedLow.textColor=NSColor.secondaryLabelColor;
    speedLow.frame=NSMakeRect(40,242,230,18); [view addSubview:speedLow];
    NSTextField *speedHigh=label(@"200%  /  DOUBLE SPEED",10,NSFontWeightMedium); speedHigh.textColor=NSColor.secondaryLabelColor;
    speedHigh.alignment=NSTextAlignmentRight; speedHigh.frame=NSMakeRect(470,242,230,18); [view addSubview:speedHigh];
    _master=[NSButton checkboxWithTitle:@"Master Tempo · keep key" target:self action:@selector(pitchChanged:)];
    _master.state=NSControlStateValueOn; _master.frame=NSMakeRect(40,199,250,26); [view addSubview:_master];
    NSButton *resetSpeed=button(@"Reset speed",self,@selector(resetSpeed:)); resetSpeed.frame=NSMakeRect(558,195,145,34); [view addSubview:resetSpeed];
    _play=button(@"Play",self,@selector(playPause:)); _play.frame=NSMakeRect(34,151,98,34); _play.enabled=NO; [view addSubview:_play];
    NSButton *restart=button(@"Restart",self,@selector(restart:)); restart.frame=NSMakeRect(136,151,98,34); [view addSubview:restart];
    NSButton *reset=button(@"Reset pitch",self,@selector(resetPitch:)); reset.frame=NSMakeRect(238,151,110,34); [view addSubview:reset];
    _bypass=[NSButton checkboxWithTitle:@"Bypass" target:self action:@selector(pitchChanged:)];
    _bypass.frame=NSMakeRect(393,155,90,24); [view addSubview:_bypass];
    _formants=[NSButton checkboxWithTitle:@"Preserve formants" target:self action:@selector(pitchChanged:)];
    _formants.frame=NSMakeRect(509,155,191,24); [view addSubview:_formants];
    _timeLabel=label(@"00:00  /  00:00",14,NSFontWeightMedium); _timeLabel.frame=NSMakeRect(39,99,330,25); [view addSubview:_timeLabel];
    _statusLabel=label(@"Local files only · playback starts when you press Play",12,NSFontWeightRegular);
    _statusLabel.textColor=NSColor.secondaryLabelColor; _statusLabel.frame=NSMakeRect(39,43,663,44);
    _statusLabel.maximumNumberOfLines=2; [view addSubview:_statusLabel];
    [_window center]; [_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    _timer=[NSTimer scheduledTimerWithTimeInterval:0.15 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}
- (void)showError:(NSString *)message {
    NSAlert *alert=[NSAlert new]; alert.messageText=@"Could not play this audio"; alert.informativeText=message;
    [alert beginSheetModalForWindow:_window completionHandler:nil];
}
- (void)stop {
    _playing=NO; if(_player) _player->pause(true);
    [_engine stop]; if(_source) [_engine detachNode:_source];
    _source=nil; _engine=nil; _player.reset(); _play.title=@"Play";
}
- (BOOL)prepare {
    [self stop];
    if(_decoded.empty()) return NO;
    try { _player=std::make_shared<audition::Player>(_decoded,_sampleRate,[self currentSettings]); }
    catch(const std::exception &error) { [self showError:[NSString stringWithUTF8String:error.what()]]; return NO; }
    @try {
    AVAudioFormat *format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:_sampleRate channels:2];
    _source=sourceNode(_player,format);
    _engine=[AVAudioEngine new]; [_engine attachNode:_source];
    [_engine connect:_source to:_engine.mainMixerNode format:format];
    // Equal headroom for processed and bypass audio; no limiter obscures A/B.
    _engine.mainMixerNode.outputVolume=0.7f;
    [self pitchChanged:nil]; [_engine prepare];
    return YES;
    } @catch(NSException *exception) {
        [self stop]; [self showError:exception.reason ?: @"The system audio component is unavailable."]; return NO;
    }
}
- (void)openAudio:(id)sender {
    (void)sender;
    NSOpenPanel *panel=[NSOpenPanel openPanel]; panel.canChooseDirectories=NO; panel.allowsMultipleSelection=NO;
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        if(response!=NSModalResponseOK) return;
        [self stop]; self->_open.enabled=NO; self->_play.enabled=NO; self->_pitch.enabled=NO; self->_tempo.enabled=NO;
        self->_statusLabel.stringValue=@"Decoding audio…";
        NSURL *url=panel.URL;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            NSError *error=nil;
            AVAudioFile *file=[[AVAudioFile alloc] initForReading:url commonFormat:AVAudioPCMFormatFloat32 interleaved:NO error:&error];
            NSString *failure=nil; std::vector<float> decoded; double rate=0;
            if(!file) failure=error.localizedDescription ?: @"Unsupported audio file.";
            else if(file.processingFormat.channelCount<1 || file.processingFormat.channelCount>2 || file.length<=0 ||
                    file.processingFormat.sampleRate<8000 || file.processingFormat.sampleRate>192000 ||
                    file.length>64000000) failure=@"Use a mono/stereo file at 8–192 kHz, up to 64 million frames (about 22 minutes at 48 kHz).";
            else {
                rate=file.processingFormat.sampleRate;
                try {
                    decoded.resize(static_cast<size_t>(file.length)*2);
                    AVAudioPCMBuffer *buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:file.processingFormat frameCapacity:8192];
                    size_t position=0;
                    while(position<decoded.size()/2) {
                        if(![file readIntoBuffer:buffer frameCount:static_cast<AVAudioFrameCount>(std::min<size_t>(8192,decoded.size()/2-position)) error:&error] || !buffer.frameLength) {
                            failure=error.localizedDescription ?: @"Unexpected end of file."; break;
                        }
                        for(AVAudioFrameCount i=0;i<buffer.frameLength;++i) {
                            float left=buffer.floatChannelData[0][i];
                            float right=buffer.floatChannelData[file.processingFormat.channelCount==1?0:1][i];
                            if(!std::isfinite(left)||!std::isfinite(right)||std::abs(left)>64||std::abs(right)>64) { failure=@"The audio contains invalid samples."; break; }
                            decoded[2*(position+i)]=left; decoded[2*(position+i)+1]=right;
                        }
                        if(failure) break; position+=buffer.frameLength;
                    }
                } catch(...) { failure=@"Not enough memory to decode this file."; }
            }
            // Shared ownership moves the large result between queues once.
            auto result=std::make_shared<std::vector<float>>(std::move(decoded));
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_open.enabled=YES;
                if(failure) { self->_statusLabel.stringValue=@"Choose another audio file."; [self showError:failure]; return; }
                self->_decoded=std::move(*result); self->_sampleRate=rate;
                self->_fileLabel.stringValue=url.lastPathComponent;
                if([self prepare]) { self->_play.enabled=YES; [self pitchChanged:nil]; self->_statusLabel.stringValue=@"Ready · use headphones and compare Bypass · output headroom −3.1 dB"; }
            });
        });
    }];
}
- (audition::Settings)currentSettings {
    return audition::Settings{_pitch.doubleValue,_tempo.doubleValue,
        _master.state==NSControlStateValueOn,_bypass.state==NSControlStateValueOn,
        _formants.state==NSControlStateValueOn};
}
- (void)pitchChanged:(id)sender {
    (void)sender; const auto settings=[self currentSettings];
    _pitchLabel.stringValue=settings.master_tempo ? [NSString stringWithFormat:@"%+.2f st",settings.semitones] : @"Pitch follows speed";
    _tempoLabel.stringValue=[NSString stringWithFormat:@"%.2f%% speed · %@",settings.tempo*100,
        settings.master_tempo ? (settings.semitones==0 ? @"original key" : @"selected key") : @"linked pitch"];
    _pitch.enabled=bool(_player) && settings.master_tempo && !settings.bypass;
    _tempo.enabled=bool(_player) && !settings.bypass;
    _formants.enabled=settings.master_tempo && !settings.bypass;
    if(_player) _player->request(settings);
}
- (void)resetPitch:(id)sender { (void)sender; _pitch.doubleValue=0; [self pitchChanged:nil]; }
- (void)resetSpeed:(id)sender { (void)sender; _tempo.doubleValue=1; [self pitchChanged:nil]; }
- (void)playPause:(id)sender {
    (void)sender; if(!_player) return;
    if(_player->ended() && ![self prepare]) return;
    if(!_playing) {
        NSError *error=nil;
        if(![_engine startAndReturnError:&error]) { [self showError:error.localizedDescription]; return; }
    }
    _playing=!_playing; _player->pause(!_playing); _play.title=_playing?@"Pause":@"Play";
}
- (void)restart:(id)sender { (void)sender; if(!_decoded.empty()) [self prepare]; }
- (void)tick:(NSTimer *)timer {
    (void)timer; if(!_player) return;
    unsigned at=unsigned(_player->source_position()/_sampleRate), end=unsigned(double(_player->frames())/_sampleRate);
    _timeLabel.stringValue=[NSString stringWithFormat:@"%02u:%02u  /  %02u:%02u",at/60,at%60,end/60,end%60];
    if(_player->error()) { _statusLabel.stringValue=@"Playback stopped: the engine rejected the audio."; [self stop]; }
    else if(_player->ended()) { _player->pause(true); [_engine pause]; _playing=NO; _play.title=@"Play again"; _statusLabel.stringValue=@"Finished · Play again to restart"; }
    else if(_playing) _statusLabel.stringValue=[NSString stringWithFormat:@"%.0f Hz · pitch & tempo transitions · queue %.0f ms · underruns %u",_sampleRate,1000.0*double(_player->queued())/_sampleRate,_player->underruns()];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (void)applicationWillTerminate:(NSNotification *)notification { (void)notification; [_timer invalidate]; [self stop]; }
@end
int main(int argc,const char *argv[]) {
    @autoreleasepool {
        if(argc==2 && std::string(argv[1])=="--self-test") {
            @try { return audioSelfTest(); }
            @catch(NSException *exception) { std::fprintf(stderr,"Audio component unavailable: %s\n",exception.reason.UTF8String); return 7; }
        }
    }
    @autoreleasepool { NSApplication *app=NSApplication.sharedApplication; PitchLab *delegate=[PitchLab new]; app.delegate=delegate; [app run]; }
    return 0;
}
