// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "tuning_session.hpp"
#include "chronobent/tuning.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace chronobent_host {
namespace {
int read(void *context,uint64_t first,size_t n,float *out) {
    const auto &audio=*static_cast<const std::vector<float> *>(context);
    if(first>audio.size()/2 || n>audio.size()/2-first)return 0;
    std::copy_n(audio.data()+first*2,n*2,out);return 1;
}
void require(chronobent_status status) {
    if(status!=CHRONOBENT_OK && status!=CHRONOBENT_END)throw std::runtime_error(chronobent_status_string(status));
}
}
struct TuningSession::Impl {
    mutable std::mutex mutex;
    mutable std::condition_variable wake;
    std::atomic<uint64_t> revision{0};
    std::atomic<bool> stopping{false};
    std::atomic<double> progress{0};
    Audio source;
    double rate=0;
    bool pending=false,busy=false,release_cache=false;
    chronobent_tune_options options=chronobent_tune_default_options();
    std::vector<TuneEdit> edits;
    std::string error;
    std::shared_ptr<const TuneResult> result;
    std::thread worker;
    struct Epoch {Impl *owner;uint64_t revision;};
    static int report(void *context,double fraction) {
        auto &e=*static_cast<Epoch *>(context);
        if(e.owner->stopping.load() || e.owner->revision.load()!=e.revision)return 0;
        e.owner->progress.store(fraction*.7);return 1;
    }
    Impl():worker([this]{work();}) {}
    ~Impl(){stopping.store(true);revision.fetch_add(1);wake.notify_all();worker.join();}
    void work() noexcept {
        Audio analyzed;double analyzed_rate=0;
        std::unique_ptr<chronobent_cpp::Tuner> tuner;
        while(!stopping.load()) {
            Audio input;double input_rate;uint64_t serial;bool analyze,release;chronobent_tune_options requested;std::vector<TuneEdit> requested_edits;
            {
                std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[this]{return stopping.load() || pending || release_cache;});
                if(stopping.load())break;
                input=source;input_rate=rate;serial=revision.load();requested=options;if(pending)requested_edits=std::move(edits);analyze=pending;release=release_cache;pending=release_cache=false;
            }
            try {
                if(release){tuner.reset();analyzed.reset();}
                if(!analyze || !input)continue;
                Epoch epoch{this,serial};
                if(analyzed!=input || analyzed_rate!=input_rate || !tuner) {
                    tuner=std::make_unique<chronobent_cpp::Tuner>(chronobent_tune_default_config(input_rate,2),input->size()/2);
                    const auto status=tuner->analyze(read,const_cast<std::vector<float> *>(input.get()),report,&epoch);
                    if(status==CHRONOBENT_CANCELLED){tuner.reset();analyzed.reset();continue;}
                    require(status);analyzed=input;analyzed_rate=input_rate;
                }
                require(tuner->set_options(requested));size_t note_count=0;auto notes=tuner->notes(note_count);
                std::vector<chronobent_tune_edit> batch;batch.reserve(requested_edits.size());
                for(const auto &edit:requested_edits) {
                    if(!note_count)throw std::runtime_error("A note edit no longer matches the source analysis");
                    auto note=std::lower_bound(notes,notes+note_count,edit.first,[](const auto &n,uint64_t first){return n.first_frame<first;});
                    if(note==notes+note_count || note->first_frame!=edit.first || note->end_frame!=edit.end)
                        throw std::runtime_error("A note edit no longer matches the source analysis");
                    batch.push_back({size_t(note-notes),edit.target});
                }
                require(tuner->set_notes(batch.data(),batch.size()));
                auto output=std::make_shared<std::vector<float>>(input->size());const size_t total=input->size()/2;
                for(size_t first=0;first<total;) {
                    if(stopping.load() || revision.load()!=serial)break;
                    size_t n=0;require(tuner->render(read,const_cast<std::vector<float> *>(input.get()),first,output->data()+first*2,std::min<size_t>(8192,total-first),n));
                    if(!n)throw std::runtime_error("The tuning renderer made no progress");
                    first+=n;progress.store(.7+.3*double(first)/double(total));
                }
                if(stopping.load() || revision.load()!=serial)continue;
                auto finished=std::make_shared<TuneResult>();finished->revision=serial;finished->sample_rate=input_rate;
                finished->original=input;finished->corrected=std::move(output);finished->options=requested;finished->edits=std::move(requested_edits);
                size_t count=0;auto frames=tuner->frames(count);if(count)finished->frames.assign(frames,frames+count);
                notes=tuner->notes(count);if(count)finished->notes.assign(notes,notes+count);
                std::lock_guard<std::mutex> lock(mutex);
                if(revision.load()==serial){result=std::move(finished);busy=false;error.clear();progress.store(1);wake.notify_all();}
            }catch(const std::exception &exception) {
                tuner.reset();analyzed.reset();
                std::lock_guard<std::mutex> lock(mutex);if(revision.load()==serial){busy=false;error=exception.what();wake.notify_all();}
            }catch(...) {
                tuner.reset();analyzed.reset();
                std::lock_guard<std::mutex> lock(mutex);if(revision.load()==serial){busy=false;error="Tuning failed";wake.notify_all();}
            }
        }
    }
};
TuningSession::TuningSession():impl_(std::make_unique<Impl>()){}
TuningSession::~TuningSession()=default;
bool TuningSession::set_source(Audio audio,double rate) {
    if(!audio || audio->empty() || audio->size()%2 || !(rate>=8000 && rate<=192000) || audio->size()/2>rate*600)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->revision.fetch_add(1);impl_->source=std::move(audio);impl_->rate=rate;
    impl_->pending=impl_->busy=false;impl_->release_cache=true;impl_->result.reset();impl_->edits.clear();impl_->options=chronobent_tune_default_options();impl_->error.clear();impl_->progress.store(0);impl_->wake.notify_all();return true;
}
bool TuningSession::restore(std::shared_ptr<const TuneResult> saved) {
    if(!saved || !saved->original || !saved->corrected || saved->original->empty() || saved->original->size()%2 ||
       saved->corrected->size()!=saved->original->size() || !(saved->sample_rate>=8000 && saved->sample_rate<=192000) ||
       saved->original->size()/2>saved->sample_rate*600 || chronobent_tune_validate_options(&saved->options)!=CHRONOBENT_OK)return false;
    uint64_t previous=0;
    for(const auto &edit:saved->edits) {
        if(edit.first<previous || edit.end<=edit.first || edit.end>saved->original->size()/2 || !(edit.target>=0 && edit.target<=127))return false;
        previous=edit.end;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->revision.fetch_add(1);impl_->source=saved->original;impl_->rate=saved->sample_rate;
    impl_->options=saved->options;impl_->edits=saved->edits;impl_->result=std::move(saved);impl_->pending=impl_->busy=false;
    impl_->error.clear();impl_->progress.store(1);impl_->wake.notify_all();return true;
}
bool TuningSession::analyze() {
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->source)return false;
    if(!impl_->pending && impl_->result && impl_->result->original==impl_->source)impl_->edits=impl_->result->edits;
    impl_->revision.fetch_add(1);impl_->pending=impl_->busy=true;impl_->error.clear();impl_->progress.store(0);impl_->wake.notify_all();return true;
}
bool TuningSession::apply(chronobent_tune_options options,const std::vector<TuneEdit> &edits) {
    if(chronobent_tune_validate_options(&options)!=CHRONOBENT_OK)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->result || impl_->result->original!=impl_->source || edits.size()>impl_->result->notes.size())return false;
    uint64_t previous=0;
    for(const auto &edit:edits) {
        if(edit.first<previous || edit.end<=edit.first || !(edit.target>=0 && edit.target<=127))return false;
        previous=edit.end;
        const auto &notes=impl_->result->notes;
        auto n=std::find_if(notes.begin(),notes.end(),[&](const auto &note){return note.first_frame==edit.first && note.end_frame==edit.end;});
        const double center=n==notes.end()?0:n->detected_midi+12*std::log2(impl_->result->options.reference_hz/options.reference_hz);
        if(n==notes.end() || std::abs(edit.target-center)>5)return false;
    }
    impl_->options=options;impl_->edits=edits;impl_->revision.fetch_add(1);impl_->pending=impl_->busy=true;impl_->error.clear();impl_->progress.store(0);impl_->wake.notify_all();return true;
}
void TuningSession::cancel() {
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->revision.fetch_add(1);impl_->pending=impl_->busy=false;impl_->progress.store(0);impl_->wake.notify_all();
}
void TuningSession::clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->revision.fetch_add(1);impl_->source.reset();impl_->result.reset();
    impl_->pending=impl_->busy=false;impl_->release_cache=true;impl_->edits.clear();impl_->error.clear();impl_->progress.store(0);impl_->wake.notify_all();
}
TuneSnapshot TuningSession::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);return {impl_->busy,impl_->progress.load(),impl_->revision.load(),impl_->error,impl_->result};
}
bool TuningSession::wait(unsigned milliseconds) const {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->wake.wait_for(lock,std::chrono::milliseconds(milliseconds),[this]{return !impl_->busy;}) && impl_->error.empty() && bool(impl_->result);
}
}
