// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
// A worker-side, dependency-free monophonic tuning example. The host owns PCM,
// analysis lifetime and output storage; it publishes only the completed result.
#include "chronobent/tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
struct Source {
    std::vector<float> mono;
    static int read(void *context,uint64_t first,size_t frames,float *out) {
        const auto &s=*static_cast<Source *>(context);
        if(first>s.mono.size() || frames>s.mono.size()-first)return 0;
        std::copy_n(s.mono.data()+first,frames,out);return 1;
    }
};
int main() {
    constexpr double rate=48000;
    Source source;source.mono.resize(size_t(rate*2));
    for(size_t i=0;i<source.mono.size();++i)source.mono[i]=float(.25*std::sin(6.283185307179586*225*i/rate));
    chronobent_cpp::Tuner tuner(chronobent_tune_default_config(rate,1),source.mono.size());
    if(tuner.analyze(Source::read,&source)!=CHRONOBENT_OK)return 1;
    auto options=chronobent_tune_default_options();options.amount=.9;options.preserve_vibrato=1;
    if(tuner.set_options(options)!=CHRONOBENT_OK)return 2;
    size_t count=0;const auto notes=tuner.notes(count);
    for(size_t i=0;i<count;++i)std::printf("Note %zu: detected %.2f MIDI, automatic target %.0f\n",i,notes[i].detected_midi,notes[i].target_midi);
    // A caller can replace all manual edits in one allocation-free plan pass:
    // chronobent_tune_edit edit{0,58}; tuner.set_notes(&edit,1);
    std::vector<float> corrected(source.mono.size());
    size_t first=0;
    while(first<corrected.size()) {
        size_t produced=0;
        const auto status=tuner.render(Source::read,&source,first,corrected.data()+first,
            std::min<size_t>(4096,corrected.size()-first),produced);
        if((status!=CHRONOBENT_OK && status!=CHRONOBENT_END) || !produced)return 3;
        first+=produced;
    }
    std::printf("Rendered %zu frames at the original duration. Original PCM remains available for A/B.\n",first);
    return count==1?0:4;
}
