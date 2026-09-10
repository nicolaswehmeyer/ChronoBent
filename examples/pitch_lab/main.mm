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

static bool uiSelfTestMode=false;

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

// Keep native slider tracking, keyboard access and accessibility, with a
// deterministic drawing path that also works in offscreen view tests.
@interface LabSliderCell : NSSliderCell
@end
@implementation LabSliderCell
- (void)drawBarInside:(NSRect)rect flipped:(BOOL)flipped {
    (void)flipped;
    rect.origin.y=NSMidY(rect)-2; rect.size.height=4;
    [[NSColor colorWithCalibratedWhite:.32 alpha:self.enabled ? 1 : .5] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:2 yRadius:2] fill];
}
- (void)drawKnob:(NSRect)rect {
    rect=NSMakeRect(NSMidX(rect)-7,NSMidY(rect)-7,14,14);
    [(self.enabled ? NSColor.controlAccentColor : NSColor.disabledControlTextColor) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:rect] fill];
}
@end
@interface LabSlider : NSSlider
@end
@implementation LabSlider
+ (Class)cellClass { return LabSliderCell.class; }
@end

// An overview of decoded samples. Drawing reads this immutable summary only.
@interface WaveformView : NSView {
    std::vector<std::pair<float,float>> _peaks;
    double _progress;
}
- (void)setAudio:(const std::vector<float> &)audio;
- (void)setProgress:(double)value;
@end
@implementation WaveformView
- (void)setAudio:(const std::vector<float> &)audio {
    _peaks.clear();
    const size_t bins=std::min<size_t>(1000,audio.size()/2);
    for(size_t b=0;b<bins;++b) {
        float lo=0,hi=0;
        for(size_t i=b*(audio.size()/2)/bins;i<(b+1)*(audio.size()/2)/bins;++i) {
            lo=std::min(lo,std::min(audio[i*2],audio[i*2+1]));
            hi=std::max(hi,std::max(audio[i*2],audio[i*2+1]));
        }
        _peaks.emplace_back(lo,hi);
    }
    _progress=0; self.needsDisplay=YES;
}
- (void)setProgress:(double)value { _progress=std::clamp(value,0.0,1.0); self.needsDisplay=YES; }
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const auto bounds=self.bounds;
    [[NSColor colorWithCalibratedRed:.09 green:.12 blue:.16 alpha:1] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:bounds xRadius:12 yRadius:12] fill];
    const CGFloat middle=NSMidY(bounds), usable=NSWidth(bounds)-28;
    for(size_t i=0;i<_peaks.size();++i) {
        const CGFloat x=14+usable*double(i)/double(_peaks.size());
        const CGFloat low=middle+std::clamp(_peaks[i].first,-1.f,1.f)*(NSHeight(bounds)*.42);
        const CGFloat high=middle+std::clamp(_peaks[i].second,-1.f,1.f)*(NSHeight(bounds)*.42);
        [(double(i)/double(_peaks.size())<_progress ? NSColor.controlAccentColor : NSColor.secondaryLabelColor) setStroke];
        NSBezierPath *line=[NSBezierPath bezierPath]; [line moveToPoint:NSMakePoint(x,low)]; [line lineToPoint:NSMakePoint(x,std::max(low+1,high))];
        line.lineWidth=std::max(CGFloat(1),usable/CGFloat(_peaks.size())*.7); [line stroke];
    }
    if(!_peaks.empty()) {
        [NSColor.whiteColor setStroke]; NSBezierPath *cursor=[NSBezierPath bezierPath];
        const CGFloat x=14+usable*_progress; [cursor moveToPoint:NSMakePoint(x,10)]; [cursor lineToPoint:NSMakePoint(x,NSHeight(bounds)-10)];
        cursor.lineWidth=1.5; [cursor stroke];
    }
}
@end

