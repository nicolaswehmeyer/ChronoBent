// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#pragma once
#include "IPlug_include_in_plug_hdr.h"
#include "instrument.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
using namespace iplug;
enum { kPitch, kTime, kTimbre, kAttack, kRelease, kGain, kLoop, kRoot, kProfile, kTransients, kFormants, kNumParams };
class ChronoBentPlugin final : public Plugin {
public:
    explicit ChronoBentPlugin(const InstanceInfo &);
    ~ChronoBentPlugin();
    void ProcessBlock(sample **,sample **,int) override;
    void ProcessMidiMsg(const IMidiMsg &) override;
    void OnReset() override;
    void OnIdle() override;
    bool OnMessage(int,int,int,const void *) override;
    bool SerializeState(IByteChunk &) const override;
    int UnserializeState(const IByteChunk &,int) override;
    bool CanNavigateToURL(const char *);
    bool OnCanDownloadMIMEType(const char *) override { return false; }
    void *OpenWindow(void *) override;
#ifdef AU_API
    OSStatus SetState(CFPropertyListRef) override;
#endif
#ifdef VST3_API
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *) override;
#endif
private:
    chronobent_instrument::Preparation preparation() const;
    void request(int preset,const std::string &path={});
    void work();
    void midi(const IMidiMsg &) noexcept;
    // Hosts suspend processing before OnReset. Control/UI/serialization share
    // this mutex; ProcessBlock and MIDI never take it.
    mutable std::mutex mControl;
    std::condition_variable mWake;
    bool mStop=false;
    uint64_t mRequest=0;
    int mPreset=0;
    std::string mPath,mError;
    double mRate=48000;
    chronobent_instrument::Preparation mPending,mApplied;
    std::shared_ptr<const chronobent_instrument::Source> mSource;
    std::unique_ptr<chronobent_instrument::Engine> mEngine;
    std::thread mWorker;
    std::array<IMidiMsg,512> mMidi{};
    std::size_t mMidiCount=0;
    std::atomic<float> mPeak{0};
    std::atomic<uint64_t> mMidiOverflow{0};
    std::atomic<uint64_t> mQueued{0},mSubmitted{0};
    std::atomic<bool> mRateSupported{true};
    std::atomic<bool> mOfflineTimeout{false};
};
