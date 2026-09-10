// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#pragma once
#import <Foundation/Foundation.h>
#include <cstdint>
#include <stdexcept>
#include <cstring>
#include <vector>
namespace tuning_fixture {
struct Blob {
    uint32_t magic=0;
    NSMutableDictionary *metadata;
    NSData *audio;
};
inline Blob parse(NSData *bytes,size_t trailer=0) {
    if(bytes.length<8+trailer)throw std::runtime_error("short test state");
    uint32_t header[2];std::memcpy(header,bytes.bytes,8);
    if(header[1]>bytes.length-8-trailer)throw std::runtime_error("bad test state header");
    NSData *json=[bytes subdataWithRange:NSMakeRange(8,header[1])];NSError *error=nil;
    id object=[NSJSONSerialization JSONObjectWithData:json options:NSJSONReadingMutableContainers error:&error];
    if(error || ![object isKindOfClass:NSDictionary.class])throw std::runtime_error("bad test state JSON");
    return {header[0],object,[bytes subdataWithRange:NSMakeRange(8+header[1],bytes.length-8-header[1]-trailer)]};
}
inline NSData *encode(const Blob &blob,size_t trailer=0) {
    NSError *error=nil;NSData *json=[NSJSONSerialization dataWithJSONObject:blob.metadata options:0 error:&error];
    if(!json || error)throw std::runtime_error("cannot encode test state");
    const uint32_t header[]={blob.magic,uint32_t(json.length)};
    NSMutableData *result=[NSMutableData dataWithBytes:header length:8];[result appendData:json];[result appendData:blob.audio];
    if(trailer){uint32_t bypass=0;[result appendBytes:&bypass length:trailer];}return result;
}
inline NSMutableDictionary *metadata(bool corrected,uint64_t first,uint64_t end,double target) {
    return [@{@"version":@1,@"corrected":@(corrected),@"options":@[@4095,@440,@1,@80,@1,@0,@2],@"edits":@[@[@(first),@(end),@(target)]]} mutableCopy];
}
inline double frequency(NSData *pcm,size_t channel,double rate,size_t frame_count) {
    if(channel>=2 || frame_count>pcm.length/(2*sizeof(float)))throw std::runtime_error("short test PCM");
    const auto data=static_cast<const unsigned char *>(pcm.bytes);double first=0,last=0;size_t count=0;
    const auto sample=[&](size_t index){float value;std::memcpy(&value,data+index*sizeof(float),sizeof(float));return value;};
    for(size_t i=frame_count/4+1;i<frame_count*3/4;++i){const auto a=sample((i-1)*2+channel),b=sample(i*2+channel);if(a<=0 && b>0){const double at=double(i-1)-a/(b-a);if(!count)first=at;last=at;++count;}}
    if(count<5)throw std::runtime_error("no periodic test output");return double(count-1)*rate/(last-first);
}
}