@interface PitchLab : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow *_window;
    NSTextField *_fileLabel, *_pitchLabel, *_tempoLabel, *_timeLabel, *_statusLabel;
    NSSlider *_pitch, *_tempo, *_timbre, *_timeline, *_envelope;
    NSTextField *_timbreLabel, *_envelopeLabel, *_gainLabel;
    NSSlider *_gain;
    NSSegmentedControl *_transients;
    WaveformView *_waveform;
    NSPopUpButton *_profile;
    BOOL _resumeAfterSeek;
    NSButton *_play, *_bypass, *_formants, *_open, *_master;
    AVAudioEngine *_engine;
    AVAudioSourceNode *_source;
    NSTimer *_timer;
    std::shared_ptr<audition::Player> _player;
    std::shared_ptr<const std::vector<float>> _decoded;
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
    if(!uiSelfTestMode) [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu *menu = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new]; [menu addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit ChronoBent Lab" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSMenuItem *fileItem=[NSMenuItem new]; [menu addItem:fileItem];
    NSMenu *fileMenu=[[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem *openItem=[fileMenu addItemWithTitle:@"Open Audio..." action:@selector(openAudio:) keyEquivalent:@"o"];
    openItem.target=self; fileItem.submenu=fileMenu;
    NSMenuItem *transportItem=[NSMenuItem new]; [menu addItem:transportItem];
    NSMenu *transport=[[NSMenu alloc] initWithTitle:@"Playback"];
    NSMenuItem *playItem=[transport addItemWithTitle:@"Play or Pause" action:@selector(playPause:) keyEquivalent:@" "];
    playItem.target=self; playItem.keyEquivalentModifierMask=0;
    NSMenuItem *back=[transport addItemWithTitle:@"Back 5 Seconds" action:@selector(skipBack:) keyEquivalent:[NSString stringWithFormat:@"%C",static_cast<unichar>(NSLeftArrowFunctionKey)]];
    back.target=self; back.keyEquivalentModifierMask=0;
    NSMenuItem *forward=[transport addItemWithTitle:@"Forward 5 Seconds" action:@selector(skipForward:) keyEquivalent:[NSString stringWithFormat:@"%C",static_cast<unichar>(NSRightArrowFunctionKey)]];
    forward.target=self; forward.keyEquivalentModifierMask=0;
    transportItem.submenu=transport; NSApp.mainMenu = menu;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,760)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"ChronoBent Lab"; _window.delegate = self;
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _window.backgroundColor = [NSColor colorWithCalibratedRed:.055 green:.072 blue:.095 alpha:1];
    NSView *view = _window.contentView;
    view.wantsLayer=YES; view.layer.backgroundColor=_window.backgroundColor.CGColor;
    NSTextField *heading = label(@"ChronoBent Lab",32,NSFontWeightSemibold);
    heading.frame=NSMakeRect(32,693,600,42); [view addSubview:heading];
    NSTextField *sub=label(@"Pitch, time and timbre. Your audio, under your control.",13,NSFontWeightRegular);
    sub.textColor=NSColor.secondaryLabelColor; sub.frame=NSMakeRect(34,667,700,22); [view addSubview:sub];
    NSTextField *version=label([NSString stringWithFormat:@"%s",chronobent_version()],13,NSFontWeightMedium);
    version.alignment=NSTextAlignmentRight; version.textColor=NSColor.secondaryLabelColor;
    version.frame=NSMakeRect(802,702,122,24); [view addSubview:version];
    NSTextField *analysisLabel=label(@"Analysis",11,NSFontWeightMedium); analysisLabel.frame=NSMakeRect(683,668,66,20); [view addSubview:analysisLabel];
    _profile=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(750,664,174,26) pullsDown:NO];
    [_profile addItemsWithTitles:@[@"Compact",@"Balanced",@"Detailed"]]; [_profile selectItemAtIndex:1];
    _profile.target=self; _profile.action=@selector(profileChanged:);
    [_profile setAccessibilityLabel:@"Analysis profile"]; _profile.toolTip=@"Longer windows resolve lower partials but can soften attacks. Changing the profile restarts processing at the current position."; [view addSubview:_profile];
    _open=button(@"Open audio...",self,@selector(openAudio:)); _open.frame=NSMakeRect(30,614,130,34); [view addSubview:_open];
    _fileLabel=label(@"Choose a track to begin",14,NSFontWeightMedium);
    _fileLabel.lineBreakMode=NSLineBreakByTruncatingMiddle; _fileLabel.frame=NSMakeRect(178,619,746,24); [view addSubview:_fileLabel];
    _waveform=[[WaveformView alloc] initWithFrame:NSMakeRect(34,477,892,120)];
    [_waveform setAccessibilityLabel:@"Source waveform overview"]; [view addSubview:_waveform];
    _timeline=[LabSlider sliderWithValue:0 minValue:0 maxValue:1 target:self action:@selector(seekChanged:)];
    _timeline.frame=NSMakeRect(34,445,892,24); _timeline.continuous=NO; _timeline.enabled=NO;
    [_timeline setAccessibilityLabel:@"Track position"]; _timeline.toolTip=@"Drag to seek. Left and right arrow keys skip five seconds."; [view addSubview:_timeline];
    _timeLabel=label(@"00:00  /  00:00",13,NSFontWeightMedium);
    _timeLabel.font=[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
    _timeLabel.frame=NSMakeRect(36,417,320,22); [view addSubview:_timeLabel];
    NSTextField *hint=label(@"Space: play or pause    Arrows: skip 5 seconds",11,NSFontWeightRegular);
    hint.textColor=NSColor.secondaryLabelColor; hint.alignment=NSTextAlignmentRight; hint.frame=NSMakeRect(530,417,394,22); [view addSubview:hint];
    _play=button(@"Play",self,@selector(playPause:)); _play.frame=NSMakeRect(30,366,110,36); _play.enabled=NO; [view addSubview:_play];
    NSButton *backButton=button(@"Back 5s",self,@selector(skipBack:)); backButton.frame=NSMakeRect(148,366,100,36); [view addSubview:backButton];
    NSButton *nextButton=button(@"Forward 5s",self,@selector(skipForward:)); nextButton.frame=NSMakeRect(252,366,110,36); [view addSubview:nextButton];
    NSButton *restart=button(@"Restart",self,@selector(restart:)); restart.frame=NSMakeRect(366,366,100,36); [view addSubview:restart];
    _gainLabel=label(@"Output: -9.0 dB",11,NSFontWeightMedium); _gainLabel.frame=NSMakeRect(504,395,198,18); [view addSubview:_gainLabel];
    _gain=[LabSlider sliderWithValue:-9 minValue:-18 maxValue:0 target:self action:@selector(outputChanged:)];
    _gain.frame=NSMakeRect(504,373,198,23); _gain.continuous=YES;
    [_gain setAccessibilityLabel:@"Output gain in decibels"]; _gain.toolTip=@"Leave headroom for processed peaks. This gain applies equally to bypass and processed audio."; [view addSubview:_gain];
    _bypass=[NSButton checkboxWithTitle:@"Bypass processing" target:self action:@selector(pitchChanged:)];
    _bypass.frame=NSMakeRect(729,372,197,24); [view addSubview:_bypass];
    _bypass.toolTip=@"Original pitch and speed with the same output headroom.";
    NSArray<NSString *> *titles=@[@"PITCH",@"SPEED",@"TIMBRE"];
    for(unsigned i=0;i<3;++i) {
        NSView *card=[[NSView alloc] initWithFrame:NSMakeRect(34+304*i,138,284,212)];
        card.wantsLayer=YES; card.layer.backgroundColor=[NSColor colorWithCalibratedRed:.09 green:.115 blue:.15 alpha:1].CGColor;
        card.layer.cornerRadius=12; [view addSubview:card];
        NSTextField *title=label(titles[i],11,NSFontWeightSemibold); title.textColor=NSColor.secondaryLabelColor;
        title.frame=NSMakeRect(20,173,244,20); [card addSubview:title];
    }
    _pitchLabel=label(@"+0.00 st",32,NSFontWeightLight); _pitchLabel.frame=NSMakeRect(54,259,244,44); [view addSubview:_pitchLabel];
    _pitch=[LabSlider sliderWithValue:0 minValue:-48 maxValue:48 target:self action:@selector(pitchChanged:)];
    _pitch.frame=NSMakeRect(54,221,244,25); _pitch.continuous=YES; _pitch.enabled=NO;
    [_pitch setAccessibilityLabel:@"Pitch in semitones"]; _pitch.toolTip=@"Transpose up or down one octave at the selected speed."; [view addSubview:_pitch];
    NSButton *reset=button(@"Reset pitch",self,@selector(resetPitch:)); reset.frame=NSMakeRect(50,163,118,30); [view addSubview:reset];
    _tempoLabel=label(@"100.00%",32,NSFontWeightLight); _tempoLabel.frame=NSMakeRect(358,259,244,44); [view addSubview:_tempoLabel];
    _tempo=[LabSlider sliderWithValue:1 minValue:.5 maxValue:2 target:self action:@selector(pitchChanged:)];
    _tempo.frame=NSMakeRect(358,221,244,25); _tempo.continuous=YES; _tempo.enabled=NO;
    [_tempo setAccessibilityLabel:@"Playback speed ratio"]; _tempo.toolTip=@"50% to 200%. Master Tempo keeps the selected key."; [view addSubview:_tempo];
    _master=[NSButton checkboxWithTitle:@"Master Tempo: keep key" target:self action:@selector(pitchChanged:)];
    _master.state=NSControlStateValueOn; _master.frame=NSMakeRect(357,193,248,24); [view addSubview:_master];
    NSButton *resetSpeed=button(@"Reset speed",self,@selector(resetSpeed:)); resetSpeed.frame=NSMakeRect(354,155,126,30); [view addSubview:resetSpeed];
    _timbreLabel=label(@"Follows pitch",27,NSFontWeightLight); _timbreLabel.frame=NSMakeRect(662,259,244,44); [view addSubview:_timbreLabel];
    _timbre=[LabSlider sliderWithValue:0 minValue:-12 maxValue:12 target:self action:@selector(pitchChanged:)];
    _timbre.frame=NSMakeRect(662,221,244,25); _timbre.continuous=YES; _timbre.enabled=NO;
    [_timbre setAccessibilityLabel:@"Formant shift in semitones"]; _timbre.toolTip=@"Shift the spectral envelope independently. Zero preserves the original envelope."; [view addSubview:_timbre];
    _formants=[NSButton checkboxWithTitle:@"Independent formants" target:self action:@selector(pitchChanged:)];
    _formants.frame=NSMakeRect(661,193,248,24); [view addSubview:_formants];
    NSButton *resetTimbre=button(@"Preserve original",self,@selector(resetTimbre:)); resetTimbre.frame=NSMakeRect(658,155,154,30); [view addSubview:resetTimbre];
    NSTextField *transientTitle=label(@"Transient handling",12,NSFontWeightMedium); transientTitle.frame=NSMakeRect(36,103,170,22); [view addSubview:transientTitle];
    _transients=[NSSegmentedControl segmentedControlWithLabels:@[@"Smooth",@"Crisp",@"Mixed"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(pitchChanged:)];
    _transients.selectedSegment=1; _transients.frame=NSMakeRect(186,101,266,26);
    [_transients setAccessibilityLabel:@"Transient handling"]; _transients.toolTip=@"Smooth retains phase continuity. Crisp emphasizes onsets. Mixed protects sustained low frequencies during onsets."; [view addSubview:_transients];
    _envelopeLabel=label(@"Envelope: 2.00 ms",12,NSFontWeightMedium); _envelopeLabel.frame=NSMakeRect(504,103,165,22); [view addSubview:_envelopeLabel];
    _envelope=[LabSlider sliderWithValue:2 minValue:1 maxValue:4 target:self action:@selector(pitchChanged:)];
    _envelope.frame=NSMakeRect(678,101,246,26); _envelope.continuous=YES;
    [_envelope setAccessibilityLabel:@"Envelope resolution in milliseconds"]; _envelope.toolTip=@"Higher values retain finer spectral envelope detail. This is not an audio delay."; [view addSubview:_envelope];
    _statusLabel=label(@"Open a local audio file. Playback starts when you press Play.",12,NSFontWeightRegular);
    _statusLabel.textColor=NSColor.secondaryLabelColor; _statusLabel.frame=NSMakeRect(36,28,888,48);
    _statusLabel.maximumNumberOfLines=2; [view addSubview:_statusLabel];
    [self pitchChanged:nil];
    if(!uiSelfTestMode) {
        [_window center]; [_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
        _timer=[NSTimer scheduledTimerWithTimeInterval:0.15 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    }
}
- (void)showError:(NSString *)message {
    NSAlert *alert=[NSAlert new]; alert.messageText=@"Could not play this audio"; alert.informativeText=message;
    [alert beginSheetModalForWindow:_window completionHandler:nil];
}
- (void)stop {
    _playing=NO; _resumeAfterSeek=NO; if(_player) _player->pause(true);
    [_engine stop]; if(_source) [_engine detachNode:_source];
    _source=nil; _engine=nil; _player.reset(); _play.title=@"Play";
}
- (BOOL)prepare {
    [self stop];
    if(!_decoded || _decoded->empty()) return NO;
    try { _player=std::make_shared<audition::Player>(_decoded,_sampleRate,[self currentSettings],static_cast<chronobent_profile>(_profile.indexOfSelectedItem)); }
    catch(const std::exception &error) { [self showError:[NSString stringWithUTF8String:error.what()]]; return NO; }
    @try {
    AVAudioFormat *format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:_sampleRate channels:2];
    _source=sourceNode(_player,format);
    _engine=[AVAudioEngine new]; [_engine attachNode:_source];
    [_engine connect:_source to:_engine.mainMixerNode format:format];
    // Equal headroom for processed and bypass audio; no limiter obscures A/B.
    [self outputChanged:nil];
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
        [self stop]; self->_open.enabled=NO; self->_play.enabled=NO; self->_pitch.enabled=NO; self->_tempo.enabled=NO; self->_timeline.enabled=NO; self->_timbre.enabled=NO;
        self->_statusLabel.stringValue=@"Decoding audio…";
        NSURL *url=panel.URL;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            NSError *error=nil;
            AVAudioFile *file=[[AVAudioFile alloc] initForReading:url commonFormat:AVAudioPCMFormatFloat32 interleaved:NO error:&error];
            NSString *failure=nil; std::vector<float> decoded; double rate=0;
            if(!file) failure=error.localizedDescription ?: @"Unsupported audio file.";
            else if(file.processingFormat.channelCount<1 || file.processingFormat.channelCount>2 || file.length<=0 ||
                    file.processingFormat.sampleRate<8000 || file.processingFormat.sampleRate>192000 ||
                    file.length>64000000) failure=@"Use a mono/stereo file at 8 to 192 kHz, up to 64 million frames (about 22 minutes at 48 kHz).";
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
            auto result=std::make_shared<const std::vector<float>>(std::move(decoded));
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_open.enabled=YES;
                if(failure) { self->_statusLabel.stringValue=@"Choose another audio file."; [self showError:failure]; return; }
                self->_decoded=result; self->_sampleRate=rate;
                self->_fileLabel.stringValue=url.lastPathComponent;
                [self->_waveform setAudio:*self->_decoded]; self->_timeline.doubleValue=0;
                if([self prepare]) { self->_play.enabled=YES; self->_timeline.enabled=YES; [self pitchChanged:nil]; self->_statusLabel.stringValue=@"Ready. Use headphones and compare Bypass. Leave output headroom for processed peaks."; }
            });
        });
    }];
}
- (audition::Settings)currentSettings {
    return audition::Settings{_pitch.doubleValue,_tempo.doubleValue,
        _master.state==NSControlStateValueOn,_bypass.state==NSControlStateValueOn,
        _formants.state==NSControlStateValueOn,_timbre.doubleValue,
        unsigned(_transients.selectedSegment),_envelope.doubleValue};
}
- (void)pitchChanged:(id)sender {
    (void)sender; const auto settings=[self currentSettings];
    _pitchLabel.stringValue=settings.master_tempo ? [NSString stringWithFormat:@"%+.2f st",settings.semitones] : @"Linked to speed";
    _pitchLabel.font=[NSFont systemFontOfSize:settings.master_tempo?32:25 weight:NSFontWeightLight];
    _tempoLabel.stringValue=[NSString stringWithFormat:@"%.2f%%",settings.tempo*100];
    _timbreLabel.stringValue=settings.formants ? [NSString stringWithFormat:@"%+.2f st",settings.formant_semitones] : @"Follows pitch";
    _envelopeLabel.stringValue=[NSString stringWithFormat:@"Envelope: %.2f ms",settings.envelope_ms];
    const BOOL enabled=bool(_player) && !settings.bypass;
    _pitch.enabled=enabled && settings.master_tempo;
    _tempo.enabled=enabled; _formants.enabled=enabled; _master.enabled=enabled;
    _timbre.enabled=enabled && settings.formants; _envelope.enabled=_timbre.enabled;
    _transients.enabled=enabled; _bypass.enabled=bool(_player);
    if(_player) _player->request(settings);
}
- (void)outputChanged:(id)sender {
    (void)sender;
    _gainLabel.stringValue=[NSString stringWithFormat:@"Output: %.1f dB",_gain.doubleValue];
    if(_engine) _engine.mainMixerNode.outputVolume=float(std::pow(10,_gain.doubleValue/20));
}
- (void)resetPitch:(id)sender { (void)sender; _pitch.doubleValue=0; [self pitchChanged:nil]; }
- (void)resetTimbre:(id)sender { (void)sender; _timbre.doubleValue=0; _formants.state=NSControlStateValueOn; [self pitchChanged:nil]; }
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
- (void)profileChanged:(id)sender {
    (void)sender; if(!_player) return;
    const double position=_player->source_position()/_sampleRate;
    const BOOL resume=_playing;
    if([self prepare]) { [self seekTo:position]; _resumeAfterSeek=resume; }
}
- (void)seekTo:(double)seconds {
    if(!_player || _player->error()) return;
    const auto frame=uint64_t(std::llround(std::clamp(seconds*_sampleRate,0.0,double(_player->frames()))));
    if(_player->seek(frame)) {
        _timeline.doubleValue=double(frame)/double(_player->frames());
        [_waveform setProgress:_timeline.doubleValue];
        _statusLabel.stringValue=@"Seeking...";
    }
}
- (void)seekChanged:(id)sender { (void)sender; if(_player) [self seekTo:_timeline.doubleValue*double(_player->frames())/_sampleRate]; }
- (void)skipBack:(id)sender { (void)sender; if(_player) [self seekTo:_player->source_position()/_sampleRate-5]; }
- (void)skipForward:(id)sender { (void)sender; if(_player) [self seekTo:_player->source_position()/_sampleRate+5]; }
- (void)restart:(id)sender { (void)sender; [self seekTo:0]; }
- (void)tick:(NSTimer *)timer {
    (void)timer; if(!_player) return;
    if(_resumeAfterSeek && !_player->seeking() && _player->queued()>0 && !_player->error()) {
        _resumeAfterSeek=NO; [self playPause:nil];
    }
    if(!_player->seeking()) {
        _timeline.doubleValue=_player->source_position()/double(_player->frames());
        [_waveform setProgress:_timeline.doubleValue];
    }
    unsigned at=unsigned(_player->source_position()/_sampleRate), end=unsigned(double(_player->frames())/_sampleRate);
    _timeLabel.stringValue=[NSString stringWithFormat:@"%02u:%02u  /  %02u:%02u",at/60,at%60,end/60,end%60];
    if(_player->error()) { _statusLabel.stringValue=@"Playback stopped: the engine rejected the audio."; [self stop]; }
    else if(_player->ended()) { _player->pause(true); [_engine pause]; _playing=NO; _play.title=@"Play again"; _statusLabel.stringValue=@"Finished · Play again to restart"; }
    else if(_player->seeking()) _statusLabel.stringValue=@"Seeking...";
    else if(_playing) _statusLabel.stringValue=[NSString stringWithFormat:@"%.0f Hz · pitch & tempo transitions · queue %.0f ms · underruns %u",_sampleRate,1000.0*double(_player->queued())/_sampleRate,_player->underruns()];
    else _statusLabel.stringValue=@"Paused. Space to play. Bypass uses the same output gain.";
}
// Exercise our native view hierarchy offscreen, without capturing the desktop
// or opening an audio output device. Generated images are test artifacts only.
- (int)validateUIAt:(NSString *)directory {
    if([[NSFileManager defaultManager] fileExistsAtPath:directory]) return 1;
    NSError *error=nil;
    if(![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error]) return 2;
    auto snapshot=[&](NSString *name) {
        NSView *view=_window.contentView;
        [view layoutSubtreeIfNeeded]; [view displayIfNeeded];
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
        [view layoutSubtreeIfNeeded]; [view displayIfNeeded];
        for(NSView *child in view.subviews) if(!NSContainsRect(view.bounds,child.frame)) return false;
        NSBitmapImageRep *image=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
        if(!image) return false;
        [view.effectiveAppearance performAsCurrentDrawingAppearance:^{
            [view cacheDisplayInRect:view.bounds toBitmapImageRep:image];
        }];
        NSData *png=[image representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return bool([png writeToFile:[directory stringByAppendingPathComponent:name] options:NSDataWritingWithoutOverwriting error:nil]);
    };
    if(_pitch.minValue!=-48 || _pitch.maxValue!=48 || _timbre.minValue!=-12 || _timbre.maxValue!=12) return 15;
    if(_pitch.enabled || _tempo.enabled || _timeline.enabled || _timbre.enabled || _play.enabled) return 3;
    if(!snapshot(@"empty.png")) return 4;
    std::vector<float> pcm(48000*12*2);
    for(size_t i=0;i<pcm.size()/2;++i) {
        const double t=double(i)/48000;
        const float value=float((.18+.12*std::sin(t*1.7))*std::sin(t*6.283185307179586*220));
        pcm[i*2]=value; pcm[i*2+1]=-value;
    }
    _decoded=std::make_shared<const std::vector<float>>(std::move(pcm)); _sampleRate=48000;
    _fileLabel.stringValue=@"Synthetic stereo test signal"; [_waveform setAudio:*_decoded];
    if(![self prepare]) return 5;
    _play.enabled=YES; _timeline.enabled=YES;
    _pitch.doubleValue=5; _tempo.doubleValue=.75; _formants.state=NSControlStateValueOn;
    _timbre.doubleValue=-3; _envelope.doubleValue=2.5; _transients.selectedSegment=2;
    [self pitchChanged:nil]; [self seekTo:6];
    auto waitForSeek=[&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(_player->seeking() && !_player->error()) {
            if(std::chrono::steady_clock::now()>deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !_player->error();
    };
    if(!waitForSeek()) return 6;
    [self tick:nil];
    if(!_pitch.enabled || !_timbre.enabled || _player->source_position()!=48000*6 || _timeline.doubleValue!=.5) return 7;
    if(!snapshot(@"loaded.png")) return 8;
    [self restart:nil]; if(!waitForSeek() || _player->source_position()!=0) return 9;
    [self skipForward:nil]; if(!waitForSeek() || _player->source_position()!=48000*5) return 10;
    [self skipBack:nil]; if(!waitForSeek() || _player->source_position()!=0) return 11;
    [_profile selectItemAtIndex:2]; [self profileChanged:nil];
    if(!waitForSeek() || _player->frames()!=48000*12 || _player->source_position()!=0) return 12;
    _bypass.state=NSControlStateValueOn; [self pitchChanged:nil];
    if(_pitch.minValue!=-48 || _pitch.maxValue!=48 || _timbre.minValue!=-12 || _timbre.maxValue!=12) return 15;
    if(_pitch.enabled || _tempo.enabled || _timbre.enabled || _transients.enabled) return 13;
    [self stop];
    std::printf("Native offscreen UI: empty/loaded controls, bounds, seek, skips, profile and bypass passed; images: %s\n",directory.UTF8String);
    return 0;
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
    if(argc>=2 && std::string(argv[1])=="--ui-self-test") {
        @autoreleasepool {
            uiSelfTestMode=true;
            NSApplication *app=NSApplication.sharedApplication;
            [app setActivationPolicy:NSApplicationActivationPolicyProhibited];
            PitchLab *delegate=[PitchLab new]; app.delegate=delegate;
            app.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
            [app finishLaunching];
            [delegate applicationDidFinishLaunching:[NSNotification notificationWithName:NSApplicationDidFinishLaunchingNotification object:app]];
            NSString *directory=argc==3 ? [NSString stringWithUTF8String:argv[2]] :
                [NSTemporaryDirectory() stringByAppendingPathComponent:[@"chronobent-ui-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            const int status=[delegate validateUIAt:directory];
            if(status) std::fprintf(stderr,"Native offscreen UI failed: %d\n",status);
            return status;
        }
    }
    @autoreleasepool { NSApplication *app=NSApplication.sharedApplication; PitchLab *delegate=[PitchLab new]; app.delegate=delegate; [app run]; }
    return 0;
}
