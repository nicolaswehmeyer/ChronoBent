// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "ChronoBentPlugin.h"
#include "IPlug_include_in_plug_src.h"
#include "mac_audio.hpp"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
using namespace chronobent_instrument;
using json=nlohmann::json;
namespace {
json encode(const Preparation &p) {
    return {{"tempo",p.tempo},{"pitch",p.semitones},{"formant",p.formant_scale},{"envelope",p.envelope_ms},
        {"transients",p.transients},{"profile",int(p.profile)},{"root",p.root_note}};
}
Preparation decode(const json &j) {
    Preparation p; p.tempo=j.at("tempo"); p.semitones=j.at("pitch"); p.formant_scale=j.at("formant");
    p.envelope_ms=j.at("envelope"); p.transients=j.at("transients");
    p.profile=static_cast<chronobent_profile>(j.at("profile").get<int>()); p.root_note=j.at("root"); return p;
}
}
ChronoBentPlugin::ChronoBentPlugin(const InstanceInfo &info):Plugin(info,MakeConfig(kNumParams,1)) {
    const int prepared=IParam::kFlagCannotAutomate;
    GetParam(kPitch)->InitDouble("Pitch",0,-24,24,.01,"st",prepared);
    GetParam(kTime)->InitDouble("Time",1,.5,2,.001,"×",prepared);
    GetParam(kTimbre)->InitDouble("Timbre",0,-12,12,.01,"st",prepared);
    GetParam(kAttack)->InitDouble("Attack",8,.1,2000,.1,"ms",0,"Envelope",IParam::ShapePowCurve(3));
    GetParam(kRelease)->InitDouble("Release",240,1,5000,1,"ms",0,"Envelope",IParam::ShapePowCurve(3));
    GetParam(kGain)->InitDouble("Output",-9,-60,6,.1,"dB");
    GetParam(kLoop)->InitBool("Loop",false);
    GetParam(kRoot)->InitInt("Root note",60,24,103,"MIDI",prepared);
    GetParam(kProfile)->InitEnum("Detail",1,{"Compact","Balanced","Detailed"},prepared);
    GetParam(kTransients)->InitEnum("Transients",2,{"Smooth","Crisp","Mixed"},prepared);
    GetParam(kFormants)->InitBool("Keep formants",false,"",prepared);
    MakeDefaultPreset("Glass Circuit",1);
    mEngine=std::make_unique<Engine>(mRate);
    SetMaxJSStringLength(32768);
    mEditorInitFunc=[this] { LoadIndexHtml(__FILE__,GetBundleID()); EnableScroll(false); };
    mWorker=std::thread([this]{work();}); request(0);
}
ChronoBentPlugin::~ChronoBentPlugin() {
    { std::lock_guard<std::mutex> lock(mControl); mStop=true; mWake.notify_one(); }
    mWorker.join();
}
Preparation ChronoBentPlugin::preparation() const {
    Preparation p; p.semitones=GetParam(kPitch)->Value(); p.tempo=GetParam(kTime)->Value();
    p.formant_scale=GetParam(kFormants)->Bool() ? std::exp2(GetParam(kTimbre)->Value()/12) : 0;
    p.root_note=GetParam(kRoot)->Int(); p.profile=static_cast<chronobent_profile>(GetParam(kProfile)->Int());
    p.transients=GetParam(kTransients)->Int(); return p;
}
void ChronoBentPlugin::request(int preset,const std::string &path) {
    std::lock_guard<std::mutex> lock(mControl);
    mPreset=preset; mPath=path; mPending=preparation(); mQueued.store(++mRequest); mWake.notify_one();
}
void ChronoBentPlugin::work() {
    uint64_t consumed=0;
    for(;;) {
        uint64_t revision; double rate; int preset; std::string path;
        Preparation parameters; std::shared_ptr<const Source> previous;
        {
            std::unique_lock<std::mutex> lock(mControl);
            mWake.wait(lock,[&]{return mStop || consumed!=mRequest;});
            if(mStop) return;
            consumed=revision=mRequest; rate=mRate; preset=mPreset; path=mPath; parameters=mPending; previous=mSource;
        }
        try {
            auto source=!path.empty() ? load_audio_file(path,rate) : preset>=0 ? factory_source(unsigned(preset),rate) :
                previous ? (previous->sample_rate==rate ? previous : resample_source(*previous,rate)) : factory_source(0,rate);
            std::lock_guard<std::mutex> lock(mControl);
            if(mStop) return;
            if(revision!=mRequest) continue;
            if(!mEngine->prepare(source,parameters)) throw std::runtime_error("Cannot prepare these sample settings");
            mSource=std::move(source); mApplied=parameters; mError.clear();
            mSubmitted.store(revision);
        } catch(const std::exception &e) {
            std::lock_guard<std::mutex> lock(mControl); if(revision==mRequest) { mError=e.what(); mSubmitted.store(revision); }
        }
    }
}
void ChronoBentPlugin::OnReset() {
    const double rate=GetSampleRate();
    std::lock_guard<std::mutex> lock(mControl);
    mMidiCount=0;
    mRateSupported.store(rate>=8000 && rate<=192000);
    if(rate>=8000 && rate<=192000 && rate!=mRate) {
        mRate=rate; mEngine=std::make_unique<Engine>(rate); mPreset=-1; mPath.clear();
        mPending=mApplied; mQueued.store(++mRequest); mWake.notify_one();
    } else mEngine->reset();
}
void ChronoBentPlugin::ProcessMidiMsg(const IMidiMsg &message) {
    if(mMidiCount==mMidi.size()) { mMidiOverflow.fetch_add(1); return; }
    std::size_t i=mMidiCount++;
    while(i && mMidi[i-1].mOffset>message.mOffset) { mMidi[i]=mMidi[i-1]; --i; }
    mMidi[i]=message;
}
void ChronoBentPlugin::midi(const IMidiMsg &m) noexcept {
    switch(m.StatusMsg()) {
        case IMidiMsg::kNoteOn: if(m.Velocity()) mEngine->note_on(m.NoteNumber(),double(m.Velocity())/127); else mEngine->note_off(m.NoteNumber()); break;
        case IMidiMsg::kNoteOff: mEngine->note_off(m.NoteNumber()); break;
        case IMidiMsg::kControlChange:
            if(m.mData1==64) mEngine->sustain(m.mData2>=64);
            else if(m.mData1==120) mEngine->reset();
            else if(m.mData1==123) mEngine->all_notes_off();
            break;
        default: break;
    }
}
void ChronoBentPlugin::ProcessBlock(sample **,sample **outputs,int frames) {
    if(!mRateSupported.load()) {
        std::fill_n(outputs[0],frames,0); std::fill_n(outputs[1],frames,0); mMidiCount=0; return;
    }
    Performance p; p.attack_ms=GetParam(kAttack)->Value(); p.release_ms=GetParam(kRelease)->Value();
    p.gain_db=GetParam(kGain)->Value(); p.loop=GetParam(kLoop)->Bool();
    const bool offline=GetRenderingOffline();
    if(offline && mMidiCount) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(mSubmitted.load()!=mQueued.load() && std::chrono::steady_clock::now()<end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto remaining=std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count());
        if(mSubmitted.load()!=mQueued.load() || !mEngine->wait_ready(unsigned(remaining))) {
            std::fill_n(outputs[0],frames,0); std::fill_n(outputs[1],frames,0);
            mMidiCount=0; mOfflineTimeout.store(true); return;
        }
        mOfflineTimeout.store(false);
    }
    std::array<float,256> left{},right{};
    std::size_t event=0; int at=0; float peak=0;
    while(at<frames) {
        while(event<mMidiCount && mMidi[event].mOffset<=at) midi(mMidi[event++]);
        const int next=event<mMidiCount ? std::min(frames,std::max(at+1,mMidi[event].mOffset)) : frames;
        const int count=std::min(256,next-at);
        mEngine->render(left.data(),right.data(),std::size_t(count),p,offline);
        for(int i=0;i<count;++i) {
            outputs[0][at+i]=left[i]; outputs[1][at+i]=right[i];
            peak=std::max(peak,std::max(std::abs(left[i]),std::abs(right[i])));
        }
        at+=count;
    }
    std::size_t keep=0;
    while(event<mMidiCount) { mMidi[event].mOffset-=frames; mMidi[keep++]=mMidi[event++]; }
    mMidiCount=keep; mPeak.store(peak);
}
void ChronoBentPlugin::OnIdle() {
    std::unique_lock<std::mutex> lock(mControl,std::try_to_lock); if(!lock.owns_lock()) return;
    const auto s=mEngine->snapshot();
    const json state={{"ready",s.ready},{"preparing",s.preparing || mSubmitted.load()!=mQueued.load()},{"progress",s.progress},{"name",s.name},
        {"duration",s.duration},{"rate",s.sample_rate},{"voices",s.active_voices},{"notes",s.active_notes},{"peak",mPeak.load()},
        {"error",!mRateSupported.load()?"Use a host sample rate from 8 to 192 kHz":mOfflineTimeout.load()?"Preparation timed out. Wait until ready, then bounce again.":mError.empty()?s.error:mError},{"underruns",s.underruns},{"dropped",s.dropped_notes+mMidiOverflow.load()},
        {"waveform",s.waveform},{"applied",encode(s.preparation)},{"revision",s.revision}};
    const auto text=state.dump(); SendArbitraryMsgFromDelegate(100,int(text.size()),text.data());
}
bool ChronoBentPlugin::OnMessage(int tag,int control,int size,const void *data) {
    (void)control; (void)size; (void)data;
    if(tag==1) { request(-1); return true; }
    if(tag==2) { const auto path=choose_audio_file(); if(!path.empty()) request(-1,path); return true; }
    if(tag>=10 && tag<=12) { request(tag-10); return true; }
    return false;
}
bool ChronoBentPlugin::CanNavigateToURL(const char *url) {
    return url && (std::strncmp(url,"file://",7)==0 || std::strcmp(url,"about:blank")==0);
}
bool ChronoBentPlugin::SerializeState(IByteChunk &chunk) const {
    std::lock_guard<std::mutex> lock(mControl);
    try {
        std::array<double,kNumParams> values{};
        for(int i=0;i<kNumParams;++i) values[i]=GetParam(i)->Value();
        const json state={{"version",1},{"params",values},{"applied",encode(mApplied)},
            {"rate",mSource?mSource->sample_rate:mRate},{"name",mSource?mSource->name:"Glass Circuit"},
            {"frames",mSource?mSource->stereo.size()/2:0}};
        const auto text=state.dump(); const uint32_t magic=0x354e4243,length=uint32_t(text.size());
        chunk.Put(&magic); chunk.Put(&length); chunk.PutBytes(text.data(),int(text.size()));
        if(mSource) chunk.PutBytes(mSource->stereo.data(),int(mSource->stereo.size()*sizeof(float)));
        return true;
    } catch(...) { return false; }
}
int ChronoBentPlugin::UnserializeState(const IByteChunk &chunk,int position) {
    try {
        uint32_t magic=0,length=0;
        position=chunk.Get(&magic,position); if(position<0 || magic!=0x354e4243) return -1;
        position=chunk.Get(&length,position); if(position<0 || length>65536 || length>uint32_t(chunk.Size()-position)) return -1;
        std::string text(length,'\0'); position=chunk.GetBytes(text.data(),int(length),position); if(position<0) return -1;
        const auto state=json::parse(text);
        if(state.at("version")!=1 || state.at("params").size()!=kNumParams) return -1;
        const auto values=state.at("params").get<std::array<double,kNumParams>>();
        for(int i=0;i<kNumParams;++i) if(!std::isfinite(values[i]) || values[i]<GetParam(i)->GetMin() || values[i]>GetParam(i)->GetMax()) return -1;
        const auto parameters=decode(state.at("applied"));
        const double rate=state.at("rate"); const uint64_t frames=state.at("frames");
        if(!(rate>=8000 && rate<=192000) || frames>uint64_t(rate*120) || frames>uint64_t(chunk.Size()-position)/8) return -1;
#ifdef VST3_API
        constexpr int trailer=4;
#else
        constexpr int trailer=0;
#endif
        if(uint64_t(chunk.Size()-position)!=frames*8+trailer) return -1;
        auto source=std::make_shared<Source>(); source->sample_rate=rate; source->name=state.at("name").get<std::string>();
        if(source->name.size()>4096) return -1;
        source->stereo.resize(std::size_t(frames)*2);
        if(frames) position=chunk.GetBytes(source->stereo.data(),int(frames*8),position);
        if(position<0) return -1;
        for(float v:source->stereo) if(!std::isfinite(v) || std::abs(v)>64) return -1;
        if(!(parameters.tempo>=.5 && parameters.tempo<=2) || !(parameters.semitones>=-24 && parameters.semitones<=24) ||
           !(parameters.formant_scale==0 || (parameters.formant_scale>=.5 && parameters.formant_scale<=2)) ||
           !(parameters.envelope_ms>=1 && parameters.envelope_ms<=4) || parameters.transients>2 ||
           int(parameters.profile)<0 || int(parameters.profile)>2 || parameters.root_note<24 || parameters.root_note>103) return -1;
        std::lock_guard<std::mutex> lock(mControl);
        for(int i=0;i<kNumParams;++i) GetParam(i)->Set(values[i]);
        mSource=frames?source:factory_source(0,mRate); mApplied=mPending=parameters;
        mPreset=-1; mPath.clear(); mQueued.store(++mRequest); mWake.notify_one(); return position;
    } catch(...) { return -1; }
}
#ifdef VST3_API
Steinberg::tresult PLUGIN_API ChronoBentPlugin::setState(Steinberg::IBStream *stream) {
    if(!stream) return Steinberg::kResultFalse;
    try {
        std::vector<uint8_t> data; std::array<uint8_t,4096> block{};
        for(;;) {
            Steinberg::int32 got=0;
            const auto status=stream->read(block.data(),int(block.size()),&got);
            if(got<0 || got>int(block.size()) || data.size()+std::size_t(got)>185000000) return Steinberg::kResultFalse;
            data.insert(data.end(),block.begin(),block.begin()+got);
            if(!got || status!=Steinberg::kResultOk) break;
        }
        if(data.size()<12) return Steinberg::kResultFalse;
        int32_t bypass=0; std::memcpy(&bypass,data.data()+data.size()-4,4);
        if(bypass!=0 && bypass!=1) return Steinberg::kResultFalse;
        IByteChunk chunk; chunk.PutBytes(data.data(),int(data.size()));
        if(UnserializeState(chunk,0)!=int(data.size())-4) return Steinberg::kResultFalse;
        UpdateParams(this,bypass); OnRestoreState(); return Steinberg::kResultOk;
    } catch(...) { return Steinberg::kResultFalse; }
}
#endif
