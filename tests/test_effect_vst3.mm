// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <Cocoa/Cocoa.h>
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;
static void require(bool condition,const char *why) { if(!condition) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
static void check(tresult result,const char *why) { require(result==kResultOk,why); }
struct Context final : U::Implements<U::Directly<IHostApplication>> {
    tresult PLUGIN_API getName(String128 name) override {
        constexpr char text[]="ChronoBent native validation";
        std::fill_n(name,128,0); for(unsigned i=0;i<sizeof(text)-1;++i) name[i]=text[i]; return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID,TUID,void **out) override { *out=nullptr; return kNoInterface; }
};
struct Stream final : U::Implements<U::Directly<IBStream>> {
    std::vector<uint8_t> bytes;
    std::size_t position=0;
    tresult PLUGIN_API read(void *out,int32 count,int32 *read) override {
        if(count<0) return kInvalidArgument;
        const auto n=std::min<std::size_t>(std::size_t(count),bytes.size()-position);
        if(n) std::memcpy(out,bytes.data()+position,n);
        position+=n; if(read) *read=int32(n); return n?kResultOk:kResultFalse;
    }
    tresult PLUGIN_API write(void *input,int32 count,int32 *written) override {
        if(count<0 || position+std::size_t(count)>47000000) return kInvalidArgument;
        bytes.resize(std::max(bytes.size(),position+std::size_t(count)));
        if(count) std::memcpy(bytes.data()+position,input,std::size_t(count));
        position+=std::size_t(count); if(written) *written=count; return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset,int32 mode,int64 *result) override {
        const auto base=mode==kIBSeekSet?int64(0):mode==kIBSeekCur?int64(position):mode==kIBSeekEnd?int64(bytes.size()):int64(-1);
        if(base<0 || offset < -base || offset>int64(bytes.size())-base) return kInvalidArgument;
        position=std::size_t(base+offset); if(result) *result=int64(position); return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *out) override { if(!out) return kInvalidArgument; *out=int64(position); return kResultOk; }
};
struct VstPoint final : U::Implements<U::Directly<IParamValueQueue>> {
    ParamID id=0; double value=0; bool present=false;
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return present?1:0; }
    tresult PLUGIN_API getPoint(int32 index,int32 &offset,ParamValue &v) override {
        if(index || !present) return kInvalidArgument; offset=0; v=value; return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32,ParamValue v,int32 &index) override { present=true; value=v; index=0; return kResultOk; }
};
struct Changes final : U::Implements<U::Directly<IParameterChanges>> {
    std::array<VstPoint,16> points;
    int32 count=0;
    int32 PLUGIN_API getParameterCount() override { return count; }
    IParamValueQueue *PLUGIN_API getParameterData(int32 i) override { return i>=0&&i<count?&points[std::size_t(i)]:nullptr; }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id,int32 &index) override {
        for(int32 i=0;i<count;++i) if(points[std::size_t(i)].id==id) { index=i; return &points[std::size_t(i)]; }
        if(count==16) return nullptr;
        index=count++; auto &point=points[std::size_t(index)]; point.id=id; point.present=false; return &point;
    }
    void set(ParamID id,double value) { int32 at=0; auto p=addParameterData(id,at); require(p,"test parameter capacity"); check(p->addPoint(0,value,at),"test parameter point"); }
    void clear() { count=0; }
};
struct Host {
    IComponent *component=nullptr;
    IAudioProcessor *processor=nullptr;
    IEditController *controller=nullptr;
    Context context;
    Changes changes;
    ParamID bypass=0;
    unsigned frame=0;
    bool silent=false;
    std::array<float,512> left{},right{},out_left{},out_right{};
    Host(IPluginFactory *factory,const TUID cid) {
        check(factory->createInstance(cid,IComponent::iid.toTUID(),reinterpret_cast<void **>(&component)),"create VST3 component");
        check(component->initialize(&context),"initialize VST3 component");
        check(component->queryInterface(IAudioProcessor::iid.toTUID(),reinterpret_cast<void **>(&processor)),"VST3 processor interface");
        check(component->queryInterface(IEditController::iid.toTUID(),reinterpret_cast<void **>(&controller)),"VST3 controller interface");
        SpeakerArrangement in=SpeakerArr::kStereo,out=SpeakerArr::kStereo;
        check(processor->setBusArrangements(&in,1,&out,1),"stereo VST3 buses");
        check(component->activateBus(kAudio,kInput,0,true),"activate VST3 input");
        check(component->activateBus(kAudio,kOutput,0,true),"activate VST3 output");
        ProcessSetup setup{}; setup.processMode=kOffline; setup.symbolicSampleSize=kSample32; setup.maxSamplesPerBlock=512; setup.sampleRate=48000;
        check(processor->setupProcessing(setup),"set up offline VST3 processing");
        check(component->setActive(true),"activate VST3 component"); check(processor->setProcessing(true),"start VST3 processing");
        bool found=false;
        for(int32 i=0;i<controller->getParameterCount();++i) {
            ParameterInfo info{}; check(controller->getParameterInfo(i,info),"VST3 parameter metadata");
            if(info.flags&ParameterInfo::kIsBypass) { bypass=info.id; found=true; }
        }
        require(found,"host bypass parameter exists");
        require(processor->getLatencySamples()==10368,"VST3 latency metadata");
        require(processor->getTailSamples()==kInfiniteTail,"VST3 captured-loop tail metadata");
    }
    ~Host() {
        processor->setProcessing(false); component->setActive(false);
        controller->release(); processor->release(); component->terminate(); component->release();
    }
    static float wave(unsigned frame,unsigned channel) {
        return float((channel?.12:.2)*std::sin(6.283185307179586*(channel?220:440)*double(frame)/48000));
    }
    void parameter(ParamID id,double plain) {
        const auto value=controller->plainParamToNormalized(id,plain);
        check(controller->setParamNormalized(id,value),"update host controller parameter"); changes.set(id,value);
    }
    void render(unsigned count=512) {
        for(unsigned i=0;i<count;++i) { left[i]=silent?0:wave(frame+i,0); right[i]=silent?0:wave(frame+i,1); }
        std::array<float *,2> inputs{left.data(),right.data()},outputs{out_left.data(),out_right.data()};
        AudioBusBuffers in{},out{}; in.numChannels=out.numChannels=2; in.channelBuffers32=inputs.data(); out.channelBuffers32=outputs.data();
        ProcessData data{}; data.processMode=kOffline; data.symbolicSampleSize=kSample32; data.numSamples=int32(count);
        data.numInputs=data.numOutputs=1; data.inputs=&in; data.outputs=&out; data.inputParameterChanges=&changes;
        check(processor->process(data),"process native VST3 audio");
        frame+=count; changes.clear();
        for(unsigned i=0;i<count;++i) require(std::isfinite(out_left[i]) && std::isfinite(out_right[i]),"finite VST3 output");
    }
    std::vector<float> render_many(unsigned count) {
        std::vector<float> audio; audio.reserve(count);
        while(count) { const auto n=std::min(count,512u); render(n); audio.insert(audio.end(),out_left.begin(),out_left.begin()+n); count-=n; }
        return audio;
    }
};
int main(int argc,char **argv) {
    @autoreleasepool {
        require(argc==2,"pass the exact FX VST3 bundle"); [NSApplication sharedApplication];
        const auto url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        auto bundle=CFBundleCreate(nullptr,(__bridge CFURLRef)url); require(bundle && CFBundleLoadExecutable(bundle),"load VST3 bundle");
        auto entry=reinterpret_cast<bool (*)(CFBundleRef)>(CFBundleGetFunctionPointerForName(bundle,CFSTR("bundleEntry")));
        auto exit=reinterpret_cast<bool (*)()>(CFBundleGetFunctionPointerForName(bundle,CFSTR("bundleExit")));
        auto get=reinterpret_cast<IPluginFactory *(*)()>(CFBundleGetFunctionPointerForName(bundle,CFSTR("GetPluginFactory")));
        require(entry && exit && get && entry(bundle),"VST3 module entry");
        IPluginFactory *factory=get(); require(factory,"VST3 factory"); PClassInfo info{};
        check(factory->getClassInfo(0,&info),"VST3 effect class");
        {
            Host host(factory,info.cid); const auto delay=host.processor->getLatencySamples();
            for(unsigned first=0;first<delay+4096;first+=512) {
                host.render(); for(unsigned i=0;i<512;++i) {
                    require(host.out_left[i]==(first+i<delay?0:Host::wave(first+i-delay,0)),"VST3 exact unity and latency");
                    require(host.out_right[i]==(first+i<delay?0:Host::wave(first+i-delay,1)),"VST3 independent stereo channels");
                }
            }
            host.changes.set(host.bypass,1); host.render_many(delay+4096);
            host.silent=true; host.changes.set(host.bypass,0);
            for(float value:host.render_many(delay+4096)) require(value==0,"VST3 bypass resume retires stale input epoch");
            host.silent=false; host.parameter(5,1); host.render_many(48000);
            host.parameter(5,2); host.parameter(1,.75); host.silent=true; host.render_many(delay+4096);
            Stream state; check(host.component->getState(&state),"save VST3 embedded capture");
            require(state.bytes.size()>48000*8,"VST3 project embeds captured PCM");
            {
                Host restored(factory,info.cid); restored.silent=true; state.position=0;
                check(restored.component->setState(&state),"restore VST3 captured source");
                const auto output=restored.render_many(delay+48000);
                double energy=0; for(std::size_t i=delay;i<output.size();++i) energy+=output[i]*output[i];
                require(energy>1,"VST3 capture plays without live input");
                Host duplicate(factory,info.cid); duplicate.silent=true; state.position=0;
                check(duplicate.component->setState(&state),"repeat VST3 captured state restore");
                require(duplicate.render_many(delay+48000)==output,"deterministic VST3 restored audio");
            }
            host.parameter(0,7.25); host.render();
            Stream before_bad; check(host.component->getState(&before_bad),"snapshot state before invalid input");
            Stream bad; bad.bytes={1,2,3};
            require(host.component->setState(&bad)!=kResultOk,"VST3 malformed state refusal");
            require(std::abs(host.controller->normalizedParamToPlain(0,host.controller->getParamNormalized(0))-7.25)<.001,"VST3 failed state preserves parameters");
            Stream after_bad; check(host.component->getState(&after_bad),"snapshot state after invalid input");
            require(before_bad.bytes==after_bad.bytes,"VST3 invalid state preserves all processor settings and captured PCM");
        }
        factory->release(); require(exit(),"VST3 module exit"); CFRelease(bundle);
    }
    std::puts("FX VST3 host: exact latency/stereo, bypass resume, embedded capture, deterministic restoration and invalid-state refusal passed");
}
