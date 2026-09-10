// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "effect.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace chronobent_effect {
namespace {
constexpr std::size_t quantum=128, input_capacity=1024;
constexpr uint64_t source_limit=UINT64_C(1)<<48;
using Source=chronobent_instrument::Source;
std::size_t power_of_two(std::size_t n) { std::size_t p=1; while(p<n) p*=2; return p; }
bool valid(const Controls &c) noexcept {
    return c.semitones>=-24 && c.semitones<=24 && c.tempo>=.5 && c.tempo<=2 &&
        (c.formant_scale==0 || (c.formant_scale>=.5 && c.formant_scale<=2)) &&
        c.mix>=0 && c.mix<=1 && c.gain_db>=-60 && c.gain_db<=6 && c.transients<=2 &&
        int(c.mode)>=0 && int(c.mode)<=2;
}
bool equal(const Controls &a,const Controls &b) noexcept {
    return a.semitones==b.semitones && a.tempo==b.tempo && a.formant_scale==b.formant_scale &&
        a.mix==b.mix && a.gain_db==b.gain_db && a.transients==b.transients && a.mode==b.mode;
}
template<class T,std::size_t Size> struct Queue {
    std::array<T,Size> slots{};
    alignas(64) std::atomic<uint64_t> put{0};
    alignas(64) std::atomic<uint64_t> get{0};
    bool push(const T &value) noexcept {
        const auto p=put.load(std::memory_order_relaxed);
        if(p-get.load(std::memory_order_acquire)==Size) return false;
        slots[std::size_t(p)%Size]=value; put.store(p+1,std::memory_order_release); return true;
    }
    bool pop(T &value) noexcept {
        const auto g=get.load(std::memory_order_relaxed);
        if(g==put.load(std::memory_order_acquire)) return false;
        value=slots[std::size_t(g)%Size]; get.store(g+1,std::memory_order_release); return true;
    }
};
struct Input { uint64_t first=0, restore=0,generation=1; std::size_t count=0; Controls controls; std::array<float,quantum*2> samples{}; };
struct Output { uint64_t frame=0,generation=1; float left=0,right=0,mix=1,gain=1; bool valid=true; };
struct Event { uint64_t first,restore; Controls controls; };
}

struct Engine::Impl {
    const double rate;
    const unsigned window,lookahead,latency;
    const std::size_t history_size,output_size;
    Queue<Input,input_capacity> input;
    std::vector<float> history,dry,transition,staging;
    std::vector<Output> output;
    alignas(64) std::atomic<uint64_t> output_put{0};
    alignas(64) std::atomic<uint64_t> output_get{0};
    uint64_t audio_position=0;
    std::atomic<uint64_t> generation{1};
    Controls audio_controls;
    float fallback_mix=1,fallback_gain=1;
    std::atomic<bool> stop{false},failed{false},ready{false},auto_completed{false};
    std::atomic<uint64_t> underruns{0},dropped{0},invalid{0},failures{0},restore_serial{0},published_restore{0};
    std::atomic<int> published_mode{0};
    std::atomic<double> published_capture{0},published_progress{0},published_position{0};
    mutable std::mutex state_mutex;
    std::shared_ptr<const Source> capture,restore_source;
    std::array<float,256> waveform{};
    std::string error;
    std::thread worker;
    // Everything below is worker-owned, including all source/cache reads.
    std::unique_ptr<chronobent_processor,decltype(&chronobent_processor_destroy)> dsp{nullptr,chronobent_processor_destroy};
    std::shared_ptr<const Source> loop_source;
    std::deque<Event> events;
    uint64_t written=0,generated=0,oldest=0,record_start=0,restored=0,worker_generation=1,live_origin=0;
    Mode mode=Mode::live,requested_mode=Mode::live;
    Controls settings,last_enqueued;
    bool has_event=false,in_loop=false;
    uint64_t last_restore=0;
    float smooth_mix=1,smooth_gain=1;
    std::size_t transition_at=0,transition_frames=0;

