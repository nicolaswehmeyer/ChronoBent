// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#import <AVFoundation/AVFoundation.h>
#include "mac_wav_export.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace chronobent_host {
bool export_wav(Audio audio,double rate,const std::string &path,std::string &error) noexcept {
    @autoreleasepool {
        NSURL *temporary=nil;
        auto fail=[&](NSString *message) {
            try {error=message?message.UTF8String:"WAV export failed";}catch(...){}
            if(temporary)[NSFileManager.defaultManager removeItemAtURL:temporary error:nil];return false;
        };
        @try {
            try {
                if(!audio || audio->empty() || audio->size()%2 || !(rate>=8000 && rate<=192000) || audio->size()/2>rate*600 || path.empty())return fail(@"Invalid export source");
                NSURL *destination=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
                temporary=[[destination URLByDeletingLastPathComponent] URLByAppendingPathComponent:[NSString stringWithFormat:@".chronobent-%@.wav",NSUUID.UUID.UUIDString]];
                NSDictionary *settings=@{AVFormatIDKey:@(kAudioFormatLinearPCM),AVSampleRateKey:@(rate),AVNumberOfChannelsKey:@2,
                    AVLinearPCMBitDepthKey:@32,AVLinearPCMIsFloatKey:@YES,AVLinearPCMIsBigEndianKey:@NO};
                NSError *failure=nil;
                AVAudioFile *file=[[AVAudioFile alloc] initForWriting:temporary settings:settings commonFormat:AVAudioPCMFormatFloat32 interleaved:NO error:&failure];
                if(!file)return fail(failure.localizedDescription);
                AVAudioPCMBuffer *buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:file.processingFormat frameCapacity:8192];
                if(!buffer || !buffer.floatChannelData){file=nil;return fail(@"Cannot allocate export buffer");}
                for(size_t first=0;first<audio->size()/2;) {
                    const auto frames=std::min<size_t>(8192,audio->size()/2-first);buffer.frameLength=AVAudioFrameCount(frames);
                    for(size_t i=0;i<frames;++i)for(unsigned c=0;c<2;++c) {
                        const auto value=(*audio)[(first+i)*2+c];if(!std::isfinite(value) || std::abs(value)>64){file=nil;return fail(@"The export source contains invalid samples");}
                        buffer.floatChannelData[c][i]=value;
                    }
                    if(![file writeFromBuffer:buffer error:&failure]){file=nil;return fail(failure.localizedDescription);}
                    first+=frames;
                }
                file=nil;
                if(std::rename(temporary.fileSystemRepresentation,destination.fileSystemRepresentation)!=0)return fail([NSString stringWithUTF8String:std::strerror(errno)]);
                error.clear();return true;
            }catch(const std::exception &exception){return fail([NSString stringWithUTF8String:exception.what()]);}
            catch(...){return fail(@"WAV export failed");}
        }@catch(NSException *exception){return fail(exception.reason);}
    }
}
}
