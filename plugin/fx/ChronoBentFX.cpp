// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "ChronoBentFX.h"
#include "IPlug_include_in_plug_src.h"
#include "../mac_audio.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
using namespace chronobent_effect;
using Source=chronobent_instrument::Source;
using json=nlohmann::json;
ChronoBentFX::ChronoBentFX(const InstanceInfo &info):ChronoBentHost(info,MakeConfig(kFxNumParams,1)) {
    GetParam(kFxPitch)->InitDouble("Pitch",0,-24,24,.01,"st");
    GetParam(kFxTime)->InitDouble("Capture time",1,.5,2,.001,"×");
    GetParam(kFxTimbre)->InitDouble("Timbre",0,-12,12,.01,"st");
    GetParam(kFxMix)->InitDouble("Mix",100,0,100,.1,"%");
    GetParam(kFxGain)->InitDouble("Output",0,-60,6,.1,"dB");
    GetParam(kFxMode)->InitEnum("Source",0,{"Live input","Record capture","Captured loop"},IParam::kFlagCannotAutomate);
    GetParam(kFxTransients)->InitEnum("Transients",2,{"Smooth","Crisp","Mixed"});
    GetParam(kFxFormants)->InitBool("Keep formants",false);
    mEngine=std::make_unique<Engine>(mRate);
    MakeDefaultPreset("Live input",1);
    SetLatency(int(mEngine->latency_frames()));
    // The maximum tail is unbounded because captured loops can keep playing.
    // Keep format metadata stable; users select an explicit bounce range.
    SetTailSize(kTailInfinite);
    SetMaxJSStringLength(32768);
    mEditorInitFunc=[this] { LoadIndexHtml(__FILE__,GetBundleID()); EnableScroll(false); };
}
void ChronoBentFX::OnReset() {
    std::lock_guard<std::mutex> lock(mControl);
    const double rate=GetSampleRate(); mRateSupported.store(rate>=8000 && rate<=192000);
    if(!mRateSupported.load()) return;
    try {
        auto source=mEngine->captured_source();
        if(source && source->sample_rate!=rate) source=chronobent_instrument::resample_source(*source,rate);
        auto engine=std::make_unique<Engine>(rate);
        if(source && !engine->restore_capture(source)) throw std::runtime_error("Cannot restore captured audio at this sample rate");
        mEngine=std::move(engine); mRate=rate; mError.clear();
        if(GetParam(kFxMode)->Int()==1) GetParam(kFxMode)->Set(0);
        SetLatency(int(mEngine->latency_frames()));
        SetTailSize(kTailInfinite);
    } catch(const std::exception &e) { mError=e.what(); mRateSupported.store(false); }
}
void ChronoBentFX::ProcessBlock(sample **inputs,sample **outputs,int frames) {
    const int incoming=NInChansConnected(),outgoing=NOutChansConnected();
    if(!mRateSupported.load()) { for(int c=0;c<outgoing;++c) std::fill_n(outputs[c],frames,0); return; }
    if(mSkippedBlock) { mEngine->reset_audio(); mSkippedBlock=false; }
    Controls controls;
    controls.semitones=GetParam(kFxPitch)->Value(); controls.tempo=GetParam(kFxTime)->Value();
    controls.formant_scale=GetParam(kFxFormants)->Bool()?std::exp2(GetParam(kFxTimbre)->Value()/12):0;
    controls.mix=GetParam(kFxMix)->Value()/100; controls.gain_db=GetParam(kFxGain)->Value();
    controls.mode=Mode(GetParam(kFxMode)->Int()); controls.transients=unsigned(GetParam(kFxTransients)->Int());
    std::array<float,128> left{},right{},out_left{},out_right{};
    float input_peak=0,output_peak=0;
    const bool offline=GetRenderingOffline();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(int at=0;at<frames;) {
        const int count=std::min(128,frames-at);
        for(int i=0;i<count;++i) {
            left[i]=incoming?float(inputs[0][at+i]):0;
            right[i]=incoming>1?float(inputs[1][at+i]):left[i];
            if(std::isfinite(left[i])) input_peak=std::max(input_peak,std::abs(left[i]));
            if(std::isfinite(right[i])) input_peak=std::max(input_peak,std::abs(right[i]));
        }
        const auto remaining=offline?std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count()):0;
        mEngine->process(left.data(),right.data(),out_left.data(),out_right.data(),std::size_t(count),controls,offline,unsigned(remaining));
        for(int i=0;i<count;++i) {
            if(outgoing) outputs[0][at+i]=out_left[i];
            if(outgoing>1) outputs[1][at+i]=out_right[i];
            output_peak=std::max(output_peak,std::max(std::abs(out_left[i]),std::abs(out_right[i])));
        }
        at+=count;
    }
    mInputPeak.store(input_peak); mOutputPeak.store(output_peak);
}
#ifdef VST3_API
Steinberg::tresult PLUGIN_API ChronoBentFX::process(Steinberg::Vst::ProcessData &data) {
    const auto result=ChronoBentHost::process(data);
    // The framework owns host bypass. It does not call ProcessBlock while
    // bypassed, so retire the paused input epoch before sound resumes.
    if(GetBypassed() && data.numSamples>0) mSkippedBlock=true;
    return result;
}
#endif
void ChronoBentFX::OnIdle() {
    std::unique_lock<std::mutex> lock(mControl,std::try_to_lock); if(!lock.owns_lock()) return;
    const auto state=mEngine->snapshot();
    if(state.auto_completed && GetParam(kFxMode)->Int()==1) {
        GetParam(kFxMode)->Set(2); SendParameterValueFromAPI(kFxMode,2,false);
    }
    const json message={{"mode",int(state.mode)},{"ready",state.ready},{"captured",state.captured_seconds},
        {"progress",state.capture_progress},{"position",state.position},{"input",mInputPeak.load()},{"output",mOutputPeak.load()},
        {"waveform",state.waveform},{"latency",double(mEngine->latency_frames())/mRate*1000},
        {"error",!mError.empty()?mError:!mRateSupported.load()?"Use a host sample rate from 8 to 192 kHz":state.error},
        {"underruns",state.underruns},{"dropped",state.dropped},{"invalid",state.invalid_samples}};
    const auto text=message.dump(); SendArbitraryMsgFromDelegate(100,int(text.size()),text.data());
}
bool ChronoBentFX::SerializeState(IByteChunk &chunk) const {
    std::lock_guard<std::mutex> lock(mControl);
    try {
        const auto source=mEngine->captured_source();
        std::array<double,kFxNumParams> values{};
        for(int i=0;i<kFxNumParams;++i) values[i]=GetParam(i)->Value();
        if(values[kFxMode]==1 || (values[kFxMode]==2 && !source)) values[kFxMode]=0;
        const json state={{"version",1},{"params",values},{"rate",source?source->sample_rate:mRate},
            {"name",source?source->name:"Captured passage"},{"frames",source?source->stereo.size()/2:0}};
        const auto text=state.dump(); const uint32_t magic=0x58464243,length=uint32_t(text.size());
        chunk.Put(&magic); chunk.Put(&length); chunk.PutBytes(text.data(),int(text.size()));
        if(source) chunk.PutBytes(source->stereo.data(),int(source->stereo.size()*sizeof(float)));
        return true;
    } catch(...) { return false; }
}
int ChronoBentFX::UnserializeState(const IByteChunk &chunk,int position) {
    try {
        uint32_t magic=0,length=0;
        position=chunk.Get(&magic,position); if(position<0 || magic!=0x58464243) return -1;
        position=chunk.Get(&length,position); if(position<0 || length>65536 || length>uint32_t(chunk.Size()-position)) return -1;
        std::string text(length,'\0'); position=chunk.GetBytes(text.data(),int(length),position); if(position<0) return -1;
        const auto state=json::parse(text);
        if(state.at("version")!=1 || state.at("params").size()!=kFxNumParams) return -1;
        const auto values=state.at("params").get<std::array<double,kFxNumParams>>();
        for(int i=0;i<kFxNumParams;++i)
            if(!std::isfinite(values[i]) || values[i]<GetParam(i)->GetMin() || values[i]>GetParam(i)->GetMax() ||
               (i>=kFxMode && std::floor(values[i])!=values[i])) return -1;
        const double rate=state.at("rate"); const uint64_t frames=state.at("frames");
        if(!(rate>=8000 && rate<=192000) || frames>uint64_t(rate*Engine::capture_seconds) || frames>uint64_t(chunk.Size()-position)/8) return -1;
#ifdef VST3_API
        constexpr int trailer=4;
#else
        constexpr int trailer=0;
#endif
        if(uint64_t(chunk.Size()-position)!=frames*8+trailer || (values[kFxMode]==2 && !frames)) return -1;
        auto source=std::make_shared<Source>(); source->sample_rate=rate; source->name=state.at("name").get<std::string>();
        if(source->name.size()>4096) return -1;
        source->stereo.resize(std::size_t(frames)*2);
        if(frames) position=chunk.GetBytes(source->stereo.data(),int(frames*8),position);
        if(position<0) return -1;
        for(float v:source->stereo) if(!std::isfinite(v) || std::abs(v)>64) return -1;
        std::lock_guard<std::mutex> lock(mControl);
        std::shared_ptr<const Source> admitted=frames?source:nullptr;
        if(admitted && admitted->sample_rate!=mRate) admitted=chronobent_instrument::resample_source(*admitted,mRate);
        if(!mEngine->restore_capture(admitted)) return -1;
        for(int i=0;i<kFxNumParams;++i) GetParam(i)->Set(i==kFxMode && values[i]==1?0:values[i]);
        mError.clear(); return position;
    } catch(...) { return -1; }
}
