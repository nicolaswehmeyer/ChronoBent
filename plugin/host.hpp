// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#pragma once
#include "IPlug_include_in_plug_hdr.h"
// Shared editor lifetime and transactional host-state admission. Each plugin
// still owns its format identity, parameters, source schema and audio engine.
class ChronoBentHost : public iplug::Plugin {
public:
    using iplug::Plugin::Plugin;
    virtual int StateLimit() const { return 370000000; }
    bool CanNavigateToURL(const char *);
    bool OnCanDownloadMIMEType(const char *) override { return false; }
    void *OpenWindow(void *) override;
#ifdef AU_API
    OSStatus SetState(CFPropertyListRef) override;
#endif
#ifdef VST3_API
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *) override;
#endif
};
