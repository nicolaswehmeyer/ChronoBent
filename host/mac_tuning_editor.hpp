// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_MAC_TUNING_EDITOR_HPP
#define CHRONOBENT_MAC_TUNING_EDITOR_HPP
#include "tuning_session.hpp"
#include <functional>
namespace chronobent_host {
struct TuneDocument {
    Audio original;
    double sample_rate=0;
    std::string name;
    std::shared_ptr<const TuneResult> correction;
    bool corrected=false;
};
// AppKit owns only presentation; the portable session owns the analysis worker.
// Construct/show on the main UI thread. Destruction closes and drains AppKit
// before bundle unload. Callbacks run on that UI thread and must preserve source
// identity: an obsolete result must never replace a subsequently loaded source.
class MacTuningEditor {
public:
    using Document=std::function<TuneDocument()>;
    using Apply=std::function<bool(std::shared_ptr<const TuneResult>,bool corrected)>;
    MacTuningEditor(Document,Apply);
    ~MacTuningEditor();
    void show();
    void close();
    // Native layout/control exercise for an in-process synthetic host. Requires
    // a document, never opens an audio output or a file chooser.
    bool self_test(const std::string &screenshot_path={});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
