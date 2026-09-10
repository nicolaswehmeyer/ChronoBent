// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/tuning.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
static std::atomic<size_t> allocations{0};
void *operator new(size_t n) { allocations.fetch_add(1);if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void *operator new[](size_t n) {return ::operator new(n);}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete[](void *p) noexcept {std::free(p);}
void operator delete(void *p,size_t) noexcept {std::free(p);}
void operator delete[](void *p,size_t) noexcept {std::free(p);}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"tuning:%d: %s\n",__LINE__,#x);std::exit(1);}} while(0)
constexpr double pi=3.14159265358979323846;
struct Source {
    std::vector<float> pcm; unsigned channels=1;int fail_after=-1;size_t reads=0;
    static int read(void *context,uint64_t first,size_t n,float *out) {
        auto &s=*static_cast<Source *>(context);CHECK((first+n)*s.channels<=s.pcm.size());
        if(s.fail_after>=0 && s.reads++>=size_t(s.fail_after))return 0;
        std::copy_n(s.pcm.data()+first*s.channels,n*s.channels,out);return 1;
    }
};
Source tone(double rate,double hz,double seconds=1.5,unsigned channels=1,bool missing=false,double vibrato=0) {
    Source s;s.channels=channels;s.pcm.resize(size_t(rate*seconds)*channels);double phase=0;
    for(size_t i=0;i<s.pcm.size()/channels;++i) {
        phase+=2*pi*hz*std::exp2(vibrato*std::sin(2*pi*5*i/rate)/12)/rate;
        const float v=missing?float(.23*std::sin(2*phase)+.19*std::sin(3*phase)+.13*std::sin(4*phase)):float(.4*std::sin(phase));
        s.pcm[i*channels]=v;if(channels==2)s.pcm[i*channels+1]=-v;
    }return s;
}
// Independent zero-crossing oracle, restricted to the single-sine fixtures.
double measured(const std::vector<float> &pcm,double rate,unsigned channels=1,double begin=.25,double end=1.25) {
    double first=0,last=0;size_t count=0;
    for(size_t i=size_t(begin*rate)+1;i<size_t(end*rate);++i) {
        double a=pcm[(i-1)*channels],b=pcm[i*channels];
        if(a<=0 && b>0){double at=double(i-1)-a/(b-a);if(!count)first=at;last=at;++count;}
    }CHECK(count>5);return double(count-1)*rate/(last-first);
}
int cancel(void *,double fraction){return fraction<.3;}
void render(chronobent_cpp::Tuner &t,Source &s,std::vector<float> &out,size_t block=4093) {
    size_t first=0,total=out.size()/s.channels;
    while(first<total) {size_t produced=0;auto status=t.render(Source::read,&s,first,out.data()+first*s.channels,std::min(block,total-first),produced);
        CHECK(produced>0);first+=produced;CHECK(status==(first==total?CHRONOBENT_END:CHRONOBENT_OK));}
}
void pitch_and_partition(double rate) {
    auto s=tone(rate,220*std::exp2(.3/12),1.5,2);const auto length=s.pcm.size()/2;
    chronobent_cpp::Tuner t(chronobent_tune_default_config(rate,2),length);
    std::vector<float> a(s.pcm.size()),b(a.size()),scratch(514);size_t count=0;
    const auto before=allocations.load();CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);
    auto notes=t.notes(count);CHECK(notes && count==1 && std::abs(notes[0].detected_midi-57.3)<.04);
    render(t,s,a);render(t,s,b,37);CHECK(a==b);
    CHECK(std::abs(1200*std::log2(measured(a,rate,2)/220))<2);
    for(size_t i=0;i<length;++i)CHECK(a[i*2]==-a[i*2+1] && std::isfinite(a[i*2]) && std::abs(a[i*2])<.55);
    for(size_t first=length-257;first>300;first-=271) {
        size_t produced=0;CHECK(t.render(Source::read,&s,first,scratch.data(),257,produced)==CHRONOBENT_OK || first+produced==length);
        CHECK(produced==257);CHECK(std::memcmp(scratch.data(),a.data()+first*2,514*sizeof(float))==0);
    }
    auto options=chronobent_tune_default_options();options.amount=0;CHECK(t.set_options(options)==CHRONOBENT_OK);render(t,s,b);CHECK(b==s.pcm);
    options.amount=1;CHECK(t.set_options(options)==CHRONOBENT_OK);CHECK(t.set_note(0,58)==CHRONOBENT_OK);render(t,s,b);
    CHECK(std::abs(1200*std::log2(measured(b,rate,2)/(220*std::exp2(1./12))))<2);
    CHECK(t.set_note(0,-1)==CHRONOBENT_OK);render(t,s,b);CHECK(a==b);
    options.reference_hz=442;CHECK(t.set_options(options)==CHRONOBENT_OK);render(t,s,b);
    CHECK(std::abs(1200*std::log2(measured(b,rate,2)/221))<2);
    chronobent_tune_edit edit{0,58};CHECK(t.set_notes(&edit,1)==CHRONOBENT_OK);
    edit.note_index=9;CHECK(t.set_notes(&edit,1)==CHRONOBENT_INVALID_ARGUMENT);
    CHECK(t.notes(count)[0].manual==1 && t.notes(count)[0].target_midi==58);
    CHECK(t.set_notes(nullptr,0)==CHRONOBENT_OK && !t.notes(count)[0].manual);
    CHECK(allocations.load()==before);
}
void recovery() {
    auto s=tone(48000,225);chronobent_cpp::Tuner t(chronobent_tune_default_config(48000,1),s.pcm.size());
    size_t n=9;float value=19;CHECK(t.render(Source::read,&s,0,&value,1,n)==CHRONOBENT_NOT_RESET && n==0 && value==19);
    CHECK(t.analyze(Source::read,&s,cancel)==CHRONOBENT_CANCELLED);CHECK(t.frames(n)==nullptr && n==0);
    s.fail_after=2;CHECK(t.analyze(Source::read,&s)==CHRONOBENT_SOURCE_UNAVAILABLE);CHECK(t.notes(n)==nullptr && n==0);
    s.fail_after=-1;CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);std::vector<float> expected(s.pcm.size()),out(expected.size(),9);
    render(t,s,expected);s.reads=0;s.fail_after=7;CHECK(t.render(Source::read,&s,0,out.data(),out.size(),n)==CHRONOBENT_SOURCE_UNAVAILABLE);
    CHECK(n<out.size());CHECK(std::memcmp(out.data(),expected.data(),n*sizeof(float))==0);CHECK(out[n]==9);
    s.fail_after=-1;size_t resumed=0;CHECK(t.render(Source::read,&s,n,out.data()+n,out.size()-n,resumed)==CHRONOBENT_END);CHECK(n+resumed==out.size() && out==expected);
}
void contracts() {
    auto s=tone(48000,225);chronobent_cpp::Tuner t(chronobent_tune_default_config(48000,1),s.pcm.size());CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);
    auto options=chronobent_tune_default_options();options.scale_mask=0;CHECK(t.set_options(options)==CHRONOBENT_INVALID_ARGUMENT);
    options=chronobent_tune_default_options();options.amount=std::numeric_limits<double>::quiet_NaN();CHECK(t.set_options(options)==CHRONOBENT_INVALID_ARGUMENT);
    CHECK(t.set_note(999,60)==CHRONOBENT_INVALID_ARGUMENT);CHECK(t.set_note(0,100)==CHRONOBENT_INVALID_ARGUMENT);
    CHECK(t.set_note(0,std::numeric_limits<double>::infinity())==CHRONOBENT_INVALID_ARGUMENT);
    auto config=chronobent_tune_default_config(48000,1);chronobent_tuner *raw=reinterpret_cast<chronobent_tuner *>(1);
    CHECK(chronobent_tune_create(&config,uint64_t(48000)*601,&raw)==CHRONOBENT_INVALID_ARGUMENT && !raw);
    config.sample_rate=std::numeric_limits<double>::infinity();CHECK(chronobent_tune_create(&config,0,&raw)==CHRONOBENT_INVALID_ARGUMENT && !raw);
    chronobent_cpp::Tuner empty(chronobent_tune_default_config(8000,1),0);CHECK(empty.analyze(nullptr,nullptr)==CHRONOBENT_OK);size_t n=2;
    CHECK(empty.render(nullptr,nullptr,0,nullptr,0,n)==CHRONOBENT_END && n==0);
    s.pcm[100]=std::numeric_limits<float>::quiet_NaN();CHECK(t.analyze(Source::read,&s)==CHRONOBENT_INVALID_AUDIO);CHECK(t.frames(n)==nullptr && n==0);
}
void difficult_sources() {
    auto s=tone(48000,220*std::exp2(.3/12),1.5,1,true);chronobent_cpp::Tuner t(chronobent_tune_default_config(48000,1),s.pcm.size());
    CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);size_t count;auto f=t.frames(count);size_t good=0;
    for(size_t i=20;i+20<count;++i)good+=std::abs(f[i].frequency_hz-223.845)<1;CHECK(good>count*.8);
    s=tone(48000,220*std::exp2(.25/12),1.5,1,false,.22);CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);
    std::vector<float> out(s.pcm.size());render(t,s,out);
    double low=1e9,high=0;for(double at=.3;at<1.15;at+=.025){const auto hz=measured(out,48000,1,at,at+.04);low=std::min(low,hz);high=std::max(high,hz);}
    CHECK(1200*std::log2(high/low)>25 && 1200*std::log2(high/low)<55);
    auto options=chronobent_tune_default_options();options.preserve_vibrato=0;options.correct_drift=1;options.retune_ms=0;CHECK(t.set_options(options)==CHRONOBENT_OK);render(t,s,out);
    low=1e9;high=0;for(double at=.3;at<1.15;at+=.025){const auto hz=measured(out,48000,1,at,at+.04);low=std::min(low,hz);high=std::max(high,hz);}
    CHECK(1200*std::log2(high/low)<8);
    uint32_t random=42;for(auto &x:s.pcm){random=1664525*random+1013904223;x=float(double(random)/4294967296.-.5)*.15f;}
    CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);t.notes(count);CHECK(count==0);render(t,s,out);CHECK(out==s.pcm);
    std::fill(s.pcm.begin(),s.pcm.end(),0);CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);render(t,s,out);CHECK(out==s.pcm);
}
void melody_and_scale() {
    const double rate=48000;Source s;
    for(double note:{57.3,59.2,61.2}) {
        auto part=tone(rate,440*std::exp2((note-69)/12),.7);
        s.pcm.insert(s.pcm.end(),part.pcm.begin(),part.pcm.end());s.pcm.insert(s.pcm.end(),size_t(rate*.08),0);
    }
    chronobent_cpp::Tuner t(chronobent_tune_default_config(rate,1),s.pcm.size());CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);
    size_t count=0;auto notes=t.notes(count);CHECK(count==3);
    auto options=chronobent_tune_default_options();options.scale_mask=(1<<0)|(1<<2)|(1<<4)|(1<<5)|(1<<7)|(1<<9)|(1<<11);
    CHECK(t.set_options(options)==CHRONOBENT_OK);notes=t.notes(count);CHECK(notes[0].target_midi==57 && notes[1].target_midi==59 && notes[2].target_midi==62);
    std::vector<float> out(s.pcm.size());render(t,s,out);
    CHECK(std::abs(1200*std::log2(measured(out,rate,1,.2,.55)/220))<2);
    CHECK(std::abs(1200*std::log2(measured(out,rate,1,.98,1.33)/(440*std::exp2((59.-69)/12))))<2);
    CHECK(std::abs(1200*std::log2(measured(out,rate,1,1.76,2.11)/(440*std::exp2((62.-69)/12))))<2);
    chronobent_tune_edit edits[]={{0,58},{2,60}};const auto before=allocations.load();CHECK(t.set_notes(edits,2)==CHRONOBENT_OK);
    notes=t.notes(count);CHECK(notes[0].manual && !notes[1].manual && notes[2].manual);
    const auto first=notes[0].target_midi;edits[1].note_index=0;CHECK(t.set_notes(edits,2)==CHRONOBENT_INVALID_ARGUMENT);CHECK(t.notes(count)[0].target_midi==first);
    CHECK(allocations.load()==before);
    options.maximum_shift=5.01;CHECK(t.set_options(options)==CHRONOBENT_INVALID_ARGUMENT);
    CHECK(t.set_note(0,notes[0].detected_midi+5.01)==CHRONOBENT_INVALID_ARGUMENT);
}
double vowel_shape(double hz) {
    return .015+std::exp(-.5*std::pow((hz-700)/140,2))+.7*std::exp(-.5*std::pow((hz-1250)/180,2))+.4*std::exp(-.5*std::pow((hz-2600)/250,2));
}
void vowel_envelope() {
    const double rate=48000,fundamental=115*std::exp2(.3/12);Source s;s.pcm.resize(96000);
    for(size_t i=0;i<s.pcm.size();++i) {
        double value=0;for(int h=1;h<45;++h)value+=vowel_shape(h*fundamental)*std::sin(2*pi*h*fundamental*i/rate+h*.7)/10;
        s.pcm[i]=float(value);
    }
    chronobent_cpp::Tuner t(chronobent_tune_default_config(rate,1),s.pcm.size());CHECK(t.analyze(Source::read,&s)==CHRONOBENT_OK);
    size_t count=0;const auto notes=t.notes(count);CHECK(count==1 && notes);const double center=notes[0].detected_midi;
    auto options=chronobent_tune_default_options();options.maximum_shift=5;options.retune_ms=0;CHECK(t.set_options(options)==CHRONOBENT_OK);
    std::vector<float> out(s.pcm.size());
    for(double shift:{-5.,-.3,5.}) {
        CHECK(t.set_note(0,center+shift)==CHRONOBENT_OK);render(t,s,out);
        const double hz=fundamental*std::exp2(shift/12);double measured[28]{},expected[28]{},product=0,power=0;
        // Independent coherent projections at the known synthetic output
        // harmonics, compared with the source's analytic spectral envelope.
        for(int h=2;h<30;++h) {
            double re=0,im=0;for(size_t i=24000;i<72000;++i){const double angle=2*pi*h*hz*i/rate;re+=out[i]*std::cos(angle);im+=out[i]*std::sin(angle);}
            measured[h-2]=2*std::hypot(re,im)/48000;expected[h-2]=vowel_shape(h*hz)/10;
            product+=measured[h-2]*expected[h-2];power+=expected[h-2]*expected[h-2];
        }
        const double gain=product/power;double error=0,reference=0,source_energy=0,output_energy=0;
        for(int h=0;h<28;++h){error+=std::pow(measured[h]-gain*expected[h],2);reference+=std::pow(gain*expected[h],2);}
        for(size_t i=0;i<out.size();++i){source_energy+=double(s.pcm[i])*s.pcm[i];output_energy+=double(out[i])*out[i];CHECK(std::isfinite(out[i]) && std::abs(out[i])<1);}
        CHECK(std::sqrt(error/reference)<.1);CHECK(std::sqrt(output_energy/source_energy)>.8 && std::sqrt(output_energy/source_energy)<1.1);
    }
}
int main(){contracts();recovery();melody_and_scale();vowel_envelope();for(double rate:{8000.,44100.,48000.,96000.,192000.})pitch_and_partition(rate);difficult_sources();std::puts("tuning: analysis, targets, vibrato, missing fundamental, linked stereo, partition/seek, retry, cancellation, allocation passed");}
