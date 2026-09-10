// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "../examples/pitch_lab/player.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "player: %s\n", message); std::abort(); }
}
template<class Check> static void until(Check check) {
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (!check()) {
        require(std::chrono::steady_clock::now()<deadline, "worker timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static std::vector<float> drain(audition::Player &player, bool automate = false) {
    std::array<float,4096> left{},right{};
    const std::array<std::size_t,5> blocks{{16,31,257,1024,4096}};
    std::vector<float> output;
    double previous_source=0;
    unsigned block=0,stage=0;
    player.pause(false);
    while (!player.ended()) {
        const auto wanted=blocks[block++%blocks.size()];
        until([&]{return player.queued()>=wanted || player.ended() || player.error() ||
            (player.queued()>0 && player.source_position()+double(player.queued())*2>=double(player.frames()));});
        require(!player.error(),"speed worker failed");
        const auto count=std::size_t(std::min<std::uint64_t>(wanted,player.queued()));
        if (!count) continue;
        player.pull(left.data(),right.data(),count);
        const double source=player.source_position();
        require(source>=previous_source && source<=double(player.frames()),"source cursor moved backwards or past EOF");
        previous_source=source;
        for (std::size_t i=0;i<count;++i) {
            require(std::isfinite(left[i]) && std::abs(left[i]+right[i])<1e-6,"speed change lost stereo");
            output.push_back(left[i]);
        }
        if (automate && stage==0 && source>8000) { player.request(audition::Settings{0,1.7,true,false,false}); ++stage; }
        if (automate && stage==1 && source>20000) { player.request(audition::Settings{0,0.6,true,false,false}); ++stage; }
        if (automate && stage==2 && source>32000) { player.request(audition::Settings{0,1.1,false,false,false}); ++stage; }
    }
    require(player.source_position()==double(player.frames()),"source duration not reached");
    require(!player.underruns(),"speed playback underrun");
    return output;
}
static double frequency(const std::vector<float> &audio) {
    double first=0,last=0; unsigned crossings=0;
    for (std::size_t i=audio.size()/4;i<audio.size()*3/4;++i) if(audio[i-1]<=0 && audio[i]>0) {
        const double at=double(i-1)-double(audio[i-1])/(audio[i]-audio[i-1]);
        if(!crossings) first=at;
        last=at; ++crossings;
    }
    require(crossings>3,"no tone in speed result");
    return double(crossings-1)*48000/(last-first);
}
static void seek_contract(const std::vector<float> &source) {
    audition::Player player(source,48000);
    std::array<float,256> left{},right{};
    until([&]{return player.queued()==4096;});
    require(!player.seek(48001),"out of range seek admitted");
    require(player.seek(12000),"paused seek accepted");
    until([&]{return !player.seeking() && player.queued()>=256;});
    require(player.source_position()==12000 && player.played()==0,"paused seek accounting");
    player.pull(left.data(),right.data(),256);
    for(auto value:left) require(value==0,"paused seek played");
    player.pause(false);
    player.pull(left.data(),right.data(),256);
    for(std::size_t i=128;i<256;++i) require(left[i]==source[(12000+i)*2],"stale queued audio after seek");
    require(player.source_position()==12256 && player.played()==256,"seek consumer position");
    player.request(audition::Settings{7,1.3,true,false,true,3,2,3});
    require(player.seek(20000) && player.seek(10000) && player.seek(30000),"coalesced seek accepted");
    until([&]{return !player.seeking() && player.queued()>=256;});
    require(player.source_position()==30000,"last seek wins");
    player.pull(left.data(),right.data(),256);
    for(std::size_t i=0;i<256;++i) require(std::isfinite(left[i]) && std::abs(left[i]+right[i])<1e-6,"advanced seek stereo");
    require(player.seek(48000),"seek EOF");
    until([&]{return player.ended();});
    require(player.source_position()==48000,"seek EOF position");
    player.pause(true);
    require(player.seek(0),"seek after EOF");
    until([&]{return !player.seeking() && player.queued()>=256;});
    require(!player.ended() && player.source_position()==0,"restart after EOF");
    // Concurrent callback consumption and repeated seek exercise queue ownership.
    player.pause(false);
    std::atomic<bool> stop{false};
    std::thread consumer([&] {
        std::array<float,73> l{},r{};
        while(!stop.load()) {
            player.pull(l.data(),r.data(),l.size());
            for(std::size_t i=0;i<l.size();++i)
                require(std::isfinite(l[i]) && std::abs(l[i]+r[i])<1e-6,"concurrent seek stereo");
            std::this_thread::yield();
        }
    });
    for(unsigned i=0;i<40;++i) {
        require(player.seek((i*7919)%40000),"concurrent seek accepted");
        until([&]{return !player.seeking() || player.error();});
        require(!player.error(),"concurrent seek failed");
    }
    stop.store(true); consumer.join();
}
int main() {
    std::vector<float> source(48000*2);
    for (std::size_t i=0; i<48000; ++i) {
        source[i*2]=float(0.2*std::sin(6.283185307179586*220*double(i)/48000));
        source[i*2+1]=-source[i*2];
    }
    seek_contract(source);
    audition::Player player(source, 48000);
    std::array<float, 256> left{}, right{};
    until([&]{return player.queued()>=4096;});
    player.pull(left.data(), right.data(), 256);
    require(player.played()==0, "pause advanced timeline");
    for(float value:left) require(value==0, "pause emitted audio");
    player.pause(false);
    player.pull(left.data(), right.data(), 256);
    for(std::size_t i=0;i<256;++i) require(left[i]==source[i*2], "unity queue changed input");
    player.request(7.31, false, true);
    std::size_t total=256;
    float previous=left.back(), max_jump=0;
    while (total<48000) {
        const auto count=std::min<std::size_t>(256,48000-total);
        until([&]{return player.queued()>=count || player.error();});
        require(!player.error(), "render failed");
        player.pull(left.data(),right.data(),count);
        for(std::size_t i=0;i<count;++i) {
            require(std::isfinite(left[i]) && std::abs(left[i]+right[i])<1e-6, "crossfade lost stereo");
            max_jump=std::max(max_jump,std::abs(left[i]-previous)); previous=left[i];
        }
        total+=count;
        if(total>24000 && total<24512) player.request(-5.7,true,false);
    }
    until([&]{return player.ended();});
    require(player.played()==48000 && player.underruns()==0, "duration/queue corruption");
    require(max_jump<0.08, "pitch transition introduced a click on steady tone");
    player.pull(left.data(),right.data(),256);
    for(float value:left) require(value==0,"end replayed stale audio");
    { audition::Player stop_while_queued(source,48000); }
    for (double tempo : {0.5,1.079321,1.25,2.0}) for(bool master : {false,true}) {
        audition::Player speed(source,48000,audition::Settings{0,tempo,master,false,false});
        const auto output=drain(speed);
        require(output.size()==std::size_t(std::ceil(48000/tempo)),"speed changed exact output duration");
        const double expected=master ? 220 : 220*tempo;
        const double cents=1200*std::log2(frequency(output)/expected);
        std::printf("app tempo=%.6f master=%d cents=%+.5f frames=%zu\n",tempo,master,cents,output.size());
        require(std::abs(cents)<0.1,"Master Tempo/linked-speed pitch is inaccurate");
    }
    audition::Player automated(source,48000);
    const auto moved=drain(automated,true);
    float jump=0;
    for(std::size_t i=1;i<moved.size();++i) jump=std::max(jump,std::abs(moved[i]-moved[i-1]));
    require(jump<0.08,"tempo automation clicked on steady tone");
    audition::Player bypass(source,48000,audition::Settings{12,2,false,true,true});
    const auto dry=drain(bypass);
    require(dry.size()==48000,"bypass retained speed change");
    for(std::size_t i=0;i<dry.size();++i) require(dry[i]==source[i*2],"bypass modified source");
    std::puts("audition: pause, pitch/speed automation, Master Tempo, stereo, bypass, exact duration and teardown passed");
}