    static unsigned window_for(double sample_rate) {
        chronobent_config c{};
        if(chronobent_config_for_profile(sample_rate,2,CHRONOBENT_PROFILE_BALANCED,&c)!=CHRONOBENT_OK)
            throw std::invalid_argument("Use a host sample rate from 8 to 192 kHz");
        return c.window_frames;
    }
    explicit Impl(double sample_rate):rate(sample_rate),window(window_for(rate)),
        // Reserve at least 75 ms beyond source lookahead for worker control
        // preroll. Round to a whole conversion quantum at every host rate.
        lookahead(3*window+512),latency(lookahead+std::max(window+1536,
            unsigned(std::ceil(rate*.075/quantum))*unsigned(quantum))),
        history_size(power_of_two(std::size_t(rate*capture_seconds)+latency+8*window)),
        output_size(power_of_two(2*latency+4096)),history(history_size*2),dry(std::size_t(latency)*2),
        transition(std::size_t(rate*.02)*2),staging(transition.size()),output(output_size) {
        worker=std::thread([this]{run();});
    }
    ~Impl() { stop.store(true); worker.join(); }
    chronobent_controls parameters() const noexcept {
        return {in_loop?settings.tempo:1,std::exp2(settings.semitones/12),settings.formant_scale,2,settings.transients,0};
    }
    static int read(void *context,uint64_t first,std::size_t frames,float *out) noexcept {
        auto &self=*static_cast<Impl *>(context);
        if(self.in_loop) {
            if(!self.loop_source || first>self.loop_source->stereo.size()/2 || frames>self.loop_source->stereo.size()/2-first) return 0;
            std::copy_n(self.loop_source->stereo.data()+first*2,frames*2,out); return 1;
        }
        first+=self.live_origin;
        if(first<self.oldest || first>self.written || frames>self.written-first) return 0;
        for(std::size_t i=0;i<frames;++i) {
            const auto slot=std::size_t(first+i)&(self.history_size-1);
            out[i*2]=self.history[slot*2]; out[i*2+1]=self.history[slot*2+1];
        }
        return 1;
    }
    void report(const char *message) noexcept {
        failures.fetch_add(1);
        try { std::lock_guard<std::mutex> lock(state_mutex); error=message; } catch(...) {}
    }
    bool bind(bool looping) {
        in_loop=looping;
        const auto p=parameters();
        const auto length=looping ? loop_source->stereo.size()/2 : source_limit-live_origin;
        auto status=chronobent_processor_set_source_controls(dsp.get(),read,this,length,&p);
        if(status==CHRONOBENT_OK && !looping && generated>live_origin)
            status=chronobent_processor_seek(dsp.get(),generated-live_origin);
        if(status!=CHRONOBENT_OK) { report("Input history unavailable. Resume playback to recover."); return false; }
        return true;
    }
    void publish_capture(std::shared_ptr<const Source> source) {
        loop_source=std::move(source);
        std::array<float,256> shape{};
        if(loop_source) for(std::size_t i=0,n=loop_source->stereo.size()/2;i<n;++i)
            shape[std::min<std::size_t>(255,i*256/n)]=std::max(shape[std::min<std::size_t>(255,i*256/n)],
                std::max(std::abs(loop_source->stereo[2*i]),std::abs(loop_source->stereo[2*i+1])));
        { std::lock_guard<std::mutex> lock(state_mutex); capture=loop_source; waveform=shape; }
        published_capture.store(loop_source?double(loop_source->stereo.size()/2)/rate:0);
    }
    void finish_record(uint64_t end) {
        if(end<record_start+uint64_t(rate*.05) || record_start<oldest) { report("Capture at least 50 ms of audio before looping."); return; }
        end=std::min(end,record_start+uint64_t(rate*capture_seconds));
        auto source=std::make_shared<Source>(); source->sample_rate=rate; source->name="Captured passage";
        source->stereo.resize(std::size_t(end-record_start)*2);
        // Read the immutable logical span before it can be evicted. This worker
        // is the only history writer, so the copy itself is its reader lease.
        const bool prior=in_loop; in_loop=false;
        const bool copied=read(this,record_start-live_origin,std::size_t(end-record_start),source->stereo.data())!=0;
        in_loop=prior;
        if(copied) publish_capture(std::move(source)); else report("Capture history unavailable");
    }
    chronobent_status render_source(float *out,std::size_t count,std::size_t &got) {
        got=0;
        while(got<count) {
            chronobent_processor_state state{}; chronobent_controls controls{};
            chronobent_processor_get_state(dsp.get(),&state);
            chronobent_processor_get_controls(dsp.get(),&controls);
            std::size_t part=0;
            const auto status=chronobent_processor_render(dsp.get(),out+got*2,count-got,&part);
            if(in_loop) for(std::size_t i=0;i<part;++i) {
                const double at=state.source_position+double(i)*controls.tempo;
                const double edge=std::min(at,std::max(0.,double(loop_source->stereo.size()/2)-at))/std::max(1.,rate*.005);
                const float fade=float(std::min(1.,std::max(0.,edge)));
                out[2*(got+i)]*=fade; out[2*(got+i)+1]*=fade;
            }
            got+=part;
            if(status==CHRONOBENT_END && in_loop) {
                const auto reset=chronobent_processor_set_source_controls(dsp.get(),read,this,loop_source->stereo.size()/2,&controls);
                if(reset!=CHRONOBENT_OK) return reset;
            } else return status;
        }
        return CHRONOBENT_OK;
    }
    chronobent_status render_current(float *out,std::size_t count,std::size_t &got) {
        const auto status=render_source(out,count,got);
        for(std::size_t i=0;i<got && transition_at<transition_frames;++i,++transition_at) {
            const float blend=float(transition_at+1)/float(transition_frames);
            for(unsigned c=0;c<2;++c) out[2*i+c]=transition[2*transition_at+c]*(1-blend)+out[2*i+c]*blend;
        }
        return status;
    }
    void stage_transition() {
        // Preserve the outgoing path before rebinding its source. Both arrays
        // were allocated at creation; a rapid switch also retains the remainder
        // of any already audible fade rather than restarting at an old source.
        std::size_t got=0;
        const auto status=render_current(staging.data(),staging.size()/2,got);
        if(status!=CHRONOBENT_OK) report("Source transition could not preserve its outgoing audio");
        transition.swap(staging); transition_at=0; transition_frames=got;
    }
    void apply_event(const Event &event) {
        settings=event.controls;
        bool new_capture=false;
        std::shared_ptr<const Source> pending;
        if(event.restore!=restored) {
            { std::lock_guard<std::mutex> lock(state_mutex);
              if(event.restore==restore_serial.load()) { pending=restore_source; new_capture=true; } }
        }
        if(settings.mode!=requested_mode || new_capture) {
            const bool available=new_capture?bool(pending):bool(loop_source)||(mode==Mode::record && generated-record_start>=uint64_t(rate*.05));
            const bool will_loop=settings.mode==Mode::loop && available;
            const bool change_audio=in_loop!=will_loop || (will_loop && new_capture);
            if(change_audio) stage_transition();
            if(mode==Mode::record && !new_capture) finish_record(generated);
            if(new_capture) publish_capture(std::move(pending));
            requested_mode=settings.mode;
            if(settings.mode==Mode::record) {
                if(in_loop) bind(false);
                mode=Mode::record; record_start=generated;
            } else if(will_loop) {
                mode=Mode::loop; if(change_audio) bind(true);
            } else {
                if(settings.mode==Mode::loop) report("Capture a passage before starting its loop");
                mode=Mode::live; if(in_loop) bind(false);
            }
            auto_completed.store(false);
            published_mode.store(int(mode));
        }
        restored=event.restore; published_restore.store(restored);
    }
    bool ingest() {
        Input packet;
        if(!input.pop(packet)) return false;
        if(packet.generation!=generation.load()) return true;
        if(packet.generation!=worker_generation) {
            worker_generation=packet.generation;
            written=generated=oldest=live_origin=0; events.clear(); has_event=false;
            mode=requested_mode=Mode::live; in_loop=false; smooth_mix=smooth_gain=1;
            transition_frames=transition_at=0; auto_completed.store(false);
            bind(false);
        }
        if(packet.first!=written) {
            // A missed live deadline never re-labels old samples. Start a new
            // available span; queued old outputs retain their original stamps.
            written=generated=oldest=live_origin=packet.first; events.clear(); has_event=false;
            mode=requested_mode=Mode::live; in_loop=false;
            transition_frames=transition_at=0; auto_completed.store(false);
            bind(false);
            report("Input queue overrun. Audio resumed with a new input span.");
        }
        for(std::size_t i=0;i<packet.count;++i) {
            const auto slot=std::size_t(written+i)&(history_size-1);
            history[slot*2]=packet.samples[2*i]; history[slot*2+1]=packet.samples[2*i+1];
        }
        written+=packet.count;
        oldest=std::max(oldest,written>history_size?written-history_size:0);
        if(!has_event || !equal(packet.controls,last_enqueued) || packet.restore!=last_restore) {
            events.push_back({packet.first,packet.restore,packet.controls});
            last_enqueued=packet.controls; last_restore=packet.restore; has_event=true;
        }
        return true;
    }
    void run() noexcept {
        try {
            chronobent_config config{}; chronobent_config_for_profile(rate,2,CHRONOBENT_PROFILE_BALANCED,&config);
            const chronobent_pitch_range range{.25,4}; chronobent_processor *raw=nullptr;
            if(chronobent_processor_create_with_pitch_range(&config,unsigned(rate*.02),&range,&raw)!=CHRONOBENT_OK)
                throw std::runtime_error("Cannot create effect processor");
            dsp.reset(raw); bind(false); ready.store(true);
            std::array<float,quantum*2> wet{};
            while(!stop.load()) {
                bool progress=false;
                // Limit ingestion to a small horizon. Thirty-second capture
                // history and output storage have fixed capacities at creation.
                while(written<generated+lookahead+quantum*4 && ingest()) progress=true;
                auto p=output_put.load(std::memory_order_relaxed);
                const auto free=output_size-std::size_t(p-output_get.load(std::memory_order_acquire));
                if(written>generated+lookahead && free) {
                    while(!events.empty() && events.front().first<=generated) { apply_event(events.front()); events.pop_front(); }
                    auto count=std::min<std::size_t>({quantum,free,std::size_t(written-generated-lookahead)});
                    if(!events.empty()) count=std::min(count,std::size_t(events.front().first-generated));
                    if(mode==Mode::record && generated-record_start>=uint64_t(rate*capture_seconds)) {
                        stage_transition(); finish_record(generated); mode=Mode::loop; bind(true);
                        published_mode.store(int(mode)); auto_completed.store(true);
                    }
                    if(mode==Mode::record) count=std::min(count,std::size_t(record_start+uint64_t(rate*capture_seconds)-generated));
                    const auto controls=parameters();
                    const auto changed=chronobent_processor_set_controls(dsp.get(),&controls);
                    if(changed!=CHRONOBENT_OK && changed!=CHRONOBENT_BUSY) report("Effect control transition could not be prepared");
                    chronobent_processor_state state{};
                    std::size_t got=0;
                    const auto status=render_current(wet.data(),count,got);
                    chronobent_processor_get_state(dsp.get(),&state);
                    const float smoothing=float(1-std::exp(-1/(rate*.02)));
                    const float target_mix=float(settings.mix),target_gain=float(std::pow(10.,settings.gain_db/20));
                    for(std::size_t i=0;i<count;++i) {
                        const bool good=i<got;
                        smooth_mix+=(target_mix-smooth_mix)*smoothing;
                        smooth_gain+=(target_gain-smooth_gain)*smoothing;
                        output[std::size_t(p+i)&(output_size-1)]={generated+i,worker_generation,good?wet[2*i]:0,good?wet[2*i+1]:0,smooth_mix,smooth_gain,good};
                    }
                    generated+=count; output_put.store(p+count,std::memory_order_release);
                    if(status!=CHRONOBENT_OK) { report("Effect stream unavailable. The aligned dry signal is active."); bind(in_loop); }
                    published_position.store(in_loop && loop_source?state.source_position/double(loop_source->stereo.size()/2):0);
                    published_progress.store(mode==Mode::record?std::min(1.,double(generated-record_start)/(rate*capture_seconds)):0);
                    progress=true;
                }
                if(!progress) std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        } catch(const std::exception &e) { report(e.what()); failed.store(true); }
          catch(...) { report("Effect worker stopped"); failed.store(true); }
    }
    bool take(uint64_t frame,Output &sample) noexcept {
        auto g=output_get.load(std::memory_order_relaxed);
        const auto p=output_put.load(std::memory_order_acquire);
        const auto current=generation.load();
        while(g<p && (output[std::size_t(g)&(output_size-1)].generation<current ||
              (output[std::size_t(g)&(output_size-1)].generation==current && output[std::size_t(g)&(output_size-1)].frame<frame))) ++g;
        bool found=false;
        if(g<p && output[std::size_t(g)&(output_size-1)].generation==current && output[std::size_t(g)&(output_size-1)].frame==frame) { sample=output[std::size_t(g)&(output_size-1)]; ++g; found=true; }
        output_get.store(g,std::memory_order_release); return found;
    }
};

Engine::Engine(double rate):impl_(std::make_unique<Impl>(rate)) {}
Engine::~Engine()=default;
unsigned Engine::latency_frames() const noexcept { return impl_->latency; }
void Engine::reset_audio() noexcept {
    auto &s=*impl_; s.generation.fetch_add(1); s.audio_position=0;
    s.output_get.store(s.output_put.load(std::memory_order_acquire),std::memory_order_release);
    std::fill(s.dry.begin(),s.dry.end(),0); s.fallback_mix=s.fallback_gain=1;
}
void Engine::process(const float *left,const float *right,float *out_left,float *out_right,
                     std::size_t frames,const Controls &controls,bool offline,unsigned offline_budget_ms) noexcept {
    auto &s=*impl_;
    if(valid(controls)) s.audio_controls=controls;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(offline_budget_ms);
    for(std::size_t at=0;at<frames;) {
        Input packet; packet.first=s.audio_position; packet.count=std::min(quantum,frames-at);
        packet.controls=s.audio_controls; packet.restore=s.restore_serial.load(); packet.generation=s.generation.load();
        for(std::size_t i=0;i<packet.count;++i) for(unsigned c=0;c<2;++c) {
            float v=(c?right:left)?(c?right:left)[at+i]:0;
            if(!std::isfinite(v) || std::abs(v)>64) { v=0; s.invalid.fetch_add(1); }
            packet.samples[2*i+c]=v;
        }
        bool pushed=s.input.push(packet);
        while(!pushed && offline && !s.failed.load() && std::chrono::steady_clock::now()<deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(250)); pushed=s.input.push(packet);
        }
        if(!pushed) s.dropped.fetch_add(packet.count);
        for(std::size_t i=0;i<packet.count;++i) {
            const auto position=s.audio_position++;
            const auto slot=std::size_t(position%s.latency);
            const float dry_l=s.dry[slot*2],dry_r=s.dry[slot*2+1];
            s.dry[slot*2]=packet.samples[i*2]; s.dry[slot*2+1]=packet.samples[i*2+1];
            Output sample; bool found=false;
            if(position<s.latency) {
                // Release outputs from a retired generation even during the
                // new delay's silent prefix, so a full old queue cannot stall it.
                auto g=s.output_get.load(std::memory_order_relaxed);
                const auto p=s.output_put.load(std::memory_order_acquire);
                while(g<p && s.output[std::size_t(g)&(s.output_size-1)].generation<packet.generation) ++g;
                s.output_get.store(g,std::memory_order_release);
            }
            if(position>=s.latency) {
                found=s.take(position-s.latency,sample);
                while(!found && offline && !s.failed.load() && std::chrono::steady_clock::now()<deadline) {
                    std::this_thread::sleep_for(std::chrono::microseconds(250)); found=s.take(position-s.latency,sample);
                }
                if(!found || !sample.valid) s.underruns.fetch_add(1);
            }
            if(found) { s.fallback_mix=sample.mix; s.fallback_gain=sample.gain; }
            const float mix=found && sample.valid?sample.mix:0;
            out_left[at+i]=(dry_l*(1-mix)+sample.left*mix)*s.fallback_gain;
            out_right[at+i]=(dry_r*(1-mix)+sample.right*mix)*s.fallback_gain;
        }
        at+=packet.count;
    }
}
Snapshot Engine::snapshot() const {
    const auto &s=*impl_; Snapshot value;
    value.mode=Mode(s.published_mode.load()); value.captured_seconds=s.published_capture.load();
    value.capture_progress=s.published_progress.load(); value.position=s.published_position.load();
    value.underruns=s.underruns.load(); value.dropped=s.dropped.load(); value.invalid_samples=s.invalid.load();
    value.failures=s.failures.load(); value.ready=s.ready.load() && !s.failed.load(); value.auto_completed=s.auto_completed.load();
    std::lock_guard<std::mutex> lock(s.state_mutex); value.error=s.error; value.waveform=s.waveform; return value;
}
std::shared_ptr<const Source> Engine::captured_source() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->restore_serial.load()!=impl_->published_restore.load()?impl_->restore_source:impl_->capture;
}
bool Engine::restore_capture(std::shared_ptr<const Source> source) {
    auto &s=*impl_;
    if(source) {
        if(source->sample_rate!=s.rate || source->stereo.size()<std::size_t(s.rate*.05)*2 || source->stereo.size()%2 ||
           source->stereo.size()>std::size_t(s.rate*capture_seconds)*2 || source->name.size()>4096) return false;
        for(float v:source->stereo) if(!std::isfinite(v) || std::abs(v)>64) return false;
    }
    std::lock_guard<std::mutex> lock(s.state_mutex);
    s.restore_source=std::move(source); s.restore_serial.fetch_add(1); return true;
}
}
