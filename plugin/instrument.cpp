// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "instrument.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace chronobent_instrument {
namespace {
constexpr std::size_t queue_frames = 65536, request_count = 64, block_frames = 256;
constexpr double pi = 3.14159265358979323846;
using DSP = std::unique_ptr<chronobent,decltype(&chronobent_destroy)>;
struct Bank {
    std::atomic<unsigned> readers{0};
    std::shared_ptr<const Source> source;
    Preparation preparation;
    uint64_t revision = 0, duration = 0;
    std::size_t prefix_frames = 0;
    std::vector<float> attacks;
    std::array<float,256> waveform{};
};
struct Command { uint64_t serial=0; int bank=-1, key=0; };
struct Worker {
    std::array<Command,request_count> commands{};
    std::atomic<uint64_t> put{0}, take{0}, wanted{0}, generation{0}, written{0}, read{0}, failed{0};
    std::vector<float> queue = std::vector<float>(queue_frames*2);
    std::thread thread;
    bool has_room() const noexcept {
        return put.load(std::memory_order_relaxed)-take.load(std::memory_order_acquire)<request_count;
    }
    bool push(Command command) noexcept {
        const auto p=put.load(std::memory_order_relaxed);
        if(p-take.load(std::memory_order_acquire)==request_count) return false;
        commands[p&(request_count-1)]=command;
        put.store(p+1,std::memory_order_release); return true;
    }
    bool pop(Command &command) noexcept {
        const auto t=take.load(std::memory_order_relaxed);
        if(t==put.load(std::memory_order_acquire)) return false;
        command=commands[t&(request_count-1)];
        take.store(t+1,std::memory_order_release); return true;
    }
};
struct Voice {
    int bank=-1, key=0, note=-1;
    uint64_t serial=0, position=0, released=0;
    double velocity=0, level=0, release_level=0;
    bool key_up=false, releasing=false;
    std::array<float,2> last{}, tail{};
    unsigned tail_left=0;
};
bool valid(const Source &s,const Preparation &p) {
    if(!(s.sample_rate>=8000 && s.sample_rate<=192000) || s.stereo.empty() || s.stereo.size()%2 ||
       s.stereo.size()/2>std::size_t(s.sample_rate*120) || !(p.tempo>=.5 && p.tempo<=2) ||
       !(p.semitones>=-24 && p.semitones<=24) ||
       !(p.formant_scale==0 || (p.formant_scale>=.5 && p.formant_scale<=2)) ||
       !(p.envelope_ms>=1 && p.envelope_ms<=4) || p.transients>2 ||
       p.profile<CHRONOBENT_PROFILE_COMPACT || p.profile>CHRONOBENT_PROFILE_DETAILED ||
       p.root_note<24 || p.root_note>103) return false;
    for(float v:s.stereo) if(!std::isfinite(v) || std::abs(v)>64) return false;
    return true;
}
int read_source(void *context,uint64_t first,std::size_t frames,float *out) {
    const auto &s=*static_cast<const Source *>(context);
    if(first>s.stereo.size()/2 || frames>s.stereo.size()/2-first) return 0;
    std::copy_n(s.stereo.data()+first*2,frames*2,out); return 1;
}
DSP make_dsp(const Bank &bank) {
    chronobent_config config{};
    if(chronobent_config_for_profile(bank.source->sample_rate,2,bank.preparation.profile,&config)!=CHRONOBENT_OK)
        throw std::runtime_error("Invalid analysis profile");
    const chronobent_pitch_range range{.0625,16}; chronobent *raw=nullptr;
    if(chronobent_create_with_pitch_range(&config,&range,&raw)!=CHRONOBENT_OK) throw std::runtime_error("Cannot reserve voice memory");
    return DSP(raw,chronobent_destroy);
}
chronobent_status reset_dsp(chronobent *dsp,const Bank &bank,int key) {
    const auto &p=bank.preparation;
    const chronobent_controls controls{p.tempo,std::exp2((p.semitones+key+Engine::lowest_key)/12),
        p.formant_scale,p.envelope_ms,p.transients,0};
    return chronobent_reset_controls(dsp,bank.source->stereo.size()/2,&controls);
}
}
struct Engine::Impl {
    std::array<Bank,2> banks;
    std::array<Worker,voice_count> workers;
    std::array<Voice,voice_count> voices;
    std::atomic<int> published{-1};
    std::atomic<unsigned> acquisitions{0}, active{0};
    std::atomic<bool> stopping{false}, preparing{false};
    std::atomic<double> progress{0};
    std::atomic<uint64_t> requested{0}, completed{0}, underruns{0}, dropped{0};
    std::atomic<uint32_t> active_notes{0};
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<const Source> pending;
    Preparation pending_parameters;
    std::string error;
    std::thread preparer;
    uint64_t next_serial=0, seen_revision=0;
    bool sustain_down=false;
    double rate;
    double smoothed_gain=std::pow(10.,-9./20.);

