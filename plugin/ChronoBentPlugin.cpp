// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "ChronoBentPlugin.h"
#include "IPlug_include_in_plug_src.h"
#include "mac_audio.hpp"
#include "json.hpp"
#include "tuning_state.hpp"
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
bool prepared_parameter(int index) {
    return index==kPitch || index==kTime || index==kTimbre || index==kRoot || index==kProfile || index==kTransients || index==kFormants;
}
bool same(const Preparation &a,const Preparation &b) {
    return a.tempo==b.tempo && a.semitones==b.semitones && a.formant_scale==b.formant_scale && a.envelope_ms==b.envelope_ms &&
        a.transients==b.transients && a.profile==b.profile && a.root_note==b.root_note;
}
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr int64_t settle_ns=150000000;
Preparation decode(const json &j) {
    Preparation p; p.tempo=j.at("tempo"); p.semitones=j.at("pitch"); p.formant_scale=j.at("formant");
    p.envelope_ms=j.at("envelope"); p.transients=j.at("transients");
    p.profile=static_cast<chronobent_profile>(j.at("profile").get<int>()); p.root_note=j.at("root"); return p;
}
}
ChronoBentPlugin::ChronoBentPlugin(const InstanceInfo &info):ChronoBentHost(info,MakeConfig(kNumParams,1)) {
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
    mTuningEditor.reset();
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
    std::lock_guard<std::mutex> lock(mControl); queue(preset,path);
}
void ChronoBentPlugin::queue(int preset,const std::string &path) {
    mPreset=preset; mPath=path; mTunePending.reset();mPending=preparation(); mQueued.store(++mRequest); mWake.notify_one();
}
void ChronoBentPlugin::OnParamChange(int index,EParamSource source,int offset) {
    ChronoBentHost::OnParamChange(index,source,offset);
    if(prepared_parameter(index)) mPreparationChanged.store(now_ns());
}
void ChronoBentPlugin::BeginInformHostOfParamChangeFromUI(int index) {
    if(prepared_parameter(index)) mGestures.fetch_add(1);
    ChronoBentHost::BeginInformHostOfParamChangeFromUI(index);
}
void ChronoBentPlugin::EndInformHostOfParamChangeFromUI(int index) {
    ChronoBentHost::EndInformHostOfParamChangeFromUI(index);
    if(prepared_parameter(index) && mGestures.load()>0) { mGestures.fetch_sub(1); mPreparationChanged.store(now_ns()); }
}
void ChronoBentPlugin::work() {
    uint64_t consumed=0;
    for(;;) {
        uint64_t revision; double rate; int preset; std::string path;
        Preparation parameters; std::shared_ptr<const Source> previous;
        std::shared_ptr<const chronobent_host::TuneResult> tuning;bool corrected=false;
        {
            std::unique_lock<std::mutex> lock(mControl);
            mWake.wait(lock,[&]{return mStop || consumed!=mRequest;});
            if(mStop) return;
            consumed=revision=mRequest; rate=mRate; preset=mPreset; path=mPath; parameters=mPending; previous=mSource;tuning=mTunePending;corrected=mTunePendingCorrected;
        }
        try {
            std::shared_ptr<const Source> source;
            if(tuning) {
                auto candidate=std::make_shared<Source>();candidate->sample_rate=tuning->sample_rate;candidate->name=previous?previous->name:"Tuned melody";
                candidate->stereo=*(corrected?tuning->corrected:tuning->original);
                source=candidate->sample_rate==rate?candidate:resample_source(*candidate,rate);
            }else source=!path.empty() ? load_audio_file(path,rate) : preset>=0 ? factory_source(unsigned(preset),rate) :
                previous ? (previous->sample_rate==rate ? previous : resample_source(*previous,rate)) : factory_source(0,rate);
            std::lock_guard<std::mutex> lock(mControl);
            if(mStop) return;
            if(revision!=mRequest) continue;
            if(!mEngine->prepare(source,parameters)) throw std::runtime_error("Cannot prepare these sample settings");
            mSource=std::move(source); mApplied=parameters; mError.clear();
            if(tuning){mTuneApplied=tuning;mTuneCorrected=corrected;mTunePending.reset();}
            else if(!path.empty() || preset>=0){mTuneApplied.reset();mTuneCorrected=false;}
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
    if(!mGestures.load() && now_ns()-mPreparationChanged.load()>=settle_ns && !same(preparation(),mPending)) queue(-1,{});
    const auto s=mEngine->snapshot();
    const json state={{"ready",s.ready},{"preparing",s.preparing || mSubmitted.load()!=mQueued.load()},{"progress",s.progress},{"name",s.name},
        {"duration",s.duration},{"rate",s.sample_rate},{"voices",s.active_voices},{"notes",s.active_notes},{"peak",mPeak.load()},
        {"error",!mRateSupported.load()?"Use a host sample rate from 8 to 192 kHz":mOfflineTimeout.load()?"Preparation timed out. Wait until ready, then bounce again.":mError.empty()?s.error:mError},{"underruns",s.underruns},{"dropped",s.dropped_notes+mMidiOverflow.load()},
        {"waveform",s.waveform},{"tuned",bool(mTuneApplied && mTuneCorrected)},{"applied",encode(s.preparation)},{"revision",s.revision}};
    const auto text=state.dump(); SendArbitraryMsgFromDelegate(100,int(text.size()),text.data());
}
bool ChronoBentPlugin::OnMessage(int tag,int control,int size,const void *data) {
    (void)control; (void)size; (void)data;
    if(tag==20) {show_tuning();return true;}
    if(tag==1) { request(-1); return true; }
    if(tag==2) { const auto path=choose_audio_file(); if(!path.empty()) request(-1,path); return true; }
    if(tag>=10 && tag<=12) { request(tag-10); return true; }
    return false;
}
chronobent_host::TuneDocument ChronoBentPlugin::tuning_document() const {
    std::lock_guard<std::mutex> lock(mControl);if(!mSource)return {};
    auto original=mTuneApplied?mTuneApplied->original:chronobent_host::Audio(mSource,&mSource->stereo);
    return {original,mTuneApplied?mTuneApplied->sample_rate:mSource->sample_rate,mSource->name,mTuneApplied,mTuneCorrected};
}
bool ChronoBentPlugin::apply_tuning(std::shared_ptr<const chronobent_host::TuneResult> result,bool corrected) {
    std::lock_guard<std::mutex> lock(mControl);if(!mSource || !result || mSubmitted.load()!=mQueued.load())return false;
    const auto original=mTuneApplied?mTuneApplied->original:chronobent_host::Audio(mSource,&mSource->stereo);
    if(result->original!=original || !result->corrected || result->corrected->size()!=original->size())return false;
    mTunePending=std::move(result);mTunePendingCorrected=corrected;mPreset=-1;mPath.clear();mPending=preparation();
    mQueued.store(++mRequest);mWake.notify_one();return true;
}
void ChronoBentPlugin::show_tuning() {
    if(!mTuningEditor)mTuningEditor=std::make_unique<chronobent_host::MacTuningEditor>([this]{return tuning_document();},
        [this](std::shared_ptr<const chronobent_host::TuneResult> result,bool corrected){return apply_tuning(std::move(result),corrected);});
    mTuningEditor->show();
}
bool ChronoBentPlugin::SerializeState(IByteChunk &chunk) const {
    std::lock_guard<std::mutex> lock(mControl);
    try {
        std::array<double,kNumParams> values{};
        for(int i=0;i<kNumParams;++i) values[i]=GetParam(i)->Value();
        const auto audio=mTuneApplied?(mTuneCorrected?mTuneApplied->corrected:mTuneApplied->original):
            mSource?chronobent_host::Audio(mSource,&mSource->stereo):nullptr;
        const auto alternate=mTuneApplied?(mTuneCorrected?mTuneApplied->original:mTuneApplied->corrected):nullptr;
        const json state={{"version",2},{"params",values},{"applied",encode(mApplied)},
            {"rate",mTuneApplied?mTuneApplied->sample_rate:mSource?mSource->sample_rate:mRate},{"name",mSource?mSource->name:"Glass Circuit"},
            {"frames",audio?audio->size()/2:0},{"tuning",chronobent_plugin::encode_tuning(mTuneApplied,mTuneCorrected)}};
        const auto text=state.dump(); const uint32_t magic=0x354e4243,length=uint32_t(text.size());
        chunk.Put(&magic); chunk.Put(&length); chunk.PutBytes(text.data(),int(text.size()));
        if(audio)chunk.PutBytes(audio->data(),int(audio->size()*sizeof(float)));
        if(alternate)chunk.PutBytes(alternate->data(),int(alternate->size()*sizeof(float)));
        return true;
    } catch(...) { return false; }
}
int ChronoBentPlugin::UnserializeState(const IByteChunk &chunk,int position) {
    try {
        uint32_t magic=0,length=0;
        position=chunk.Get(&magic,position); if(position<0 || magic!=0x354e4243) return -1;
        position=chunk.Get(&length,position); if(position<0 || length>1048576 || length>uint32_t(chunk.Size()-position)) return -1;
        std::string text(length,'\0'); position=chunk.GetBytes(text.data(),int(length),position); if(position<0) return -1;
        const auto state=json::parse(text);
        if((state.at("version")!=1 && state.at("version")!=2) || state.at("params").size()!=kNumParams) return -1;
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
        const auto tuning=chronobent_plugin::decode_tuning(state.at("version")==2?state.at("tuning"):json(nullptr),frames);
        if(uint64_t(chunk.Size()-position)!=frames*8*(tuning.present?2:1)+trailer) return -1;
        auto source=std::make_shared<Source>(); source->sample_rate=rate; source->name=state.at("name").get<std::string>();
        if(source->name.size()>4096) return -1;
        source->stereo.resize(std::size_t(frames)*2);
        if(frames) position=chunk.GetBytes(source->stereo.data(),int(frames*8),position);
        if(position<0) return -1;
        for(float v:source->stereo) if(!std::isfinite(v) || std::abs(v)>64) return -1;
        chronobent_host::Audio alternate;
        if(tuning.present) {
            auto pcm=std::make_shared<std::vector<float>>(size_t(frames)*2);position=chunk.GetBytes(pcm->data(),int(frames*8),position);if(position<0)return -1;
            for(float v:*pcm)if(!std::isfinite(v) || std::abs(v)>64)return -1;
            alternate=std::move(pcm);
        }
        const auto restored=chronobent_plugin::restore_tuning(tuning,chronobent_host::Audio(source,&source->stereo),alternate,rate);
        if(!(parameters.tempo>=.5 && parameters.tempo<=2) || !(parameters.semitones>=-24 && parameters.semitones<=24) ||
           !(parameters.formant_scale==0 || (parameters.formant_scale>=.5 && parameters.formant_scale<=2)) ||
           !(parameters.envelope_ms>=1 && parameters.envelope_ms<=4) || parameters.transients>2 ||
           int(parameters.profile)<0 || int(parameters.profile)>2 || parameters.root_note<24 || parameters.root_note>103) return -1;
        std::lock_guard<std::mutex> lock(mControl);
        for(int i=0;i<kNumParams;++i) GetParam(i)->Set(values[i]);
        mSource=frames?source:factory_source(0,mRate); mApplied=mPending=parameters;
        mTuneApplied=restored;mTuneCorrected=tuning.corrected;mTunePending.reset();
        mPreset=-1; mPath.clear(); mQueued.store(++mRequest); mWake.notify_one(); return position;
    } catch(...) { return -1; }
}
