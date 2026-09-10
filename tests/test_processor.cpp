// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
bool forbid_allocation = false;
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "processor: %s\n", message); std::abort(); }
}
struct Source {
    std::vector<float> pcm;
    unsigned calls = 0, fail_at = 0;
    bool unavailable = false, invalid = false;
    explicit Source(std::size_t frames) : pcm(frames * 2) {
        for (std::size_t i = 0; i < frames; ++i) {
            pcm[2*i] = float(.3*std::sin(double(i)*.039));
            pcm[2*i+1] = float(.2*std::cos(double(i)*.061));
        }
    }
    static int read(void *context, uint64_t first, size_t count, float *out) {
        auto &s = *static_cast<Source *>(context);
        require(first <= s.pcm.size()/2 && count <= s.pcm.size()/2-first, "source bounds");
        if (s.unavailable || ++s.calls == s.fail_at) return 0;
        std::copy_n(s.pcm.data()+first*2,count*2,out);
        if (s.invalid && count) out[0] = std::numeric_limits<float>::quiet_NaN();
        return 1;
    }
};
chronobent_config config() { auto c = chronobent_default_config(48000,2); c.window_frames = 512; return c; }
chronobent_processor_state state(const chronobent_cpp::Processor &p) {
    chronobent_processor_state s{};
    require(p.state(s)==CHRONOBENT_OK, "state"); return s;
}
void basic() {
    Source source(12000);
    chronobent_cpp::Processor processor(config(),31);
    float out[100]{};
    std::size_t got = 99;
    require(processor.render(out,50,got)==CHRONOBENT_NOT_RESET && !got,"unbound");
    require(processor.set_source(Source::read,&source,12000)==CHRONOBENT_OK,"bind");
    auto bad = chronobent_default_parameters(); bad.tempo = std::numeric_limits<double>::quiet_NaN();
    require(processor.set_parameters(bad)==CHRONOBENT_INVALID_ARGUMENT,"NaN rejected");
    require(chronobent_processor_set_source(processor.get(),nullptr,nullptr,100,&bad)==CHRONOBENT_INVALID_ARGUMENT,"bad bind");
    require(state(processor).input_frames==12000,"bind rollback");
    forbid_allocation = true;
    require(processor.render(out,50,got)==CHRONOBENT_OK && got==50,"identity render");
    require(!std::memcmp(out,source.pcm.data(),sizeof(out)),"identity exact");
    require(processor.seek(9999)==CHRONOBENT_OK,"seek");
    require(processor.render(out,50,got)==CHRONOBENT_OK && got==50,"seek render");
    require(!std::memcmp(out,source.pcm.data()+9999*2,sizeof(out)),"seek exact");
    require(state(processor).delivered_frames==100 && state(processor).source_position==10049,"seek timeline");
    require(processor.seek(12000)==CHRONOBENT_OK,"seek eof");
    require(processor.render(out,50,got)==CHRONOBENT_END && !got,"eof");
    require(processor.seek(0)==CHRONOBENT_OK,"restart eof");
    auto p = chronobent_default_parameters(); p.tempo=1.31; p.pitch=.8; p.formants=1;
    require(processor.set_parameters(p)==CHRONOBENT_OK,"parameter transition");
    require(state(processor).transition_remaining==31,"fade length");
    require(processor.set_parameters(p)==CHRONOBENT_OK,"same target");
    p.pitch=1.2;
    require(processor.set_parameters(p)==CHRONOBENT_BUSY,"busy change");
    require(processor.seek(0)==CHRONOBENT_BUSY,"busy seek");
    require(processor.render(out,50,got)==CHRONOBENT_OK && got==50,"finish fade");
    require(state(processor).transition_remaining==0,"fade complete");
    require(processor.set_parameters(p)==CHRONOBENT_OK,"next transition");
    require(processor.set_source(nullptr,nullptr,0)==CHRONOBENT_OK,"empty replacement");
    require(processor.render(nullptr,0,got)==CHRONOBENT_END && !got,"empty render");
    forbid_allocation = false;
    auto moved = std::move(processor);
    require(state(moved).ended && !state(moved).delivered_frames,"move ownership");
    require(processor.render(out,1,got)==CHRONOBENT_INVALID_ARGUMENT,"moved from");
}
std::vector<float> scenario(std::size_t block, bool planar, unsigned fail_at = 0, bool fail_controls = false) {
    Source source(16000);
    chronobent_cpp::Processor processor(config(),1024);
    auto parameters = chronobent_default_parameters();
    parameters.tempo=1.079321; parameters.pitch=1.3;
    require(processor.set_source(Source::read,&source,16000,parameters)==CHRONOBENT_OK,"scenario bind");
    std::array<float,8192> buffer{};
    std::array<float,4096> left{},right{};
    float *planes[]{left.data(),right.data()};
    std::vector<float> result;
    std::uint64_t delivered = 0;
    bool changed = false;
    for (;;) {
        if (!changed && delivered==6000) {
            parameters.tempo=.731; parameters.pitch=.8; parameters.formants=1; parameters.transients=0;
            if (fail_controls) {
                const auto before=state(processor);
                source.unavailable=true;
                require(processor.set_parameters(parameters)==CHRONOBENT_SOURCE_UNAVAILABLE,"control read error");
                require(processor.seek(8000)==CHRONOBENT_SOURCE_UNAVAILABLE,"seek read error");
                source.unavailable=false;
                const auto after=state(processor);
                require(before.source_position==after.source_position && before.parameters.pitch==after.parameters.pitch &&
                    before.delivered_frames==after.delivered_frames && !after.transition_remaining,"control rollback");
            }
            require(processor.set_parameters(parameters)==CHRONOBENT_OK,"scenario change");
            source.calls=0; source.fail_at=fail_at;
            changed=true;
        }
        const auto count=std::size_t(std::min<std::uint64_t>(block,changed ? block : 6000-delivered));
        std::fill(buffer.begin(),buffer.end(),17.0f);
        std::fill(left.begin(),left.end(),17.0f); std::fill(right.begin(),right.end(),17.0f);
        std::size_t got=99;
        const auto status=planar ? processor.render_planar(planes,count,got) : processor.render(buffer.data(),count,got);
        require(got<=count,"count bound");
        if (planar) {
            require(left[got]==17 && right[got]==17,"planar tail");
            for (std::size_t i=0;i<got;++i) { buffer[2*i]=left[i]; buffer[2*i+1]=right[i]; }
        } else require(buffer[2*got]==17,"interleaved tail");
        result.insert(result.end(),buffer.begin(),buffer.begin()+got*2);
        delivered+=got;
        require(state(processor).delivered_frames==delivered,"delivered count");
        if (status==CHRONOBENT_SOURCE_UNAVAILABLE) continue;
        require(status==CHRONOBENT_OK || status==CHRONOBENT_END,"scenario status");
        if (status==CHRONOBENT_END) break;
    }
    require(state(processor).source_position==16000 && state(processor).ended,"final state");
    return result;
}
void advanced() {
    Source source(16000);
    auto c = config();
    for (auto profile : {CHRONOBENT_PROFILE_COMPACT, CHRONOBENT_PROFILE_BALANCED, CHRONOBENT_PROFILE_DETAILED}) {
        require(chronobent_config_for_profile(48000,2,profile,&c)==CHRONOBENT_OK,"profile");
        require(c.window_frames==(1024u << unsigned(profile)),"profile window");
    }
    require(chronobent_config_for_profile(48000,2,static_cast<chronobent_profile>(3),&c)==CHRONOBENT_INVALID_ARGUMENT &&
        c.window_frames==4096,"profile rollback");
    chronobent_cpp::Processor processor(config(),64);
    auto p=chronobent_default_controls();
    p.formant_scale=.75; p.envelope_ms=3; p.transients=CHRONOBENT_TRANSIENT_MIXED;
    require(processor.set_source_controls(Source::read,&source,16000,p)==CHRONOBENT_OK,"advanced bind");
    chronobent_controls actual{};
    require(processor.controls(actual)==CHRONOBENT_OK && actual.formant_scale==.75 && actual.transients==2,"advanced query");
    require(state(processor).parameters.transients==1 && state(processor).parameters.formants==1,"legacy projection");
    std::array<float,512> out{};
    std::size_t got=0;
    forbid_allocation=true;
    require(processor.render(out.data(),256,got)==CHRONOBENT_OK && got==256,"formant only render");
    require(processor.seek(9000)==CHRONOBENT_OK,"advanced seek");
    p.formant_scale=1.5; p.envelope_ms=1;
    source.unavailable=true;
    require(processor.set_controls(p)==CHRONOBENT_SOURCE_UNAVAILABLE,"advanced preroll failure");
    require(processor.controls(actual)==CHRONOBENT_OK && actual.formant_scale==.75,"advanced rollback");
    source.unavailable=false;
    require(processor.set_controls(p)==CHRONOBENT_OK,"advanced transition");
    require(processor.set_controls(p)==CHRONOBENT_OK,"advanced idempotent target");
    require(processor.render(out.data(),256,got)==CHRONOBENT_OK,"advanced fade");
    for (int field=0;field<6;++field) {
        auto bad=p;
        if(field==0) bad.formant_scale=std::numeric_limits<double>::quiet_NaN();
        if(field==1) bad.formant_scale=.49;
        if(field==2) bad.envelope_ms=std::numeric_limits<double>::infinity();
        if(field==3) bad.envelope_ms=.99;
        if(field==4) bad.transients=3;
        if(field==5) bad.reserved=1;
        require(processor.set_controls(bad)==CHRONOBENT_INVALID_ARGUMENT,"invalid extended controls");
        require(processor.controls(actual)==CHRONOBENT_OK && actual.formant_scale==1.5 && actual.envelope_ms==1,"invalid controls preserve target");
    }
    require(processor.set_parameters(chronobent_default_parameters())==CHRONOBENT_OK,"restore legacy controls");
    require(processor.controls(actual)==CHRONOBENT_OK && actual.formant_scale==0 && actual.envelope_ms==2 && actual.transients==1,"legacy defaults restored");
    forbid_allocation=false;
}
void failures() {
    Source source(10000);
    chronobent_cpp::Processor processor(config(),64);
    require(processor.set_source(Source::read,&source,10000)==CHRONOBENT_OK,"failure bind");
    std::array<float,2000> out{};
    out.fill(17);
    source.fail_at=2;
    std::size_t got=0;
    require(processor.render(out.data(),1000,got)==CHRONOBENT_SOURCE_UNAVAILABLE && got==512,"valid error prefix");
    require(!std::memcmp(out.data(),source.pcm.data(),got*2*sizeof(float)) && out[got*2]==17,"prefix and tail");
    source.invalid=true;
    require(processor.render(out.data(),10,got)==CHRONOBENT_INVALID_AUDIO && !got,"invalid audio");
    source.invalid=false;
    require(processor.render(out.data(),10,got)==CHRONOBENT_OK && got==10,"retry invalid audio");
    require(!std::memcmp(out.data(),source.pcm.data()+512*2,20*sizeof(float)),"no lost samples");
    require(processor.render(nullptr,1,got)==CHRONOBENT_INVALID_ARGUMENT && !got,"null output");
    float *planes[]{out.data(),nullptr};
    require(processor.render_planar(planes,1,got)==CHRONOBENT_INVALID_ARGUMENT,"null plane");
    require(processor.seek(10001)==CHRONOBENT_INVALID_ARGUMENT,"seek range");
}
}
void *operator new(std::size_t bytes) {
    require(!forbid_allocation,"unexpected allocation in processor call");
    if (auto *p=std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
int main() {
    basic(); failures(); advanced();
    const auto reference=scenario(1,false);
    for (const auto block : {std::size_t(7),std::size_t(257),std::size_t(2048)}) {
        require(scenario(block,false)==reference,"block invariant");
        require(scenario(block,true)==reference,"planar invariant");
    }
    require(scenario(257,false,0,true)==reference,"preroll retry exact");
    for (unsigned failure=1;failure<=24;++failure)
        require(scenario(257,false,failure)==reference,"fade/render retry exact");
    std::puts("processor: ownership, allocation, seek, options, planar, partitions and failure retries passed");
}