    explicit Impl(double sample_rate):rate(sample_rate) {
        if(!(rate>=8000 && rate<=192000)) throw std::invalid_argument("Invalid instrument sample rate");
        try {
            preparer=std::thread([this]{prepare_loop();});
            for(unsigned i=0;i<voice_count;++i) workers[i].thread=std::thread([this,i]{voice_loop(i);});
        } catch(...) {
            stopping.store(true); condition.notify_all();
            if(preparer.joinable()) preparer.join();
            for(auto &w:workers) if(w.thread.joinable()) w.thread.join();
            throw;
        }
    }
    ~Impl() {
        stopping.store(true); condition.notify_all();
        preparer.join(); for(auto &w:workers) w.thread.join();
    }
    int acquire() const noexcept {
        auto &self=*const_cast<Impl *>(this);
        self.acquisitions.fetch_add(1);
        const int slot=published.load();
        if(slot>=0) self.banks[slot].readers.fetch_add(1);
        self.acquisitions.fetch_sub(1); return slot;
    }
    void release(int slot) noexcept { if(slot>=0) banks[slot].readers.fetch_sub(1); }
    void fail(std::string message) {
        std::lock_guard<std::mutex> guard(mutex); error=std::move(message);
    }
    void prepare_loop() noexcept {
        uint64_t consumed=0;
        while(!stopping.load()) {
            uint64_t revision; std::shared_ptr<const Source> source; Preparation parameters;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock,[&]{return stopping.load() || requested.load()!=consumed;});
                if(stopping.load()) break;
                revision=requested.load(); source=pending; parameters=pending_parameters; consumed=revision;
                error.clear();
            }
            preparing.store(true); progress.store(0);
            const int slot=published.load()==0 ? 1 : 0;
            while(!stopping.load() && requested.load()==revision &&
                  (banks[slot].readers.load()!=0 || acquisitions.load()!=0))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(stopping.load()) break;
            if(requested.load()!=revision) continue;
            try {
                Bank &bank=banks[slot]; bank.source=std::move(source); bank.preparation=parameters;
                bank.revision=revision;
                bank.duration=uint64_t(std::ceil(double(bank.source->stereo.size()/2)/parameters.tempo));
                bank.prefix_frames=std::size_t(std::min<uint64_t>(bank.duration,uint64_t(std::ceil(rate*.5))));
                bank.attacks.assign(bank.prefix_frames*2*key_count,0);
                bank.waveform.fill(0);
                const auto frames=bank.source->stereo.size()/2;
                for(std::size_t i=0;i<frames;++i) {
                    const auto bin=std::min(std::size_t(255),i*256/frames);
                    bank.waveform[bin]=std::max(bank.waveform[bin],std::max(std::abs(bank.source->stereo[2*i]),std::abs(bank.source->stereo[2*i+1])));
                }
                auto dsp=make_dsp(bank);
                bool cancelled=false;
                // Prepare the center key first, then expand chromatically.
                for(int stage=0;stage<key_count;++stage) {
                    const int key=stage ? 24+(stage%2 ? -(stage+1)/2 : stage/2) : 24;
                    if(reset_dsp(dsp.get(),bank,key)!=CHRONOBENT_OK) throw std::runtime_error("Cannot prepare pitch");
                    std::size_t written=0;
                    while(written<bank.prefix_frames) {
                        if(stopping.load() || requested.load()!=revision) { cancelled=true; break; }
                        std::size_t got=0;
                        const auto status=chronobent_render(dsp.get(),read_source,const_cast<Source *>(bank.source.get()),
                            bank.attacks.data()+(key*bank.prefix_frames+written)*2,std::min(block_frames,bank.prefix_frames-written),&got);
                        if(status!=CHRONOBENT_OK && status!=CHRONOBENT_END) throw std::runtime_error("Sample preparation failed");
                        if(!got) throw std::runtime_error("Empty prepared attack");
                        written+=got;
                    }
                    if(cancelled) break;
                    progress.store(double(stage+1)/key_count);
                }
                if(cancelled) continue;
                published.store(slot); completed.store(revision); preparing.store(false);
            } catch(const std::exception &e) { fail(e.what()); preparing.store(false); }
              catch(...) { fail("Sample preparation failed"); preparing.store(false); }
        }
    }
    void voice_loop(unsigned index) noexcept {
        auto &w=workers[index]; Command current; DSP dsp(nullptr,chronobent_destroy);
        uint64_t cursor=0; std::array<float,block_frames*2> block{};
        while(!stopping.load()) {
            Command command; bool replaced=false;
            while(w.pop(command)) { release(current.bank); current=command; replaced=true; }
            if(current.bank>=0 && w.wanted.load()!=current.serial) { release(current.bank); current.bank=-1; }
            if(current.bank<0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            Bank &bank=banks[current.bank];
            if(replaced) {
                try {
                    dsp=make_dsp(bank);
                    if(reset_dsp(dsp.get(),bank,current.key)!=CHRONOBENT_OK) throw std::runtime_error("Cannot start voice");
                    cursor=0; w.written.store(bank.prefix_frames); w.generation.store(current.serial);
                } catch(...) { w.failed.store(current.serial); fail("Voice preparation failed"); release(current.bank); current.bank=-1; continue; }
            }
            if(w.wanted.load()!=current.serial) continue;
            const auto read=w.read.load(std::memory_order_acquire);
            if(cursor>=read && cursor-read>=queue_frames-block_frames) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            std::size_t got=0;
            const auto status=chronobent_render(dsp.get(),read_source,const_cast<Source *>(bank.source.get()),block.data(),block_frames,&got);
            if(status!=CHRONOBENT_OK && status!=CHRONOBENT_END) {
                w.failed.store(current.serial); fail("Voice source read failed"); release(current.bank); current.bank=-1; continue;
            }
            if(w.wanted.load()!=current.serial) continue;
            const auto first=std::max<uint64_t>(bank.prefix_frames,w.read.load(std::memory_order_acquire));
            for(std::size_t i=0;i<got;++i) if(cursor+i>=first) {
                const auto at=std::size_t(cursor+i)&(queue_frames-1);
                w.queue[2*at]=block[2*i]; w.queue[2*at+1]=block[2*i+1];
            }
            cursor+=got;
            if(cursor>=bank.prefix_frames) w.written.store(cursor,std::memory_order_release);
            if(status==CHRONOBENT_END && reset_dsp(dsp.get(),bank,current.key)!=CHRONOBENT_OK) {
                w.failed.store(current.serial); fail("Cannot restart voice"); release(current.bank); current.bank=-1;
            }
        }
        release(current.bank);
        Command command; while(w.pop(command)) release(command.bank);
    }
    void stop_voice(unsigned index) noexcept {
        auto &v=voices[index];
        if(v.bank>=0) {
            v.tail=v.last; v.tail_left=64;
            release(v.bank); v.bank=-1; workers[index].wanted.store(++next_serial);
        }
    }
};
Engine::Engine(double rate):impl_(std::make_unique<Impl>(rate)) {}
Engine::~Engine()=default;
bool Engine::prepare(std::shared_ptr<const Source> source,Preparation parameters) {
    if(!source || source->sample_rate!=impl_->rate || !valid(*source,parameters)) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pending=std::move(source); impl_->pending_parameters=parameters;
    impl_->requested.fetch_add(1); impl_->condition.notify_one(); return true;
}
Snapshot Engine::snapshot() const {
    const auto &p=*impl_; Snapshot s;
    s.preparing=p.preparing.load(); s.progress=p.progress.load(); s.underruns=p.underruns.load();
    s.dropped_notes=p.dropped.load(); s.active_voices=p.active.load();
    const auto notes=p.active_notes.load();
    for(unsigned i=0;i<voice_count;++i) s.active_notes[i]=int((notes>>(i*8))&255)-1;
    const int slot=p.acquire();
    if(slot>=0) {
        const auto &bank=p.banks[slot]; s.ready=true; s.revision=bank.revision;
        s.sample_rate=bank.source->sample_rate; s.duration=double(bank.source->stereo.size()/2)/s.sample_rate;
        s.name=bank.source->name; s.preparation=bank.preparation; s.waveform=bank.waveform;
        impl_->release(slot);
    }
    { std::lock_guard<std::mutex> lock(p.mutex); s.error=p.error; }
    return s;
}
bool Engine::wait_ready(unsigned milliseconds) const {
    const auto revision=impl_->requested.load();
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(impl_->completed.load()!=revision) {
        if(!revision || impl_->stopping.load() || std::chrono::steady_clock::now()>=end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
bool Engine::note_on(int note,double velocity) noexcept {
    if(note<0 || note>127 || !(velocity>0 && velocity<=1)) return false;
    auto &p=*impl_; const int slot=p.acquire(); if(slot<0) return false;
    const auto &bank=p.banks[slot]; const int key=note-bank.preparation.root_note-lowest_key;
    if(key<0 || key>=key_count) { p.release(slot); return false; }
    if(p.seen_revision!=bank.revision) { reset(); p.seen_revision=bank.revision; }
    unsigned index=0;
    for(unsigned i=0;i<voice_count;++i) {
        if(p.voices[i].bank<0) { index=i; break; }
        if(p.voices[i].serial<p.voices[index].serial) index=i;
    }
    auto &w=p.workers[index];
    if(!w.has_room()) { p.release(slot); p.dropped.fetch_add(1); return false; }
    p.stop_voice(index);
    const auto serial=++p.next_serial;
    auto &v=p.voices[index]; auto tail=v.tail; auto tail_left=v.tail_left;
    v=Voice{}; v.tail=tail; v.tail_left=tail_left; v.bank=slot; v.key=key; v.note=note; v.serial=serial; v.velocity=velocity;
    w.read.store(bank.prefix_frames); w.wanted.store(serial);
    p.banks[slot].readers.fetch_add(1); // Publish the command after its desired generation.
    w.push({serial,slot,key}); return true; // This is the only producer; room cannot shrink.
}
void Engine::note_off(int note) noexcept {
    for(auto &v:impl_->voices) if(v.bank>=0 && v.note==note) {
        v.key_up=true;
        if(!impl_->sustain_down && !v.releasing) { v.releasing=true; v.released=v.position; v.release_level=v.level; }
    }
}
void Engine::sustain(bool held) noexcept {
    impl_->sustain_down=held;
    if(!held) for(auto &v:impl_->voices) if(v.bank>=0 && v.key_up && !v.releasing) {
        v.releasing=true; v.released=v.position; v.release_level=v.level;
    }
}
void Engine::all_notes_off() noexcept { for(auto &v:impl_->voices) if(v.bank>=0) note_off(v.note); }
void Engine::reset() noexcept {
    auto &p=*impl_; p.sustain_down=false;
    for(unsigned i=0;i<voice_count;++i) { p.stop_voice(i); p.voices[i].tail_left=0; }
    p.active.store(0);
    p.active_notes.store(0);
}
void Engine::render(float *left,float *right,std::size_t frames,const Performance &controls,bool offline) noexcept {
    if(!left || !right) return;
    auto &p=*impl_;
    const int published=p.acquire();
    if(published>=0) {
        const auto revision=p.banks[published].revision;
        if(p.seen_revision!=revision) { reset(); p.seen_revision=revision; }
        p.release(published);
    }
    const double attack=std::clamp(std::isfinite(controls.attack_ms)?controls.attack_ms:8.,.1,2000.)*.001*p.rate;
    const double release=std::clamp(std::isfinite(controls.release_ms)?controls.release_ms:240.,1.,5000.)*.001*p.rate;
    const double gain=std::pow(10.,std::clamp(std::isfinite(controls.gain_db)?controls.gain_db:-9.,-60.,6.)/20.);
    const double smoothing=1-std::exp(-1/(.02*p.rate));
    const auto deadline=offline ? std::chrono::steady_clock::now()+std::chrono::seconds(10) : std::chrono::steady_clock::time_point{};
    std::fill_n(left,frames,0); std::fill_n(right,frames,0);
    for(std::size_t frame=0;frame<frames;++frame) {
      p.smoothed_gain+=(gain-p.smoothed_gain)*smoothing;
      for(unsigned index=0;index<voice_count;++index) {
        auto &v=p.voices[index]; auto &w=p.workers[index];
        if(v.tail_left) {
            const float g=float(v.tail_left--)/64;
            left[frame]+=v.tail[0]*g; right[frame]+=v.tail[1]*g;
        }
        if(v.bank<0) continue;
        const auto &bank=p.banks[v.bank];
        if((!controls.loop && v.position>=bank.duration) || (v.releasing && double(v.position-v.released)>=release)) {
            p.stop_voice(index); continue;
        }
        std::array<float,2> sample{};
        if(v.position<bank.prefix_frames) {
            const auto at=(std::size_t(v.key)*bank.prefix_frames+std::size_t(v.position))*2;
            sample={bank.attacks[at],bank.attacks[at+1]};
        } else {
            const auto available=[&]{return w.generation.load()==v.serial && w.written.load(std::memory_order_acquire)>v.position;};
            if(offline) {
                while(!available() && w.failed.load()!=v.serial && !p.stopping.load() && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if(available()) {
                const auto at=std::size_t(v.position)&(queue_frames-1);
                sample={w.queue[2*at],w.queue[2*at+1]};
            } else p.underruns.fetch_add(1,std::memory_order_relaxed);
        }
        v.level=v.releasing ? v.release_level*std::max(0.,1-double(v.position-v.released)/release) : std::min(1.,double(v.position+1)/attack);
        double loop_gain=1;
        if(controls.loop) {
            const auto at=v.position%bank.duration;
            const auto edge=std::min<uint64_t>(128,std::max<uint64_t>(1,bank.duration/2));
            loop_gain=std::min(1.,std::min(double(at+1)/edge,double(bank.duration-at)/edge));
        }
        const float g=float(p.smoothed_gain*v.velocity*v.level*loop_gain);
        v.last={sample[0]*g,sample[1]*g}; left[frame]+=v.last[0]; right[frame]+=v.last[1];
        ++v.position;
        w.read.store(std::max<uint64_t>(bank.prefix_frames,v.position),std::memory_order_release);
      }
    }
    unsigned count=0; uint32_t notes=0;
    for(unsigned i=0;i<voice_count;++i) if(p.voices[i].bank>=0) {
        ++count; notes|=uint32_t(p.voices[i].note+1)<<(i*8);
    }
    p.active.store(count);
    p.active_notes.store(notes);
}
std::shared_ptr<const Source> factory_source(unsigned preset,double rate) {
    if(!(rate>=8000 && rate<=192000)) return {};
    auto s=std::make_shared<Source>(); s->sample_rate=rate;
    s->name=preset%3==0 ? "Glass Circuit" : preset%3==1 ? "Soft Current" : "Copper Bloom";
    const auto frames=std::size_t(rate*1.5); s->stereo.resize(frames*2);
    uint32_t noise=0x43524f4e;
    for(std::size_t i=0;i<frames;++i) {
        const double t=double(i)/rate; noise=noise*1664525u+1013904223u;
        const double fade=std::min(1.,t/.008)*std::min(1.,(1.5-t)/.04);
        const double fundamental=261.6255653005986;
        double value=0;
        for(unsigned h=1;h<=12;++h) {
            const double envelope=std::exp(-t*(preset%3==0 ? 1.2+h*.22 : preset%3==1 ? .35+h*.04 : .9+h*.1));
            value+=std::sin(2*pi*fundamental*h*t+(h%3)*.23)*envelope/(h*(preset%3==1 ? 2.5 : 4.));
        }
        if(preset%3==2) value+=(double(noise)/4294967296.-.5)*.07*std::exp(-12*t);
        s->stereo[2*i]=float(value*fade*.5); s->stereo[2*i+1]=float(value*fade*.5);
    }
    return s;
}
}
