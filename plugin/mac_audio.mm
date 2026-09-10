// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#include "mac_audio.hpp"
#include <cmath>
#include <stdexcept>
namespace chronobent_instrument {
std::string choose_audio_file() {
    NSOpenPanel *panel=[NSOpenPanel openPanel];
    panel.canChooseDirectories=NO; panel.allowsMultipleSelection=NO;
    panel.title=@"Load a sample · ChronoBent";
    if([panel runModal]!=NSModalResponseOK) return {};
    return std::string(panel.URL.path.UTF8String ?: "");
}
std::shared_ptr<const Source> resample_source(const Source &source,double rate) {
    if(source.sample_rate==rate) return std::make_shared<Source>(source);
    if(!(rate>=8000 && rate<=192000) || !(source.sample_rate>=8000 && source.sample_rate<=192000) || source.stereo.empty())
        throw std::runtime_error("Unsupported sample rate");
    @autoreleasepool {
        AVAudioFormat *inputFormat=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:source.sample_rate channels:2];
        AVAudioFormat *outputFormat=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate channels:2];
        AVAudioConverter *converter=[[AVAudioConverter alloc] initFromFormat:inputFormat toFormat:outputFormat];
        converter.sampleRateConverterQuality=AVAudioQualityMax;
        auto out=std::make_shared<Source>(); out->name=source.name; out->sample_rate=rate;
        const auto expected=std::size_t(std::ceil(source.stereo.size()/2*rate/source.sample_rate));
        out->stereo.reserve(expected*2);
        AVAudioPCMBuffer *input=[[AVAudioPCMBuffer alloc] initWithPCMFormat:inputFormat frameCapacity:8192];
        AVAudioPCMBuffer *output=[[AVAudioPCMBuffer alloc] initWithPCMFormat:outputFormat frameCapacity:8192];
        __block std::size_t cursor=0;
        for(;;) {
            NSError *error=nil;
            const auto status=[converter convertToBuffer:output error:&error withInputFromBlock:^AVAudioBuffer *(AVAudioPacketCount count, AVAudioConverterInputStatus *state) {
                const auto n=std::min<std::size_t>({8192,count,source.stereo.size()/2-cursor});
                if(!n) { *state=AVAudioConverterInputStatus_EndOfStream; return nil; }
                input.frameLength=static_cast<AVAudioFrameCount>(n);
                for(std::size_t i=0;i<n;++i) for(unsigned c=0;c<2;++c) input.floatChannelData[c][i]=source.stereo[2*(cursor+i)+c];
                cursor+=n; *state=AVAudioConverterInputStatus_HaveData; return input;
            }];
            if(status==AVAudioConverterOutputStatus_Error) throw std::runtime_error("Sample-rate conversion failed");
            for(AVAudioFrameCount i=0;i<output.frameLength;++i) {
                out->stereo.push_back(output.floatChannelData[0][i]); out->stereo.push_back(output.floatChannelData[1][i]);
            }
            if(status==AVAudioConverterOutputStatus_EndOfStream) break;
            if(out->stereo.size()/2>expected+8192) throw std::runtime_error("Invalid sample-rate conversion length");
        }
        if(out->stereo.empty()) throw std::runtime_error("Empty converted sample");
        return out;
    }
}
std::shared_ptr<const Source> load_audio_file(const std::string &path,double rate) {
    @autoreleasepool {
        NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        NSError *error=nil;
        AVAudioFile *file=[[AVAudioFile alloc] initForReading:url commonFormat:AVAudioPCMFormatFloat32 interleaved:NO error:&error];
        if(!file) throw std::runtime_error("Cannot read this audio file");
        const auto format=file.processingFormat;
        if(format.channelCount<1 || format.channelCount>2 || file.length<=0 || format.sampleRate<8000 || format.sampleRate>192000 || file.length>format.sampleRate*120)
            throw std::runtime_error("Choose a mono or stereo sample up to 120 seconds, at 8–192 kHz");
        auto s=std::make_shared<Source>(); s->name=std::string(url.lastPathComponent.stringByDeletingPathExtension.UTF8String ?: "Sample");
        s->sample_rate=format.sampleRate; s->stereo.resize(std::size_t(file.length)*2);
        AVAudioPCMBuffer *buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:8192];
        std::size_t cursor=0;
        while(cursor<s->stereo.size()/2) {
            const auto wanted=static_cast<AVAudioFrameCount>(std::min<std::size_t>(8192,s->stereo.size()/2-cursor));
            if(![file readIntoBuffer:buffer frameCount:wanted error:&error] || !buffer.frameLength) throw std::runtime_error("Incomplete audio file");
            for(AVAudioFrameCount i=0;i<buffer.frameLength;++i) for(unsigned c=0;c<2;++c) {
                const float value=buffer.floatChannelData[format.channelCount==1 ? 0 : c][i];
                if(!std::isfinite(value) || std::abs(value)>64) throw std::runtime_error("Audio contains invalid samples");
                s->stereo[2*(cursor+i)+c]=value;
            }
            cursor+=buffer.frameLength;
        }
        return resample_source(*s,rate);
    }
}
}
