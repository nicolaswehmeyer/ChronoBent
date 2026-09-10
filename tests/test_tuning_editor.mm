// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <Cocoa/Cocoa.h>
#include "mac_tuning_editor.hpp"
#include <cmath>
#include <cstdio>
using namespace chronobent_host;
int main(int argc,const char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];NSApp.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];[NSApp finishLaunching];
        auto audio=std::make_shared<std::vector<float>>(48000*6*2);double phase=0;
        for(size_t i=0;i<audio->size()/2;++i){const double note=57.25+(i/48000)%3*2+.15*std::sin(6.28318530718*5*i/48000);phase+=6.28318530718*440*std::exp2((note-69)/12)/48000;(*audio)[i*2]=float(.3*std::sin(phase));(*audio)[i*2+1]=-(*audio)[i*2];}
        TuneDocument document{audio,48000,"Synthetic melody",nullptr,false};unsigned applied=0;
        MacTuningEditor editor([&]{return document;},[&](std::shared_ptr<const TuneResult> result,bool corrected){if(result->original!=document.original)return false;document.correction=std::move(result);document.corrected=corrected;++applied;return true;});
        if(!editor.self_test(argc>1?argv[1]:"")){std::fprintf(stderr,"Native tuning editor failed\n");return 1;}
        editor.close();editor.show();editor.close();if(applied<2)return 2;
        std::puts("native tuning editor: analysis, note edit, correction controls, A/B selection and reopen passed");
    }return 0;
}
