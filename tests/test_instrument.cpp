// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "instrument.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
namespace { thread_local bool realtime=false; }
void *operator new(std::size_t n) { if(realtime) std::abort(); if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }
using namespace chronobent_instrument;
static void require(bool ok,const char *why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
static int reader(void *p,uint64_t first,std::size_t n,float *out) {
    auto &s=*static_cast<const Source *>(p); std::copy_n(s.stereo.data()+2*first,2*n,out); return 1;
}
int main() {
    constexpr double rate=8000;
    Engine engine(rate); Preparation prep;
    auto source=factory_source(0,rate);
    require(engine.prepare(source,prep) && engine.wait_ready(),"factory preparation");
    const auto original=engine.snapshot().revision;
    auto invalid=prep; invalid.semitones=25;
    require(!engine.prepare(source,invalid) && engine.snapshot().revision==original,"invalid request preserves source");
    require(!engine.note_on(128,1) && !engine.note_on(0,1),"MIDI range rejection");
    require(engine.note_on(60,1),"prepared note");
    Performance performance; performance.attack_ms=.1;
    std::vector<float> l(12000),r(12000),oracle(24000);
    chronobent_config config{}; chronobent *dsp=nullptr;
    const chronobent_pitch_range range{.0625,16};
    require(chronobent_config_for_profile(rate,2,prep.profile,&config)==CHRONOBENT_OK &&
            chronobent_create_with_pitch_range(&config,&range,&dsp)==CHRONOBENT_OK,"oracle create");
    const chronobent_controls controls{1,1,0,2,2,0};
    require(chronobent_reset_controls(dsp,12000,&controls)==CHRONOBENT_OK,"oracle reset");
    std::size_t got=0;
    require(chronobent_render(dsp,reader,const_cast<Source *>(source.get()),oracle.data(),12000,&got)<=CHRONOBENT_END && got==12000,"oracle render");
    chronobent_destroy(dsp);
    for(std::size_t at=0;at<l.size();at+=64) {
        realtime=true; engine.render(l.data()+at,r.data()+at,std::min<std::size_t>(64,l.size()-at),performance,true); realtime=false;
    }
    double error=0;
    for(std::size_t i=0;i<l.size();++i) {
        const auto expected=oracle[2*i]*std::pow(10.,-9./20.);
        error=std::max(error,std::abs(l[i]-expected));
        require(std::isfinite(l[i]) && l[i]==r[i],"finite linked channels");
    }
    require(error<1e-6 && engine.snapshot().underruns==0,"prepared/streaming boundary matches standalone DSP");
    performance.loop=true; performance.release_ms=1;
    realtime=true;
    engine.reset();
    for(int i=0;i<400;++i) { engine.note_on(36+i%49,.8); engine.render(l.data(),r.data(),16,performance); }
    engine.sustain(true); engine.all_notes_off(); engine.render(l.data(),r.data(),64,performance);
    engine.sustain(false); engine.render(l.data(),r.data(),128,performance);
    realtime=false;
    require(engine.snapshot().active_voices==0,"sustain release and bounded voice stealing");
    engine.reset(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(engine.note_on(60,1),"loop note");
    for(int i=0;i<400;++i) { realtime=true; engine.render(l.data(),r.data(),64,performance,true); realtime=false; }
    require(engine.snapshot().active_voices==1,"voice crosses loop boundaries");
    std::atomic<bool> done{false};
    std::thread observer([&]{while(!done.load()) { auto snapshot=engine.snapshot(); require(snapshot.ready,"snapshot lease"); }});
    for(unsigned i=0;i<4;++i) {
        require(engine.prepare(factory_source(i,rate),prep),"replace source");
        for(unsigned n=0;n<60000;++n) {
            realtime=true; engine.render(l.data(),r.data(),64,performance); realtime=false;
            if(engine.snapshot().revision>original+i) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(engine.wait_ready(),"replacement drains prior source");
    }
    done.store(true); observer.join();
    std::printf("instrument: exact attack/stream continuity, loop, MIDI, source leases and allocation-free live render passed (max error %.9g)\n",error);
}
