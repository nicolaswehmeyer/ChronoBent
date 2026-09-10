// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import "mac_controls.hpp"
#include "mac_tuning_editor.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#define TuneGraph CB_JOIN(CHRONOBENT_TUNE_OBJC_PREFIX,TuneGraph)
#define TuneController CB_JOIN(CHRONOBENT_TUNE_OBJC_PREFIX,TuneController)
using namespace chronobent_host;
namespace {
NSColor *ink(){return [NSColor colorWithCalibratedRed:.81 green:.88 blue:.87 alpha:1];}
NSColor *muted(){return [NSColor colorWithCalibratedRed:.43 green:.54 blue:.53 alpha:1];}
NSColor *mint(){return [NSColor colorWithCalibratedRed:.48 green:.91 blue:.77 alpha:1];}
NSColor *gold(){return [NSColor colorWithCalibratedRed:.95 green:.71 blue:.43 alpha:1];}
NSColor *background(){return [NSColor colorWithCalibratedRed:.055 green:.074 blue:.075 alpha:1];}
NSTextField *text(NSString *value,CGFloat size=12,bool strong=false) {
    auto field=[NSTextField labelWithString:value];field.font=[NSFont systemFontOfSize:size weight:strong?NSFontWeightSemibold:NSFontWeightRegular];
    field.textColor=strong?ink():muted();return field;
}
NSButton *button(NSString *value,id owner,SEL action) {
    auto b=[NSButton buttonWithTitle:value target:owner action:action];b.bezelStyle=NSBezelStyleRounded;b.font=[NSFont systemFontOfSize:12 weight:NSFontWeightMedium];return b;
}
NSString *note_name(double midi) {
    static NSString *const names[]={@"C",@"C♯",@"D",@"D♯",@"E",@"F",@"F♯",@"G",@"G♯",@"A",@"A♯",@"B"};
    int n=int(std::llround(midi));if(n<0 || n>127)return @"—";
    const int cents=int(std::llround((midi-n)*100));
    return cents?[NSString stringWithFormat:@"%@%d  %+d¢",names[n%12],n/12-1,cents]:[NSString stringWithFormat:@"%@%d",names[n%12],n/12-1];
}
uint32_t mask(int root,int scale) {
    if(scale==0)return 4095;
    const int major[]={0,2,4,5,7,9,11},minor[]={0,2,3,5,7,8,10};uint32_t result=0;
    for(int i=0;i<7;++i)result|=1u<<unsigned((root+(scale==1?major[i]:minor[i]))%12);return result;
}
}
@interface TuneGraph : NSView {
@public
    std::shared_ptr<const TuneResult> _result;
    std::function<void(size_t,double)> _edit;
    std::function<void(size_t)> _select;
    NSInteger _selected;
    double _low,_high,_rate,_seconds,_pixels,_limit;
    double _dragTarget,_dragStart;
    NSPoint _dragPoint;
    BOOL _dragging;
}
- (void)setResult:(std::shared_ptr<const TuneResult>)result;
- (void)setScale:(double)pixels seconds:(double)seconds;
@end
@implementation TuneGraph
- (instancetype)initWithFrame:(NSRect)frame {
    if((self=[super initWithFrame:frame])){_selected=-1;_low=48;_high=72;_pixels=70;_rate=48000;_seconds=1;_limit=2;[self setAccessibilityLabel:@"Detected pitch and editable target notes"];[self setAccessibilityHelp:@"Left and right select a note. Up and down change its target. Option adjusts cents. Delete returns to automatic tuning."];}
    return self;
}
- (BOOL)isFlipped{return YES;}
- (BOOL)acceptsFirstResponder{return YES;}
- (BOOL)becomeFirstResponder {
    if(_result && !_result->notes.empty() && _selected<0){_selected=0;if(_select)_select(0);[self setNeedsDisplay:YES];}
    return YES;
}
- (id)accessibilityValue {
    return _result && _selected>=0?[NSString stringWithFormat:@"Note %ld of %zu, target %@",long(_selected+1),_result->notes.size(),note_name(_result->notes[size_t(_selected)].target_midi)]:@"No note selected";
}
- (double)y:(double)note{return 22+(_high-note)/(std::max(1.,_high-_low))*(self.bounds.size.height-44);}
- (NSRect)noteRect:(const chronobent_tune_note &)note target:(double)target {
    const double unit=(self.bounds.size.height-44)/std::max(1.,_high-_low);
    return NSMakeRect(54+double(note.first_frame)/_rate*_pixels,[self y:target]-.32*unit,
        std::max(4.,double(note.end_frame-note.first_frame)/_rate*_pixels),std::max(7.,.64*unit));
}
- (void)setScale:(double)pixels seconds:(double)seconds {
    _pixels=pixels;_seconds=seconds;auto frame=self.frame;frame.size.width=std::max(828.,_seconds*_pixels+72);self.frame=frame;[self setNeedsDisplay:YES];
}
- (void)setResult:(std::shared_ptr<const TuneResult>)result {
    _result=std::move(result);_dragging=NO;
    if(_result && !_result->notes.empty()) {
        _rate=_result->sample_rate;_low=127;_high=0;
        for(const auto &note:_result->notes){_low=std::min(_low,std::min(note.detected_midi,note.target_midi));_high=std::max(_high,std::max(note.detected_midi,note.target_midi));}
        const auto center=(_low+_high)*.5;const auto span=std::max(12.,_high-_low+6);_low=std::floor(center-span*.5);_high=std::ceil(center+span*.5);
        _limit=_result->options.maximum_shift;if(_selected>=NSInteger(_result->notes.size()))_selected=-1;
    }else{_low=48;_high=72;_selected=-1;}
    if(_result && !_result->notes.empty() && _selected<0 && self.window.firstResponder==self){_selected=0;if(_select)_select(0);}
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty {
    [background() setFill];NSRectFill(dirty);
    NSDictionary *attributes=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],NSForegroundColorAttributeName:muted()};
    for(int note=int(std::ceil(_low));note<=int(std::floor(_high));++note) {
        const CGFloat y=[self y:note];auto line=[NSBezierPath bezierPath];[line moveToPoint:NSMakePoint(0,y)];[line lineToPoint:NSMakePoint(self.bounds.size.width,y)];
        [[NSColor colorWithCalibratedWhite:note%12==0?.19:.12 alpha:1] setStroke];line.lineWidth=note%12==0?.8:.5;[line stroke];
        if(_high-_low<=24 || note%12==0)[note_name(note) drawAtPoint:NSMakePoint(8,y-6) withAttributes:attributes];
    }
    const int stride=_pixels<35?5:1;
    for(int second=std::max(0,int((dirty.origin.x-54)/_pixels)/stride*stride);second<=int((NSMaxX(dirty)-54)/_pixels)+stride;second+=stride) {
        double x=54+second*_pixels;auto line=[NSBezierPath bezierPath];[line moveToPoint:NSMakePoint(x,20)];[line lineToPoint:NSMakePoint(x,self.bounds.size.height-18)];
        [[NSColor colorWithCalibratedWhite:.16 alpha:1] setStroke];line.lineWidth=.5;[line stroke];
        [[NSString stringWithFormat:@"%d:%02d",second/60,second%60] drawAtPoint:NSMakePoint(x+4,3) withAttributes:attributes];
    }
    if(!_result || _result->notes.empty()) {
        [@"ANALYZE A SOLO VOICE OR MELODY" drawAtPoint:NSMakePoint(250,self.bounds.size.height*.47) withAttributes:attributes];return;
    }
    for(size_t i=0;i<_result->notes.size();++i) {
        const auto &note=_result->notes[i];const double target=_dragging && NSInteger(i)==_selected?_dragTarget:note.target_midi;
        const NSRect r=[self noteRect:note target:target];if(!NSIntersectsRect(r,dirty))continue;
        auto path=[NSBezierPath bezierPathWithRoundedRect:r xRadius:3 yRadius:3];NSColor *color=note.manual?gold():mint();
        [[color colorWithAlphaComponent:NSInteger(i)==_selected?.35:.18] setFill];[path fill];[[color colorWithAlphaComponent:.8] setStroke];path.lineWidth=NSInteger(i)==_selected?1.5:.7;[path stroke];
    }
    for(int corrected=0;corrected<2;++corrected) {
        auto path=[NSBezierPath bezierPath];BOOL connected=NO;
        const double first=std::max(0.,(dirty.origin.x-60)/_pixels),end=(NSMaxX(dirty)-48)/_pixels;
        auto frame=std::lower_bound(_result->frames.begin(),_result->frames.end(),first*_rate,[](const auto &f,double position){return f.source_frame<position;});
        for(;frame!=_result->frames.end() && frame->source_frame<=end*_rate;++frame) {
            if(frame->frequency_hz<=0){connected=NO;continue;}
            const double note=69+12*std::log2(frame->frequency_hz/_result->options.reference_hz)+(corrected?frame->correction_st:0);
            const NSPoint p=NSMakePoint(54+frame->source_frame/_rate*_pixels,[self y:note]);
            if(connected)[path lineToPoint:p];else[path moveToPoint:p];connected=YES;
        }
        [(corrected?mint():[muted() colorWithAlphaComponent:.7]) setStroke];path.lineWidth=corrected?1.3:1;[path stroke];
    }
}
- (void)mouseDown:(NSEvent *)event {
    [self.window makeFirstResponder:self];if(!_result)return;auto point=[self convertPoint:event.locationInWindow fromView:nil];
    _selected=-1;
    for(size_t i=0;i<_result->notes.size();++i)if(NSPointInRect(point,NSInsetRect([self noteRect:_result->notes[i] target:_result->notes[i].target_midi],0,-6))) {_selected=NSInteger(i);break;}
    if(_selected>=0){_dragPoint=point;_dragStart=_dragTarget=_result->notes[size_t(_selected)].target_midi;_dragging=YES;if(_select)_select(size_t(_selected));}
    [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent *)event {
    if(!_dragging || _selected<0)return;auto point=[self convertPoint:event.locationInWindow fromView:nil];
    const double value=_dragStart-(point.y-_dragPoint.y)*(_high-_low)/(self.bounds.size.height-44);
    const auto &note=_result->notes[size_t(_selected)];const double step=event.modifierFlags&NSEventModifierFlagOption?.01:1;
    _dragTarget=std::clamp(std::round(value/step)*step,std::max(0.,note.detected_midi-_limit),std::min(127.,note.detected_midi+_limit));[self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent *)event {
    (void)event;if(_dragging && _selected>=0 && _edit && std::abs(_dragTarget-_dragStart)>.001)_edit(size_t(_selected),_dragTarget);
    _dragging=NO;[self setNeedsDisplay:YES];
}
- (void)keyDown:(NSEvent *)event {
    if(!_result || _selected<0 || !_edit || !event.characters.length){[super keyDown:event];return;}
    const auto c=[event.characters characterAtIndex:0];auto note=_result->notes[size_t(_selected)];
    if(c==NSLeftArrowFunctionKey || c==NSRightArrowFunctionKey) {
        _selected=std::clamp(_selected+(c==NSLeftArrowFunctionKey?-1:1),NSInteger(0),NSInteger(_result->notes.size()-1));
        if(_select)_select(size_t(_selected));const auto &selected=_result->notes[size_t(_selected)];[self scrollRectToVisible:[self noteRect:selected target:selected.target_midi]];[self setNeedsDisplay:YES];return;
    }
    if(c==NSDeleteCharacter || c==NSBackspaceCharacter || c==NSDeleteFunctionKey){_edit(size_t(_selected),-1);return;}
    if(c==NSUpArrowFunctionKey || c==NSDownArrowFunctionKey) {
        const double step=(event.modifierFlags&NSEventModifierFlagOption)?.01:1;
        _edit(size_t(_selected),std::clamp(note.target_midi+(c==NSUpArrowFunctionKey?step:-step),std::max(0.,note.detected_midi-_limit),std::min(127.,note.detected_midi+_limit)));return;
    }[super keyDown:event];
}
@end

@interface TuneController : NSWindowController <NSWindowDelegate> {
@public
    MacTuningEditor::Document _document;
    MacTuningEditor::Apply _apply;
    std::unique_ptr<TuningSession> _session;
    TuneDocument _current;
    std::shared_ptr<const TuneResult> _shown;
    chronobent_tune_options _options;
    std::vector<TuneEdit> _edits;
    TuneGraph *_graph;
    NSScrollView *_scroll;
    NSTextField *_name,*_status,*_selection,*_target,*_reference;
    NSTextField *_amountLabel,*_retuneLabel,*_vibratoLabel,*_driftLabel;
    CBNativeSlider *_amount,*_retune,*_vibrato,*_drift,*_zoom;
    NSPopUpButton *_root,*_scale,*_limit;
    NSButton *_analyze,*_original,*_corrected,*_automatic,*_cancel;
    NSProgressIndicator *_progress;
    NSTimer *_timer;
    BOOL _updating;
}
- (void)poll;
- (void)editNote:(size_t)index target:(double)target;
- (void)analyze:(id)sender;
- (void)changed:(id)sender;
- (void)selectOriginal:(id)sender;
- (void)selectCorrected:(id)sender;
@end
@implementation TuneController
- (instancetype)init {
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,660) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    if((self=[super initWithWindow:window])) {
        self.window.title=@"ChronoBent · Note Studio";self.window.releasedWhenClosed=NO;self.window.delegate=self;self.window.backgroundColor=background();
        self.window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];_session=std::make_unique<TuningSession>();_options=chronobent_tune_default_options();
        NSView *view=self.window.contentView;view.wantsLayer=YES;view.layer.backgroundColor=background().CGColor;
        auto title=text(@"NOTE STUDIO",24,true);title.frame=NSMakeRect(28,610,300,32);[view addSubview:title];
        auto edition=text(@"CHRONOBENT 0.6",11,true);edition.frame=NSMakeRect(726,616,145,22);edition.alignment=NSTextAlignmentRight;[view addSubview:edition];
        _name=text(@"A little more intention in every note.",12);_name.frame=NSMakeRect(29,585,680,22);[view addSubview:_name];
        _analyze=button(@"Analyze melody",self,@selector(analyze:));_analyze.frame=NSMakeRect(27,540,151,32);[view addSubview:_analyze];
        _root=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(222,541,73,28) pullsDown:NO];[_root addItemsWithTitles:@[@"C",@"C♯",@"D",@"D♯",@"E",@"F",@"F♯",@"G",@"G♯",@"A",@"A♯",@"B"]];_root.target=self;_root.action=@selector(changed:);[_root setAccessibilityLabel:@"Scale root"];[view addSubview:_root];
        auto key=text(@"KEY",10,true);key.frame=NSMakeRect(186,546,32,18);[view addSubview:key];
        _scale=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(300,541,125,28) pullsDown:NO];[_scale addItemsWithTitles:@[@"Chromatic",@"Major",@"Natural minor",@"Custom scale"]];_scale.target=self;_scale.action=@selector(changed:);[_scale setAccessibilityLabel:@"Pitch correction scale"];[view addSubview:_scale];
        auto tuning=text(@"A4",10,true);tuning.frame=NSMakeRect(445,546,25,18);[view addSubview:tuning];
        _reference=[[NSTextField alloc] initWithFrame:NSMakeRect(473,542,52,26)];_reference.doubleValue=440;_reference.target=self;_reference.action=@selector(changed:);[_reference setAccessibilityLabel:@"Reference frequency in hertz"];[view addSubview:_reference];
        _limit=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(547,541,152,28) pullsDown:NO];[_limit addItemsWithTitles:@[@"Limit ±1 semitone",@"Limit ±2 semitones",@"Limit ±5 semitones"]];[_limit selectItemAtIndex:1];_limit.target=self;_limit.action=@selector(changed:);[_limit setAccessibilityLabel:@"Maximum pitch correction"];[view addSubview:_limit];
        _zoom=[CBNativeSlider sliderWithValue:70 minValue:15 maxValue:220 target:self action:@selector(zoom:)];_zoom.continuous=YES;_zoom.frame=NSMakeRect(760,543,110,25);[_zoom setAccessibilityLabel:@"Horizontal note zoom"];[view addSubview:_zoom];
        auto zoom=text(@"ZOOM",9,true);zoom.frame=NSMakeRect(716,547,40,16);[view addSubview:zoom];
        _scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(28,226,844,294)];_scroll.hasHorizontalScroller=YES;_scroll.borderType=NSBezelBorder;_scroll.drawsBackground=YES;_scroll.backgroundColor=background();
        _graph=[[TuneGraph alloc] initWithFrame:NSMakeRect(0,0,828,276)];_scroll.documentView=_graph;[view addSubview:_scroll];
        __weak TuneController *weak=self;
        _graph->_edit=[weak](size_t i,double target){[weak editNote:i target:target];};
        _graph->_select=[weak](size_t i){auto strong=weak;if(strong && strong->_shown && i<strong->_shown->notes.size()){const auto &n=strong->_shown->notes[i];strong->_target.doubleValue=n.target_midi;strong->_selection.stringValue=[NSString stringWithFormat:@"%@ →",note_name(n.detected_midi)];}};
        _selection=text(@"Select a note",11,true);_selection.frame=NSMakeRect(29,197,120,22);[view addSubview:_selection];
        _target=[[NSTextField alloc] initWithFrame:NSMakeRect(153,195,66,25)];_target.target=self;_target.action=@selector(targetChanged:);[_target setAccessibilityLabel:@"Selected target MIDI note"];_target.placeholderString=@"MIDI";[view addSubview:_target];
        _automatic=button(@"Reset note",self,@selector(resetNote:));_automatic.frame=NSMakeRect(226,193,105,28);[view addSubview:_automatic];
        auto hint=text(@"Drag ↑↓ · Option for cents · Delete to reset",11);hint.frame=NSMakeRect(355,197,345,22);[view addSubview:hint];
        auto legend=text(@"SOURCE   /   TUNED",9,true);legend.frame=NSMakeRect(715,198,150,20);legend.textColor=mint();[view addSubview:legend];
        auto control=[&](NSString *title,double value,double maximum,CGFloat x,CBNativeSlider *__strong *slider,NSTextField *__strong *valueLabel) {
            auto label=text(title,10,true);label.frame=NSMakeRect(x,164,180,18);[view addSubview:label];
            *slider=[CBNativeSlider sliderWithValue:value minValue:0 maxValue:maximum target:self action:@selector(changed:)];(*slider).continuous=NO;(*slider).frame=NSMakeRect(x,134,157,26);[view addSubview:*slider];[*slider setAccessibilityLabel:title];
            *valueLabel=text(@"",10);(*valueLabel).frame=NSMakeRect(x+160,138,50,20);[view addSubview:*valueLabel];
        };
        control(@"CORRECTION",100,100,29,&_amount,&_amountLabel);control(@"RETUNE",80,400,245,&_retune,&_retuneLabel);
        control(@"KEEP VIBRATO",100,100,461,&_vibrato,&_vibratoLabel);control(@"CORRECT DRIFT",0,100,677,&_drift,&_driftLabel);
        _original=button(@"Use original",self,@selector(selectOriginal:));_original.frame=NSMakeRect(27,74,145,34);[view addSubview:_original];
        _corrected=button(@"Use tuned audio",self,@selector(selectCorrected:));_corrected.frame=NSMakeRect(178,74,171,34);_corrected.contentTintColor=mint();[view addSubview:_corrected];
        _cancel=button(@"Cancel analysis",self,@selector(cancel:));_cancel.frame=NSMakeRect(722,75,151,32);[view addSubview:_cancel];
        _progress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(370,83,327,12)];_progress.indeterminate=NO;_progress.minValue=0;_progress.maxValue=1;[view addSubview:_progress];
        _status=text(@"For a single voice or melody. Uncertain sections keep their original sound.",11);_status.frame=NSMakeRect(29,29,840,30);_status.maximumNumberOfLines=2;[view addSubview:_status];
        [self updateLabels];[self.window center];
    }return self;
}
- (void)updateLabels {
    _amountLabel.stringValue=[NSString stringWithFormat:@"%.0f%%",_amount.doubleValue];_retuneLabel.stringValue=[NSString stringWithFormat:@"%.0f ms",_retune.doubleValue];
    _vibratoLabel.stringValue=[NSString stringWithFormat:@"%.0f%%",_vibrato.doubleValue];_driftLabel.stringValue=[NSString stringWithFormat:@"%.0f%%",_drift.doubleValue];
}
- (void)restoreControls:(chronobent_tune_options)options {
    _updating=YES;_options=options;_amount.doubleValue=options.amount*100;_retune.doubleValue=options.retune_ms;_vibrato.doubleValue=options.preserve_vibrato*100;_drift.doubleValue=options.correct_drift*100;
    _reference.doubleValue=options.reference_hz;[_limit selectItemAtIndex:options.maximum_shift<=1?0:options.maximum_shift<=2?1:2];
    BOOL found=NO;for(int scale=0;scale<3 && !found;++scale)for(int root=0;root<12;++root)if(mask(root,scale)==options.scale_mask){[_root selectItemAtIndex:root];[_scale selectItemAtIndex:scale];found=YES;break;}
    if(!found)[_scale selectItemAtIndex:3];[self updateLabels];_updating=NO;
}
- (void)showWindow:(id)sender {
    [self poll];[super showWindow:sender];[self.window makeKeyAndOrderFront:nil];
    if(!_timer){__weak TuneController *weak=self;_timer=[NSTimer scheduledTimerWithTimeInterval:.1 repeats:YES block:^(NSTimer *){[weak poll];}];}
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;[_timer invalidate];_timer=nil;
}
- (void)poll {
    if(!_document)return;
    auto document=_document();
    if(document.original!=_current.original || document.sample_rate!=_current.sample_rate) {
        _session->cancel();_shown.reset();_edits.clear();_current=document;_graph->_selected=-1;[_graph setResult:nullptr];
        if(document.original) {
            if(document.correction && document.correction->original==document.original)_session->restore(document.correction);
            else _session->set_source(document.original,document.sample_rate);
            [self restoreControls:document.correction?document.correction->options:chronobent_tune_default_options()];
            if(document.correction)_edits=document.correction->edits;
            const double duration=double(document.original->size()/2)/document.sample_rate;
            _zoom.doubleValue=std::clamp(756./std::max(.01,duration),15.,220.);
            [_graph setScale:_zoom.doubleValue seconds:duration];
        }else _session->clear();
    }else _current=document;
    _name.stringValue=document.original?[NSString stringWithFormat:@"%@ · %.2f s · Solo voice / melody",[NSString stringWithUTF8String:document.name.c_str()],double(document.original->size()/2)/document.sample_rate]:@"Load a sample or finish a capture to begin.";
    const auto snapshot=_session->snapshot();
    if(snapshot.result!=_shown) {_shown=snapshot.result;[_graph setResult:_shown];if(_shown){_edits=_shown->edits;[self restoreControls:_shown->options];}}
    const bool analyzed=_shown && !_shown->frames.empty();
    _analyze.enabled=bool(document.original) && !snapshot.busy;_cancel.hidden=!snapshot.busy;_progress.hidden=!snapshot.busy;_progress.doubleValue=snapshot.progress;
    for(NSControl *control in @[_root,_scale,_reference,_limit,_amount,_retune,_vibrato,_drift])control.enabled=analyzed && !snapshot.busy;
    _corrected.enabled=analyzed && !snapshot.busy;_original.enabled=bool(_shown);_target.enabled=analyzed && !snapshot.busy && _graph->_selected>=0;_automatic.enabled=_target.enabled;
    _original.title=document.corrected?@"Use original":@"✓ Original in use";
    _corrected.title=document.corrected && document.correction==_shown?@"✓ Tuned audio in use":@"Use tuned audio";
    if(!snapshot.error.empty())_status.stringValue=[NSString stringWithUTF8String:snapshot.error.c_str()];
    else if(snapshot.busy)_status.stringValue=[NSString stringWithFormat:@"Preparing your melody… %.0f%%",snapshot.progress*100];
    else if(analyzed)_status.stringValue=[NSString stringWithFormat:@"%zu notes · %@ · Uncertain sections stay unchanged. Choose Use tuned audio to apply your edit.",_shown->notes.size(),document.corrected?@"Tuned audio selected":@"Original selected"];
    else if(_shown)_status.stringValue=@"Saved audio is ready. Analyze to reopen the editable note contour.";
    else _status.stringValue=@"For a single voice or melody. Analyze first, then choose a scale or move individual notes.";
}
- (void)analyze:(id)sender {(void)sender;if(_session->analyze())[self poll];}
- (void)cancel:(id)sender {(void)sender;_session->cancel();[self poll];}
- (void)zoom:(id)sender {(void)sender;[_graph setScale:_zoom.doubleValue seconds:_graph->_seconds];}
- (void)changed:(id)sender {
    (void)sender;if(_updating)return;
    auto options=_options;options.amount=_amount.doubleValue/100;options.retune_ms=_retune.doubleValue;options.preserve_vibrato=_vibrato.doubleValue/100;options.correct_drift=_drift.doubleValue/100;
    options.reference_hz=_reference.doubleValue;options.maximum_shift=_limit.indexOfSelectedItem==0?1:_limit.indexOfSelectedItem==1?2:5;
    if(_scale.indexOfSelectedItem<3)options.scale_mask=mask(int(_root.indexOfSelectedItem),int(_scale.indexOfSelectedItem));
    if(_session->apply(options,_edits)){_options=options;[self updateLabels];[self poll];}else{[self restoreControls:_options];_status.stringValue=@"Use A4 from 400 to 480 Hz and analyze a melody before editing.";}
}
- (void)editNote:(size_t)index target:(double)target {
    if(!_shown || index>=_shown->notes.size() || _session->snapshot().busy)return;
    const auto &note=_shown->notes[index];
    if(target!=-1 && std::abs(target-note.detected_midi)>_options.maximum_shift){_status.stringValue=@"Raise the correction limit to move this note further (up to five semitones).";return;}
    auto edits=_edits;
    edits.erase(std::remove_if(edits.begin(),edits.end(),[&](const auto &edit){return edit.first==note.first_frame;}),edits.end());
    if(target!=-1)edits.push_back({note.first_frame,note.end_frame,target});
    std::sort(edits.begin(),edits.end(),[](const auto &a,const auto &b){return a.first<b.first;});
    if(_session->apply(_options,edits)){_edits=std::move(edits);_target.doubleValue=target<0?note.detected_midi:target;[self poll];}
    else _status.stringValue=@"Choose a MIDI target within five semitones of the detected note.";
}
- (void)targetChanged:(id)sender {(void)sender;if(_graph->_selected>=0)[self editNote:size_t(_graph->_selected) target:_target.doubleValue];}
- (void)resetNote:(id)sender {(void)sender;if(_graph->_selected>=0)[self editNote:size_t(_graph->_selected) target:-1];}
- (void)selectOriginal:(id)sender {(void)sender;if(_shown && _apply && !_apply(_shown,false))_status.stringValue=@"The source changed. Reopen Note Studio for the current source.";else[self poll];}
- (void)selectCorrected:(id)sender {(void)sender;if(_shown && !_session->snapshot().busy && _apply && !_apply(_shown,true))_status.stringValue=@"The source changed. Reopen Note Studio for the current source.";else[self poll];}
@end
namespace chronobent_host {
struct MacTuningEditor::Impl {
    TuneController *controller;
    Impl(Document document,Apply apply):controller([TuneController new]){controller->_document=std::move(document);controller->_apply=std::move(apply);}
    ~Impl(){[controller close];controller->_document={};controller->_apply={};controller->_session->cancel();controller=nil;}
};
MacTuningEditor::MacTuningEditor(Document document,Apply apply):impl_(std::make_unique<Impl>(std::move(document),std::move(apply))){}
MacTuningEditor::~MacTuningEditor(){
    if([NSThread isMainThread])impl_.reset();
    else {auto p=impl_.release();dispatch_sync(dispatch_get_main_queue(),^{delete p;});}
}
void MacTuningEditor::show(){[impl_->controller showWindow:nil];}
void MacTuningEditor::close(){[impl_->controller close];}
bool MacTuningEditor::self_test(const std::string &path) {
    auto c=impl_->controller;[c showWindow:nil];[c analyze:nil];
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(c->_session->snapshot().busy && std::chrono::steady_clock::now()<end)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    [c poll];auto first=c->_session->snapshot().result;if(!first || first->notes.empty())return false;
    c->_graph->_selected=0;[c editNote:0 target:std::round(first->notes[0].detected_midi)+1];
    if(!c->_session->wait(30000))return false;[c poll];auto edited=c->_session->snapshot().result;
    if(!edited || !edited->notes[0].manual)return false;
    [c selectCorrected:nil];
    for(int i=0;i<3000 && !c->_document().corrected;++i)[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    if(!c->_document().corrected)return false;
    [c selectOriginal:nil];
    for(int i=0;i<3000 && c->_document().corrected;++i)[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    if(c->_document().corrected)return false;
    c->_amount.doubleValue=83;[c changed:nil];if(!c->_session->wait(30000))return false;[c poll];
    if(c->_session->snapshot().result->options.amount!=.83)return false;
    if(!path.empty()) {
        [c.window.contentView layoutSubtreeIfNeeded];[c.window.contentView displayIfNeeded];
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
        [c.window.contentView layoutSubtreeIfNeeded];[c.window.contentView displayIfNeeded];auto bitmap=[c.window.contentView bitmapImageRepForCachingDisplayInRect:c.window.contentView.bounds];
        [c.window.contentView.effectiveAppearance performAsCurrentDrawingAppearance:^{
            [c.window.contentView cacheDisplayInRect:c.window.contentView.bounds toBitmapImageRep:bitmap];
        }];
        NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];if(![png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES])return false;
    }
    return c->_graph.bounds.size.height>200 && c->_corrected.enabled && c->_original.enabled;
}
}
