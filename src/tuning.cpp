// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/tuning.h"
#include "fft.hpp"
#include "sinc.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using chronobent_dsp::Complex;
using chronobent_dsp::Fft;
using chronobent_dsp::Sinc;
using chronobent_dsp::pi;
constexpr std::size_t states=8, quantum=256;
constexpr double tiny=1e-12;
std::size_t power_two(std::size_t n) { std::size_t p=1; while(p<n) p*=2; return p; }
double midi(double hz,double reference=440) { return 69+12*std::log2(hz/reference); }
double clamp(double x,double a,double b) { return std::max(a,std::min(b,x)); }
bool valid(const chronobent_tune_options &p) {
    return p.scale_mask && p.scale_mask<=4095 && p.reference_hz>=400 && p.reference_hz<=480 &&
        p.amount>=0 && p.amount<=1 && p.retune_ms>=0 && p.retune_ms<=400 &&
        p.preserve_vibrato>=0 && p.preserve_vibrato<=1 && p.correct_drift>=0 && p.correct_drift<=1 &&
        p.maximum_shift>=0 && p.maximum_shift<=5;
}
struct Candidate { double hz=0,probability=0,confidence=0,cost=1e30; uint8_t previous=0; };
struct Region { std::size_t first=0,end=0,mark_first=0,mark_end=0; uint64_t begin_frame=0,end_frame=0; bool active=false; };
struct Mark { double position=0,period=0; };
struct Grain { double position=0,source=0,radius=0; std::size_t region=0; };
struct Window {
    std::vector<float> data;
    uint32_t channels;
    uint64_t length;
    int64_t origin=0;
    bool cached=false;
    chronobent_read_fn reader=nullptr;
    void *user=nullptr;
    Window(std::size_t capacity,uint32_t c,uint64_t frames):data(capacity*c),channels(c),length(frames) {}
    void bind(chronobent_read_fn fn,void *source) { reader=fn;user=source;cached=false; }
    chronobent_status load(int64_t first,std::size_t frames) {
        const auto capacity=data.size()/channels;
        if(frames>capacity) return CHRONOBENT_INVALID_ARGUMENT;
        if(cached && first>=origin && uint64_t(first-origin)+frames<=capacity) return CHRONOBENT_OK;
        cached=false; origin=first; std::fill(data.begin(),data.end(),0.0f);
        const int64_t a=std::max<int64_t>(0,first),b=std::min<int64_t>(int64_t(length),first+int64_t(capacity));
        if(b>a) {
            const auto offset=std::size_t(a-first)*channels,count=std::size_t(b-a)*channels;
            if(!reader(user,uint64_t(a),std::size_t(b-a),data.data()+offset)) return CHRONOBENT_SOURCE_UNAVAILABLE;
            for(std::size_t i=0;i<count;++i) if(!std::isfinite(data[offset+i]) || std::abs(data[offset+i])>64)
                return CHRONOBENT_INVALID_AUDIO;
        }
        cached=true; return CHRONOBENT_OK;
    }
    const float *at(int64_t frame) const { return data.data()+std::size_t(frame-origin)*channels; }
};
struct ClearBinding { Window &window; ~ClearBinding(){window.bind(nullptr,nullptr);} };
}

struct chronobent_tuner {
    chronobent_tune_config config;
    chronobent_tune_options options=chronobent_tune_default_options();
    uint64_t length;
    double analysis_rate,step;
    std::size_t hop,count,lag_min,lag_max,width,span,fft_size;
    Fft fft;
    Sinc downsample,interpolator;
    Window source;
    std::vector<Complex> a,b;
    std::vector<float> x,coefficients;
    std::vector<double> energy,difference,probabilities,median_values,smoothed,centers;
    std::vector<Candidate> candidates;
    std::vector<chronobent_tune_frame> frames;
    std::vector<chronobent_tune_note> notes;
    std::vector<Region> regions;
    std::vector<Mark> marks;
    std::vector<Grain> grains;
    std::array<float,quantum*2> original{},wet{};
    std::array<double,quantum> weight{};
    std::array<double,100> thresholds{};
    std::array<double,2> channel_energy{};
    bool ready=false,unchanged=true;

