// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <AVFoundation/AVFoundation.h>
#include "mac_audio.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool ok,const char *why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
int main() {
    @autoreleasepool {
        NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        require([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil],"temporary audio fixture directory");
        NSString *path=[directory stringByAppendingPathComponent:@"Stereo tone.wav"];
        AVAudioFormat *format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100 channels:2];
        NSError *error=nil;
        {
            AVAudioFile *file=[[AVAudioFile alloc] initForWriting:[NSURL fileURLWithPath:path] settings:format.settings commonFormat:AVAudioPCMFormatFloat32 interleaved:NO error:&error];
            require(file && !error,"write own WAV fixture");
            AVAudioPCMBuffer *buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:44100]; buffer.frameLength=44100;
            for(unsigned i=0;i<44100;++i) { const float v=float(.25*std::sin(2*3.14159265358979323846*1000*i/44100)); buffer.floatChannelData[0][i]=v; buffer.floatChannelData[1][i]=v*.5f; }
            require([file writeFromBuffer:buffer error:&error],"write stereo PCM");
        }
        const auto exact=chronobent_instrument::load_audio_file(path.UTF8String,44100);
        require(exact->stereo.size()==88200 && exact->name=="Stereo tone","decode duration and display name");
        for(unsigned i=0;i<44100;++i) {
            const float v=float(.25*std::sin(2*3.14159265358979323846*1000*i/44100));
            require(exact->stereo[2*i]==v && exact->stereo[2*i+1]==v*.5f,"float file decoding preserves samples and channel order");
        }
        for(double rate:{8000.,48000.,96000.,192000.}) {
            const auto converted=chronobent_instrument::load_audio_file(path.UTF8String,rate);
            require(std::abs(double(converted->stereo.size()/2)-rate)<=2,"resampling duration");
            double dot=0,energy=0; unsigned crossings=0;
            for(std::size_t i=100;i+100<converted->stereo.size()/2;++i) {
                const float v=converted->stereo[2*i];
                require(std::isfinite(v) && std::abs(v*.5f-converted->stereo[2*i+1])<1e-6,"resampling keeps stereo linkage");
                dot+=v*std::sin(2*3.14159265358979323846*1000*double(i)/rate); energy+=v*v;
                if(i>100 && converted->stereo[2*(i-1)]<=0 && v>0) ++crossings;
            }
            require(energy>10 && dot>0,"resampled phase and energy");
            const double duration=(double(converted->stereo.size()/2)-200)/rate;
            require(std::abs(crossings/duration-1000)<3,"resampling preserves frequency");
        }
        NSString *bad=[directory stringByAppendingPathComponent:@"Invalid.wav"];
        [@"Not an audio container" writeToFile:bad atomically:YES encoding:NSUTF8StringEncoding error:nil];
        bool rejected=false; try { chronobent_instrument::load_audio_file(bad.UTF8String,48000); } catch(...) { rejected=true; }
        require(rejected,"malformed import rejection");
        require([NSFileManager.defaultManager removeItemAtPath:directory error:nil],"remove only owned temporary fixtures");
        std::puts("plugin import: float identity, channel order, 8/48/96/192 kHz rate conversion, duration, frequency and malformed-file rejection passed");
    }
}
