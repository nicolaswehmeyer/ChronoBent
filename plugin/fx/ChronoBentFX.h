// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#pragma once
#include "../host.hpp"
#include "../effect.hpp"
#include "../../host/mac_tuning_editor.hpp"
#include <mutex>
using namespace iplug;
enum { kFxPitch,kFxTime,kFxTimbre,kFxMix,kFxGain,kFxMode,kFxTransients,kFxFormants,kFxNumParams };
class ChronoBentFX final : public ChronoBentHost {
public:
    explicit ChronoBentFX(const InstanceInfo &);
    ~ChronoBentFX();
    void ProcessBlock(sample **,sample **,int) override;
    void OnReset() override;
    void OnIdle() override;
    bool OnMessage(int,int,int,const void *) override;
    bool SerializeState(IByteChunk &) const override;
    int UnserializeState(const IByteChunk &,int) override;
    int StateLimit() const override { return 94000000; }
#ifdef VST3_API
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &) override;
#endif
private:
    chronobent_host::TuneDocument tuning_document() const;
    bool apply_tuning(std::shared_ptr<const chronobent_host::TuneResult>,bool);
    mutable std::mutex mControl;
    double mRate=48000;
    std::unique_ptr<chronobent_effect::Engine> mEngine;
    std::shared_ptr<const chronobent_instrument::Source> mTuneSource;
    std::shared_ptr<const chronobent_host::TuneResult> mTuneApplied;
    bool mTuneCorrected=false;
    std::unique_ptr<chronobent_host::MacTuningEditor> mTuningEditor;
    std::atomic<bool> mRateSupported{true};
    std::atomic<float> mInputPeak{0},mOutputPeak{0};
    std::string mError;
    bool mSkippedBlock=false;
};