    chronobent_tuner(const chronobent_tune_config &c,uint64_t n):config(c),length(n),
        analysis_rate(std::min(12000.,c.sample_rate)),step(c.sample_rate/analysis_rate),
        hop(std::size_t(std::llround(c.sample_rate*.005))),count(std::size_t((n+hop-1)/hop)),
        lag_min(std::max<std::size_t>(2,std::size_t(std::floor(analysis_rate/c.maximum_hz)))),
        lag_max(std::size_t(std::ceil(analysis_rate/c.minimum_hz))),width(lag_max*2),
        span(width+lag_max+2),fft_size(power_two(span)),fft(fft_size),
        downsample(step),interpolator(1),
        source(std::size_t(std::ceil(c.sample_rate/c.minimum_hz*5))+Sinc::capacity(step)+4096,c.channels,n),
        a(fft_size),b(fft_size),x(span),coefficients(std::max(Sinc::capacity(step),Sinc::capacity(1))),
        energy(span+1),difference(lag_max+2),probabilities(lag_max+2),median_values(count),smoothed(count),
        centers(count),candidates(count*states),frames(count) {
        notes.reserve(count);regions.reserve(count);
        const auto mark_capacity=std::size_t(std::ceil(double(n)/c.sample_rate*c.maximum_hz*2))+count*2+16;
        marks.reserve(mark_capacity);grains.reserve(mark_capacity);
        downsample.prepare(step);interpolator.prepare(1);
        double total=0;
        for(std::size_t i=0;i<thresholds.size();++i) {
            const double t=(double(i)+.5)/100;
            thresholds[i]=t*std::pow(1-t,11);total+=thresholds[i];
        }
        for(auto &v:thresholds) v/=total;
    }
    bool progress(chronobent_tune_progress_fn fn,void *user,double fraction) const { return !fn || fn(user,fraction); }
    chronobent_status detect(std::size_t index) {
        const double center=double(std::min<uint64_t>(length-1,uint64_t(index*hop)));
        const double first=center-double(span)*step*.5;
        const int64_t start=int64_t(std::floor(first))-downsample.left();
        const auto n=std::size_t(std::ceil(double(span)*step))+downsample.taps()+2;
        auto status=source.load(start,n);if(status!=CHRONOBENT_OK) return status;
        unsigned channel=0;
        std::array<double,2> sums{},squares{};
        // Choose an individual channel; downmixing anti-phase stereo can erase F0.
        for(std::size_t i=0;i<span;++i) for(unsigned c=0;c<config.channels;++c) {
            const double value=source.at(int64_t(std::floor(first+double(i)*step)))[c];
            sums[c]+=value;squares[c]+=value*value;
        }
        for(unsigned c=0;c<config.channels;++c) {
            squares[c]=std::max(0.,squares[c]-sums[c]*sums[c]/double(span));channel_energy[c]+=squares[c];
            if(squares[c]>squares[channel]) channel=c;
        }
        const double rms=std::sqrt(squares[channel]/double(span));
        double mean=0;
        for(std::size_t i=0;i<span;++i) {
            const double position=first+double(i)*step;const auto at=int64_t(std::floor(position));
            float value=0;
            if(step==1) value=source.at(at)[channel];
            else {
                downsample.coefficients(position-double(at),coefficients.data());
                const auto input=source.at(at-downsample.left());
                for(std::size_t tap=0;tap<downsample.taps();++tap) value+=coefficients[tap]*input[tap*config.channels+channel];
            }
            x[i]=value;mean+=value;
        }
        mean/=double(span);std::fill(a.begin(),a.end(),Complex{});std::fill(b.begin(),b.end(),Complex{});energy[0]=0;
        for(std::size_t i=0;i<span;++i) {
            x[i]-=float(mean);b[i]=x[i];if(i<width)a[i]=x[i];
            energy[i+1]=energy[i]+double(x[i])*x[i];
        }
        fft.transform(a.data(),false);fft.transform(b.data(),false);
        for(std::size_t i=0;i<fft_size;++i)a[i]=std::conj(a[i])*b[i];
        fft.transform(a.data(),true);
        difference[0]=1;double cumulative=0;
        for(std::size_t lag=1;lag<=lag_max+1;++lag) {
            const double d=std::max(0.,energy[width]+energy[width+lag]-energy[lag]-2*double(a[lag].real()));
            cumulative+=d;difference[lag]=cumulative>tiny?d*double(lag)/cumulative:1;
        }
        std::fill(probabilities.begin(),probabilities.end(),0);
        if(rms>.001) for(std::size_t t=0;t<thresholds.size();++t) {
            const double threshold=(double(t)+.5)/100;
            for(std::size_t lag=lag_min;lag<=lag_max;++lag) {
                if(difference[lag]<threshold && difference[lag]<=difference[lag-1] && difference[lag]<difference[lag+1]) {
                    probabilities[lag]+=thresholds[t];break;
                }
            }
        }
        std::array<Candidate,states> current{};std::size_t used=1;
        double voiced_probability=0;
        for(std::size_t lag=lag_min;lag<=lag_max;++lag) if(probabilities[lag]>1e-6) {
            const double curvature=difference[lag-1]-2*difference[lag]+difference[lag+1];
            const double offset=std::abs(curvature)>tiny?clamp(.5*(difference[lag-1]-difference[lag+1])/curvature,-.5,.5):0;
            const double hz=analysis_rate/(double(lag)+offset);
            if(hz<config.minimum_hz || hz>config.maximum_hz) continue;
            voiced_probability+=probabilities[lag];
            Candidate value;value.hz=hz;value.probability=probabilities[lag];value.confidence=clamp(1-difference[lag],0,1);
            if(used<states) current[used++]=value;
            else {
                auto worst=std::min_element(current.begin()+1,current.end(),[](const Candidate &v,const Candidate &u){return v.probability<u.probability;});
                if(value.probability>worst->probability)*worst=value;
            }
        }
        current[0].probability=std::max(1e-6,1-voiced_probability);
        double minimum=1e30;
        for(std::size_t c=0;c<states;++c) {
            auto &value=current[c];if(c>=used)continue;
            const double emission=-std::log(std::max(1e-8,value.probability));
            if(!index) value.cost=emission+(c?1.2:0);
            else for(std::size_t p=0;p<states;++p) {
                const auto &previous=candidates[(index-1)*states+p];
                if(p && previous.hz<=0)continue;
                double transition=(bool(p)!=bool(c))?1.6:0;
                if(p && c) {const double distance=std::abs(midi(value.hz)-midi(previous.hz));transition=.28*distance+(distance>9?1.2:0);}
                const double cost=previous.cost+transition+emission;
                if(cost<value.cost) {value.cost=cost;value.previous=uint8_t(p);}
            }
            minimum=std::min(minimum,value.cost);
        }
        for(std::size_t c=0;c<states;++c) {current[c].cost-=minimum;candidates[index*states+c]=current[c];}
        frames[index]={uint64_t(center),0,0,0};return CHRONOBENT_OK;
    }
    void finish_track() {
        if(!count)return;
        std::size_t state=0;
        for(std::size_t c=1;c<states;++c) if(candidates[(count-1)*states+c].cost<candidates[(count-1)*states+state].cost)state=c;
        for(std::size_t i=count;i--;) {
            const auto &value=candidates[i*states+state];
            if(state && value.confidence>=.60) {frames[i].frequency_hz=value.hz;frames[i].confidence=value.confidence;}
            state=value.previous;
        }
        // Short isolated voiced islands are insufficient evidence for correction.
        for(std::size_t i=0;i<count;) {
            if(!frames[i].frequency_hz){++i;continue;}
            const auto start=i;while(i<count && frames[i].frequency_hz)++i;
            if((i-start)*hop<config.sample_rate*.035) {
                for(auto j=start;j<i;++j)frames[j].frequency_hz=frames[j].confidence=0;
                continue;
            }
            Region region;region.first=start;region.end=i;
            region.begin_frame=frames[start].source_frame;
            region.end_frame=std::min<uint64_t>(length,frames[i-1].source_frame+hop);
            regions.push_back(region);
        }
    }
    double median_pitch(std::size_t first,std::size_t end) {
        std::size_t n=0;for(std::size_t i=first;i<end;++i) if(frames[i].frequency_hz)median_values[n++]=midi(frames[i].frequency_hz);
        if(!n)return 0;
        std::nth_element(median_values.begin(),median_values.begin()+n/2,median_values.begin()+n);
        return median_values[n/2];
    }
    void add_note(std::size_t first,std::size_t end) {
        if(end<=first)return;
        const double center=median_pitch(first,end);double confidence=0;
        for(auto i=first;i<end;++i)confidence+=frames[i].confidence;
        centers[notes.size()]=center;
        notes.push_back({frames[first].source_frame,std::min<uint64_t>(length,frames[end-1].source_frame+hop),
            center,std::round(center),confidence/double(end-first),0});
    }
    void segment() {
        for(const auto &region:regions) {
            std::size_t first=region.first,pending=first;int proposed=0;
            double anchor=median_pitch(first,std::min(region.end,first+12));
            for(auto i=first;i<region.end;++i) {
                const double local=median_pitch(i>region.first+4?i-4:region.first,std::min(region.end,i+5));
                const int note=int(std::llround(local));
                if(std::abs(local-anchor)>.85 && note!=int(std::llround(anchor))) {
                    if(note!=proposed){proposed=note;pending=i;}
                    if(i-pending>=10 && pending-first>=8) {
                        add_note(first,pending);first=pending;anchor=local;proposed=0;
                    }
                } else {proposed=0;pending=i;}
            }
            add_note(first,region.end);
        }
    }
    double frequency(double position) const {
        if(!count)return 0;
        const auto i=std::min(count-1,std::size_t(std::max(0.,position)/double(hop)));
        const auto j=std::min(count-1,i+1);
        const double f=frames[i].frequency_hz,g=frames[j].frequency_hz;
        if(!f)return g;
        if(!g)return f;
        const double fraction=clamp((position-double(frames[i].source_frame))/double(hop),0,1);
        return std::exp(std::log(f)*(1-fraction)+std::log(g)*fraction);
    }
    double correction(double position) const {
        if(!count)return 0;
        const auto i=std::min(count-1,std::size_t(std::max(0.,position)/double(hop))),j=std::min(count-1,i+1);
        const double fraction=clamp((position-double(frames[i].source_frame))/double(hop),0,1);
        return frames[i].correction_st*(1-fraction)+frames[j].correction_st*fraction;
    }
    chronobent_status mark_source(chronobent_tune_progress_fn fn,void *user) {
        const unsigned channel=config.channels==2 && channel_energy[1]>channel_energy[0]?1:0;
        for(std::size_t r=0;r<regions.size();++r) {
            auto &region=regions[r];region.mark_first=marks.size();
            double period=config.sample_rate/frequency(double(region.begin_frame));
            double predicted=double(region.begin_frame)+period*.5;
            bool first=true;
            while(predicted<double(region.end_frame)) {
                const double radius=period*(first?.5:.22);
                const auto left=int64_t(std::floor(predicted-radius)),right=int64_t(std::ceil(predicted+radius));
                const auto status=source.load(left-2,std::size_t(right-left+5));if(status!=CHRONOBENT_OK)return status;
                // Consistent waveform maxima around a period prediction preserve
                // cycle identity. Refine sub-sample position around the best peak.
                int64_t best=left;double score=-1e30;
                for(int64_t p=left;p<=right;++p) {
                    const double distance=(double(p)-predicted)/period;
                    const double value=source.at(p)[channel];
                    const double candidate=value*(1-.12*distance*distance);
                    if(candidate>score){score=candidate;best=p;}
                }
                const double y0=source.at(best-1)[channel],y1=source.at(best)[channel],y2=source.at(best+1)[channel];
                const double denominator=y0-2*y1+y2;
                const double offset=std::abs(denominator)>tiny?clamp(.5*(y0-y2)/denominator,-.5,.5):0;
                double position=double(best)+offset;
                if(!first && position<=marks.back().position+period*.5)position=predicted;
                if(marks.size()==marks.capacity())return CHRONOBENT_INVALID_AUDIO;
                marks.push_back({position,period});first=false;
                period=config.sample_rate/frequency(position+period*.5);
                predicted=position+period;
                if((marks.size()&255)==0 && !progress(fn,user,.75+.25*position/double(std::max<uint64_t>(1,length))))return CHRONOBENT_CANCELLED;
            }
            region.mark_end=marks.size();
            for(auto i=region.mark_first;i<region.mark_end;++i) {
                if(i+1<region.mark_end)marks[i].period=marks[i+1].position-marks[i].position;
                else if(i>region.mark_first)marks[i].period=marks[i-1].period;
            }
        }
        return CHRONOBENT_OK;
    }
    double target(double center) const {
        double result=std::round(center),distance=1e30;
        for(int n=std::max(0,int(std::floor(center))-12);n<=std::min(127,int(std::ceil(center))+12);++n)
            if(options.scale_mask&(1u<<unsigned(n%12))) {
                const double d=std::abs(center-n);if(d<distance){distance=d;result=n;}
            }
        return result;
    }
    void plan() {
        unchanged=true;grains.clear();
        for(auto &f:frames)f.correction_st=0;
        const double reference_offset=12*std::log2(440/options.reference_hz);
        for(std::size_t n=0;n<notes.size();++n) {
            auto &note=notes[n];note.detected_midi=centers[n]+reference_offset;
            if(!note.manual)note.target_midi=target(note.detected_midi);
            const auto first=std::min(count,std::size_t(note.first_frame/hop));
            const auto end=std::min(count,std::size_t((note.end_frame+hop-1)/hop));
            for(auto i=first;i<end;++i) {
                double total=0,weights=0;
                const auto a0=i>30?std::max(first,i-30):first,b0=std::min(end,i+31);
                for(auto j=a0;j<b0;++j) {
                    const double distance=(double(i)-double(j))/10,weight0=std::exp(-.5*distance*distance);
                    total+=weight0*midi(frames[j].frequency_hz,options.reference_hz);weights+=weight0;
                }
                const double raw=midi(frames[i].frequency_hz,options.reference_hz),slow=total/weights;
                const double desired=(note.target_midi-note.detected_midi)+options.correct_drift*(note.detected_midi-slow)-
                    (1-options.preserve_vibrato)*(raw-slow);
                frames[i].correction_st=options.amount*clamp(desired,-options.maximum_shift,options.maximum_shift);
            }
        }
        const double alpha=options.retune_ms>0?1-std::exp(-double(hop)/config.sample_rate/(options.retune_ms*.001)):1;
        for(std::size_t r=0;r<regions.size();++r) {
            auto &region=regions[r];double current=frames[region.first].correction_st;region.active=false;
            for(auto i=region.first;i<region.end;++i) {
                current+=alpha*(frames[i].correction_st-current);frames[i].correction_st=current;
                if(std::abs(current)>1e-7)region.active=true;
            }
            if(!region.active || region.mark_end-region.mark_first<3) {region.active=false;continue;}
            unchanged=false;std::size_t mark=region.mark_first;
            double position=marks[mark].position;
            while(position<double(region.end_frame)) {
                while(mark+1<region.mark_end && std::abs(marks[mark+1].position-position)<std::abs(marks[mark].position-position))++mark;
                const double observed=config.sample_rate/marks[mark].period;
                const double tracked=frequency(position);
                const double hz=std::exp(std::log(observed)*options.preserve_vibrato+std::log(tracked)*(1-options.preserve_vibrato));
                const double output_period=config.sample_rate/(hz*std::exp2(correction(position)/12));
                if(grains.size()==grains.capacity()) {region.active=false;break;}
                grains.push_back({position,marks[mark].position,marks[mark].period,r});
                position+=std::max(config.sample_rate/(config.maximum_hz*2),output_period);
            }
        }
    }
    chronobent_status render_block(uint64_t first,float *output,std::size_t n) {
        auto status=source.load(int64_t(first),n);if(status!=CHRONOBENT_OK)return status;
        const auto channels=config.channels;std::copy_n(source.at(int64_t(first)),n*channels,original.data());
        if(unchanged) {std::copy_n(original.data(),n*channels,output);return CHRONOBENT_OK;}
        std::fill(wet.begin(),wet.end(),0.0f);std::fill(weight.begin(),weight.end(),0);
        const double maximum_radius=config.sample_rate/config.minimum_hz*1.6;
        auto grain=std::lower_bound(grains.begin(),grains.end(),double(first)-maximum_radius,
            [](const Grain &g,double position){return g.position<position;});
        for(;grain!=grains.end() && grain->position<double(first+n)+maximum_radius;++grain) {
            const auto &g=*grain;if(!regions[g.region].active)continue;
            const auto begin=std::max<int64_t>(int64_t(first),int64_t(std::ceil(g.position-g.radius)));
            const auto end=std::min<int64_t>(int64_t(first+n),int64_t(std::ceil(g.position+g.radius)));
            if(end<=begin)continue;
            const double delta=g.source-g.position;const auto shift=int64_t(std::floor(delta));
            status=source.load(begin+shift-interpolator.left(),std::size_t(end-begin)+interpolator.taps());
            if(status!=CHRONOBENT_OK)return status;
            interpolator.coefficients(delta-double(shift),coefficients.data());
            for(auto at=begin;at<end;++at) {
                const auto destination=std::size_t(at-int64_t(first));
                const double window=.5+.5*std::cos(pi*(double(at)-g.position)/g.radius);
                const auto input=source.at(at+shift-interpolator.left());
                for(unsigned c=0;c<channels;++c) {
                    float value=0;for(std::size_t tap=0;tap<interpolator.taps();++tap)value+=coefficients[tap]*input[tap*channels+c];
                    wet[destination*channels+c]+=float(window)*value;
                }
                weight[destination]+=window;
            }
        }
        auto region=std::lower_bound(regions.begin(),regions.end(),first,
            [](const Region &r,uint64_t position){return r.end_frame<=position;});
        for(std::size_t i=0;i<n;++i) {
            const auto at=first+i;while(region!=regions.end() && region->end_frame<=at)++region;
            double mix=0;
            if(region!=regions.end() && region->active && at>=region->begin_frame && weight[i]>.05) {
                const double fade=std::min(double(at-region->begin_frame),double(region->end_frame-at))/std::max(1.,config.sample_rate*.015);
                mix=clamp(fade,0,1);mix=mix*mix*(3-2*mix);
            }
            for(unsigned c=0;c<channels;++c) {
                const auto slot=i*channels+c;
                output[slot]=mix>0?float(double(original[slot])*(1-mix)+double(wet[slot])/weight[i]*mix):original[slot];
            }
        }
        return CHRONOBENT_OK;
    }
};

