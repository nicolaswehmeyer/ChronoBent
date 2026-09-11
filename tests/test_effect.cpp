// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "effect.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
namespace { thread_local bool realtime=false; }
void *operator new(std::size_t n) { if(realtime) std::abort(); if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }
using namespace chronobent_effect;
static void require(bool condition,const char *why) {
    if(!condition) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
static void healthy(const Engine &e) {
    const auto s=e.snapshot();
    if(s.failures || s.underruns || s.dropped) std::fprintf(stderr,"stream: %s; failures=%llu underruns=%llu dropped=%llu\n",s.error.c_str(),
        static_cast<unsigned long long>(s.failures),static_cast<unsigned long long>(s.underruns),static_cast<unsigned long long>(s.dropped));
    require(s.ready && !s.failures && !s.underruns && !s.dropped,"healthy effect stream");
}
static std::vector<float> sine(std::size_t n,double rate,double frequency) {
    std::vector<float> out(n); for(std::size_t i=0;i<n;++i) out[i]=float(.2*std::sin(6.283185307179586*frequency*double(i)/rate)); return out;
}
static int oracle_reader(void *context,uint64_t first,std::size_t count,float *out) {
    const auto &input=*static_cast<const std::vector<float> *>(context);
    if(first>input.size() || count>input.size()-first) return 0;
    for(std::size_t i=0;i<count;++i) out[2*i]=out[2*i+1]=input[std::size_t(first)+i];
    return 1;
}
static void process(Engine &engine,const std::vector<float> &input,std::vector<float> &l,std::vector<float> &r,const Controls &c,std::size_t block=127) {
    for(std::size_t at=0;at<input.size();at+=block) {
        realtime=true;
        engine.process(input.data()+at,input.data()+at,l.data()+at,r.data()+at,std::min(block,input.size()-at),c,true);
        realtime=false;
    }
}
static double frequency(const std::vector<float> &a,std::size_t begin,std::size_t end,double rate) {
    std::vector<double> crossings;
    for(std::size_t i=begin+1;i<end;++i) if(a[i-1]<=0 && a[i]>0)
        crossings.push_back(double(i-1)-double(a[i-1])/(a[i]-a[i-1]));
    require(crossings.size()>5,"audible periodic signal");
    return double(crossings.size()-1)*rate/(crossings.back()-crossings.front());
}
int main() {
    constexpr double rate=8000;
    Controls controls;
    {
        Engine engine(rate); const auto latency=engine.latency_frames();
        auto input=sine(latency+24000,rate,220); input[157]+=.1f;
        std::vector<float> l(input.size()),r(input.size());
        process(engine,input,l,r,controls,31); healthy(engine);
        for(std::size_t i=0;i<input.size();++i)
            require(l[i]==r[i] && l[i]==(i<latency?0:input[i-latency]),"exact unity and advertised latency across block boundaries");
        realtime=true; engine.reset_audio(); realtime=false;
        std::fill(input.begin(),input.end(),0);
        process(engine,input,l,r,controls,47); healthy(engine);
        for(float v:l) require(v==0,"discontinuity retires every old output and dry frame");
    }
    for(double pitch:{-24.,-12.,12.,24.}) {
        Engine engine(rate); const auto latency=engine.latency_frames();
        auto input=sine(latency+16000,rate,440); std::vector<float> l(input.size()),r(input.size());
        controls.semitones=pitch;
        process(engine,input,l,r,controls); healthy(engine);
        const double measured=frequency(l,latency+8000,l.size()-512,rate),expected=440*std::exp2(pitch/12);
        require(std::abs(measured-expected)<expected*.005,"live pitch keeps input cadence and shifts frequency");
        // Identical channels share one packed transform; they agree within
        // float rounding (about -130 dB), not bit for bit.
        for(std::size_t i=0;i<l.size();++i) require(std::isfinite(l[i]) && std::abs(l[i]-r[i])<1e-5f,"finite linked channels");
        chronobent_config config{}; chronobent *oracle=nullptr;
        const chronobent_pitch_range range{.25,4};
        require(chronobent_config_for_profile(rate,2,CHRONOBENT_PROFILE_BALANCED,&config)==CHRONOBENT_OK &&
            chronobent_create_with_pitch_range(&config,&range,&oracle)==CHRONOBENT_OK,"standalone live-effect oracle");
        const chronobent_controls p{1,std::exp2(pitch/12),0,2,2,0};
        require(chronobent_reset_controls(oracle,input.size(),&p)==CHRONOBENT_OK,"effect oracle controls");
        std::vector<float> expected_audio((input.size()-latency)*2); std::size_t got=0;
        require(chronobent_render(oracle,oracle_reader,&input,expected_audio.data(),input.size()-latency,&got)==CHRONOBENT_OK && got==input.size()-latency,"effect oracle render");
        chronobent_destroy(oracle);
        for(std::size_t i=512;i<got;++i) require(std::abs(l[i+latency]-expected_audio[2*i])<1e-6,"bounded live reader exactly matches immutable-source DSP after control fade");
    }
    {
        Engine engine(rate); controls=Controls{};
        // Advance beyond the fixed history capacity before recording. The
        // capture then spans wrapped cache slots and must still own exact PCM.
        auto prelude=sine(280000,rate,440);
        std::vector<float> prelude_l(prelude.size()),prelude_r(prelude.size());
        process(engine,prelude,prelude_l,prelude_r,controls); healthy(engine);
        controls.mode=Mode::record;
        auto input=sine(240000+engine.latency_frames()+1024,rate,220);
        std::vector<float> l(input.size()),r(input.size()); process(engine,input,l,r,controls); healthy(engine);
        const auto source=engine.captured_source();
        require(source && source->stereo.size()==480000 && engine.snapshot().mode==Mode::loop && engine.snapshot().auto_completed,"thirty-second capture bound completes automatically");
        for(std::size_t i=0;i<240000;++i) require(source->stereo[2*i]==input[i] && source->stereo[2*i+1]==input[i],"wrapped input cache never relabels old PCM");
        controls.mode=Mode::loop;
        std::vector<float> short_input(8192),a(8192),b(8192); process(engine,short_input,a,b,controls); healthy(engine);
        require(!engine.snapshot().auto_completed,"capture completion acknowledgement");
        require(engine.restore_capture(nullptr),"explicitly clear captured state"); controls.mode=Mode::live;
        process(engine,short_input,a,b,controls); healthy(engine);
        require(!engine.captured_source(),"empty project state clears previous media");
    }
    for(double sample_rate:{11025.,44100.,96000.,192000.}) {
        Engine engine(sample_rate); controls=Controls{}; controls.semitones=-24; controls.formant_scale=1;
        auto input=sine(engine.latency_frames()+std::size_t(sample_rate*.4),sample_rate,880);
        std::vector<float> l(input.size()),r(input.size()); process(engine,input,l,r,controls,257); healthy(engine);
        const auto measured=frequency(l,engine.latency_frames()+std::size_t(sample_rate*.2),l.size()-256,sample_rate);
        require(std::abs(measured-220)<2,"rate-specific worst-pitch lookahead with preserved formants");
    }
    {
        Engine engine(rate); controls=Controls{};
        const auto latency=engine.latency_frames();
        std::vector<float> block(512),l(512),r(512);
        auto input=sine(8000,rate,330); std::vector<float> full_l(8000),full_r(8000);
        controls.mode=Mode::record; process(engine,input,full_l,full_r,controls);
        controls.mode=Mode::loop; controls.tempo=.75; controls.semitones=7;
        std::vector<float> silence(latency+16000),out(silence.size()),out_r(silence.size());
        process(engine,silence,out,out_r,controls); healthy(engine);
        auto captured=engine.captured_source();
        require(captured && captured->stereo.size()==16000,"capture boundaries exactly follow input frames");
        for(std::size_t i=0;i<input.size();++i) require(captured->stereo[i*2]==input[i] && captured->stereo[i*2+1]==input[i],"capture owns an exact immutable input copy");
        require(engine.snapshot().mode==Mode::loop,"record to processed loop");
        require(std::abs(frequency(out,latency+8000,out.size()-512,rate)-330*std::exp2(7./12))<3,"captured pitch independent of time");
        Engine restored(rate); require(restored.restore_capture(captured),"restore captured project source");
        process(restored,silence,out,out_r,controls); healthy(restored);
        require(restored.snapshot().mode==Mode::loop,"restored source plays without recording");
        auto invalid=std::make_shared<chronobent_instrument::Source>(*captured); invalid->stereo[0]=std::numeric_limits<float>::quiet_NaN();
        require(!restored.restore_capture(invalid),"invalid state preserves previous source");
        controls.mode=Mode::live; controls.semitones=0;
        process(engine,silence,out,out_r,controls); healthy(engine);
        for(std::size_t i=latency+2048;i<out.size();++i) require(out[i]==0 && out_r[i]==0,"return to live retires captured audio");
        require(captured->stereo[222]==input[111],"retained capture reader survives live return");
    }
    {
        Engine engine(rate); controls=Controls{}; controls.mode=Mode::record;
        std::vector<float> flat(8000,.25f),l(flat.size()),r(flat.size());
        process(engine,flat,l,r,controls);
        controls.mode=Mode::loop;
        process(engine,flat,l,r,controls); healthy(engine);
        const auto at=engine.latency_frames();
        for(std::size_t i=at;i<at+256;++i) require(std::abs(l[i]-l[i-1])<.05,"live-to-capture transition has no hard sample edge");
    }
    {
        Engine engine(rate); controls=Controls{}; controls.mode=Mode::loop;
        auto source=std::make_shared<chronobent_instrument::Source>(); source->sample_rate=rate; source->name="Concurrent capture";
        const auto tone=sine(8000,rate,440); source->stereo.resize(tone.size()*2);
        for(std::size_t i=0;i<tone.size();++i) source->stereo[2*i]=source->stereo[2*i+1]=tone[i];
        require(engine.restore_capture(source),"initial captured source");
        std::vector<float> input(16000),l(input.size()),r(input.size());
        process(engine,input,l,r,controls); healthy(engine);
        // The fixture owns its PCM independently of the effect's control inbox.
        std::thread submitter([&]{ for(int i=0;i<100;++i) { require(engine.restore_capture(source),"concurrent captured-source submission"); engine.snapshot(); std::this_thread::sleep_for(std::chrono::microseconds(20)); } });
        process(engine,input,l,r,controls); submitter.join(); healthy(engine);
        require(engine.captured_source()==source,"newest control submission owns the immutable source");
    }
    {
        Engine engine(rate); controls=Controls{};
        std::atomic<bool> done{false};
        std::thread observer([&]{while(!done.load()) { engine.snapshot(); engine.captured_source(); }});
        std::array<float,128> in{},left{},right{}; in[0]=std::numeric_limits<float>::infinity();
        for(int i=0;i<1500;++i) {
            controls.semitones=i%3==0?12:0;
            realtime=true; engine.process(in.data(),in.data(),left.data(),right.data(),in.size(),controls); realtime=false;
            for(float value:left) require(std::isfinite(value),"overload and invalid input remain finite");
        }
        done.store(true); observer.join();
        require(engine.snapshot().invalid_samples==3000,"invalid input sanitization accounting");
    }
    std::puts("effect: exact fixed latency, live pitch, linked channels, immutable capture/restore, loop/live transitions and allocation-free callback passed");
}
