// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "tuning_session.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"session:%d: %s\n",__LINE__,#x);std::exit(1);}} while(0)
using namespace chronobent_host;
Audio source(double hz,double seconds=1.0) {
    auto pcm=std::make_shared<std::vector<float>>(size_t(seconds*48000)*2);
    for(size_t i=0;i<pcm->size()/2;++i){(*pcm)[i*2]=float(.3*std::sin(6.283185307179586*hz*i/48000));(*pcm)[i*2+1]=-(*pcm)[i*2];}return pcm;
}
int main() {
    TuningSession session;CHECK(!session.analyze());CHECK(!session.set_source(nullptr,48000));auto first=source(225);
    CHECK(session.set_source(first,48000));CHECK(!session.snapshot().result);CHECK(session.analyze());CHECK(session.wait());
    auto a=session.snapshot();CHECK(!a.busy && a.error.empty() && a.result && a.result->notes.size()==1);
    CHECK(a.result->original==first && *a.result->corrected!=*first);
    auto options=chronobent_tune_default_options();options.amount=0;CHECK(session.apply(options,{}));CHECK(session.wait());auto identity=session.snapshot();CHECK(*identity.result->corrected==*first);
    options.amount=1;std::vector<TuneEdit> edits{{a.result->notes[0].first_frame,a.result->notes[0].end_frame,58}};
    CHECK(session.apply(options,edits));CHECK(session.wait());auto edited=session.snapshot();CHECK(edited.result->notes[0].manual && edited.result->notes[0].target_midi==58);
    CHECK(session.analyze());CHECK(session.wait());CHECK(session.snapshot().result->notes[0].manual);edited=session.snapshot();
    edits[0].end++;CHECK(!session.apply(options,edits));CHECK(session.snapshot().result==edited.result);
    options.amount=std::numeric_limits<double>::quiet_NaN();CHECK(!session.apply(options,{}));CHECK(session.snapshot().result==edited.result);
    // Concurrent observers retain immutable results while coalesced requests run.
    std::atomic<bool> stop{false};std::thread observer([&]{while(!stop.load()){auto s=session.snapshot();if(s.result){CHECK(s.result->corrected->size()==s.result->original->size());for(const auto &n:s.result->notes)CHECK(n.end_frame>n.first_frame);}}});
    for(int i=0;i<20;++i){options=chronobent_tune_default_options();options.amount=double(i)/20;CHECK(session.apply(options,{}));}
    options.amount=.73;CHECK(session.apply(options,{}));CHECK(session.wait());stop.store(true);observer.join();CHECK(session.snapshot().result->options.amount==.73);
    CHECK(*identity.result->corrected==*first);CHECK(edited.result->notes[0].manual); // Old snapshots remain intact.
    auto long_source=source(333,20);CHECK(session.set_source(long_source,48000));CHECK(session.analyze());auto next=source(300);
    CHECK(session.set_source(next,48000));CHECK(!session.snapshot().result);CHECK(session.analyze());CHECK(session.wait());CHECK(session.snapshot().result->original==next);
    auto final=session.snapshot().result;CHECK(session.analyze());session.cancel();CHECK(!session.snapshot().busy && session.snapshot().result==final);
    auto invalid=std::make_shared<std::vector<float>>(*first);(*invalid)[30]=std::numeric_limits<float>::infinity();CHECK(session.set_source(invalid,48000));CHECK(session.analyze());CHECK(!session.wait());CHECK(!session.snapshot().result && !session.snapshot().error.empty());
    CHECK(session.set_source(first,48000));CHECK(session.analyze());CHECK(session.wait());
    session.clear();CHECK(!session.snapshot().result && !session.analyze());
    // Destruction cancels and joins an active analysis without publishing partial PCM.
    {TuningSession dying;CHECK(dying.set_source(long_source,48000));CHECK(dying.analyze());}
    std::puts("tuning session: immutable A/B, edits, coalescing, cancellation, concurrent snapshots, source replacement and retry passed");
}