extern "C" chronobent_tune_config chronobent_tune_default_config(double rate,uint32_t channels) {return {rate,channels,55,1200};}
extern "C" chronobent_tune_options chronobent_tune_default_options(void) {return {4095,440,1,80,1,0,2};}
extern "C" chronobent_status chronobent_tune_validate_options(const chronobent_tune_options *options) {
    return options && valid(*options)?CHRONOBENT_OK:CHRONOBENT_INVALID_ARGUMENT;
}
extern "C" chronobent_status chronobent_tune_create(const chronobent_tune_config *config,uint64_t frames,chronobent_tuner **out) {
    if(!out)return CHRONOBENT_INVALID_ARGUMENT;
    *out=nullptr;
    if(!config || !(config->sample_rate>=8000 && config->sample_rate<=192000) || config->channels<1 || config->channels>2 ||
       !(config->minimum_hz>=40 && config->minimum_hz<=400) || !(config->maximum_hz>config->minimum_hz && config->maximum_hz<=1600) ||
       frames>uint64_t(config->sample_rate*600))return CHRONOBENT_INVALID_ARGUMENT;
    try {*out=new chronobent_tuner(*config,frames);return CHRONOBENT_OK;}catch(...){return CHRONOBENT_OUT_OF_MEMORY;}
}
extern "C" void chronobent_tune_destroy(chronobent_tuner *tuner){delete tuner;}
extern "C" chronobent_status chronobent_tune_analyze(chronobent_tuner *t,chronobent_read_fn reader,void *source,
    chronobent_tune_progress_fn progress,void *user) {
    if(!t || (t->length && !reader))return CHRONOBENT_INVALID_ARGUMENT;
    t->ready=false;t->notes.clear();t->regions.clear();t->marks.clear();t->grains.clear();t->channel_energy={};t->source.bind(reader,source);ClearBinding binding{t->source};
    if(!t->progress(progress,user,0))return CHRONOBENT_CANCELLED;
    for(std::size_t i=0;i<t->count;++i) {
        const auto status=t->detect(i);if(status!=CHRONOBENT_OK)return status;
        if((i&15)==0 && !t->progress(progress,user,.75*double(i)/double(t->count)))return CHRONOBENT_CANCELLED;
    }
    t->finish_track();t->segment();const auto status=t->mark_source(progress,user);if(status!=CHRONOBENT_OK)return status;
    t->plan();if(!t->progress(progress,user,1))return CHRONOBENT_CANCELLED;t->ready=true;return CHRONOBENT_OK;
}
extern "C" const chronobent_tune_frame *chronobent_tune_frames(const chronobent_tuner *t,size_t *count) {
    if(count)*count=t && t->ready?t->frames.size():0;
    return t && t->ready && !t->frames.empty()?t->frames.data():nullptr;
}
extern "C" const chronobent_tune_note *chronobent_tune_notes(const chronobent_tuner *t,size_t *count) {
    if(count)*count=t && t->ready?t->notes.size():0;
    return t && t->ready && !t->notes.empty()?t->notes.data():nullptr;
}
extern "C" chronobent_status chronobent_tune_set_options(chronobent_tuner *t,const chronobent_tune_options *p) {
    if(!t || !p || !valid(*p))return CHRONOBENT_INVALID_ARGUMENT;
    t->options=*p;if(t->ready)t->plan();return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_tune_get_options(const chronobent_tuner *t,chronobent_tune_options *p) {
    if(!t || !p)return CHRONOBENT_INVALID_ARGUMENT;
    *p=t->options;return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_tune_set_note(chronobent_tuner *t,size_t index,double target) {
    if(!t || !std::isfinite(target))return CHRONOBENT_INVALID_ARGUMENT;
    if(!t->ready)return CHRONOBENT_NOT_RESET;
    if(index>=t->notes.size() || (target!=-1 && (!(target>=0 && target<=127) || std::abs(target-t->notes[index].detected_midi)>5)))return CHRONOBENT_INVALID_ARGUMENT;
    auto &note=t->notes[index];note.manual=target!=-1;if(note.manual)note.target_midi=target;t->plan();return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_tune_set_notes(chronobent_tuner *t,const chronobent_tune_edit *edits,size_t count) {
    if(!t || (count && !edits))return CHRONOBENT_INVALID_ARGUMENT;
    if(!t->ready)return CHRONOBENT_NOT_RESET;
    if(count>t->notes.size())return CHRONOBENT_INVALID_ARGUMENT;
    for(size_t i=0;i<count;++i) {
        const auto &e=edits[i];
        if(e.note_index>=t->notes.size() || (i && e.note_index<=edits[i-1].note_index) ||
            !(e.target_midi>=0 && e.target_midi<=127) || std::abs(e.target_midi-t->notes[e.note_index].detected_midi)>5)
            return CHRONOBENT_INVALID_ARGUMENT;
    }
    for(auto &note:t->notes)note.manual=0;
    for(size_t i=0;i<count;++i){auto &note=t->notes[edits[i].note_index];note.manual=1;note.target_midi=edits[i].target_midi;}
    t->plan();return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_tune_render(chronobent_tuner *t,chronobent_read_fn reader,void *source,
    uint64_t first,float *output,size_t frames,size_t *produced) {
    if(produced)*produced=0;
    if(!t || !produced || first>t->length || frames>std::numeric_limits<size_t>::max()/t->config.channels/sizeof(float) ||
       (frames && (!output || !reader)))return CHRONOBENT_INVALID_ARGUMENT;
    if(!t->ready)return CHRONOBENT_NOT_RESET;
    const auto amount=std::size_t(std::min<uint64_t>(frames,t->length-first));t->source.bind(reader,source);ClearBinding binding{t->source};
    while(*produced<amount) {
        const auto n=std::min(quantum,amount-*produced);
        const auto status=t->render_block(first+*produced,output+*produced*t->config.channels,n);if(status!=CHRONOBENT_OK)return status;
        *produced+=n;
    }
    return first+*produced==t->length?CHRONOBENT_END:CHRONOBENT_OK;
}
