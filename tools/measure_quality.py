#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nicolas Wehmeyer
"""Reproducible synthetic quality measurements; no music or third-party data.

Python standard library only. Metrics are diagnostics, not perceptual ratings.
Write JSON to a new path; source/audio and renderer logs live in a temporary dir.
"""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 48000


def read_float_wav(path):
    data = path.read_bytes()
    offset = 12
    while offset + 8 <= len(data):
        name, size = struct.unpack_from('<4sI', data, offset)
        offset += 8
        if name == b'data':
            result = array.array('f')
            result.frombytes(data[offset:offset+size])
            if sys.byteorder != 'little':
                result.byteswap()
            return result
        offset += size + (size & 1)
    raise ValueError('No WAV data')


def measure(renderer, output):
    results = {'scope': 'synthetic mono, 48 kHz; no listening or device qualification', 'tones': [], 'attacks': [], 'renders': {}}
    with tempfile.TemporaryDirectory(prefix='chronobent-quality-') as temporary:
        root = Path(temporary)
        def render(name, values, tempo, pitch):
            source, target = root/(name+'.wav'), root/(name+'-out.wav')
            pcm = array.array('h', (round(max(-1, min(1, v))*32767) for v in values))
            if sys.byteorder != 'little':
                pcm.byteswap()
            with wave.open(str(source), 'wb') as file:
                file.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
                file.writeframes(pcm.tobytes())
            subprocess.run([str(renderer), str(source), str(target), str(tempo), str(pitch)], check=True, capture_output=True)
            results['renders'][name] = hashlib.sha256(target.read_bytes()).hexdigest()
            return read_float_wav(target)
        ratios = [(1, 2**(st/12)) for st in (-12, -5, -1, 1, 5, 12)] + [(t, 1) for t in (0.5, 0.8, 1.25, 2)]
        for n, (tempo, pitch) in enumerate(ratios):
            audio = render('tone'+str(n), (0.25*math.sin(2*math.pi*997*i/RATE) for i in range(RATE*2)), tempo, pitch)
            assert len(audio) == math.ceil(RATE*2/tempo)
            middle = audio[RATE//4:len(audio)-RATE//4]
            crossings = [i-1-middle[i-1]/(middle[i]-middle[i-1]) for i in range(1,len(middle)) if middle[i-1]<=0<middle[i]]
            frequency = (len(crossings)-1)*RATE/(crossings[-1]-crossings[0])
            cents = 1200*math.log2(frequency/(997*pitch))
            # Fit quadrature amplitude in 40 ms windows; report its variation.
            amplitudes=[]
            for first in range(0,len(middle)-1920,960):
                real=imag=weight=0.0
                for i in range(1920):
                    w=0.5-0.5*math.cos(2*math.pi*i/1920)
                    phase=2*math.pi*frequency*(first+i)/RATE
                    real+=w*middle[first+i]*math.cos(phase)
                    imag+=w*middle[first+i]*math.sin(phase)
                    weight+=w
                amplitudes.append(2*math.hypot(real,imag)/weight)
            modulation=20*math.log10(max(amplitudes)/min(amplitudes))
            results['tones'].append({'tempo':tempo,'pitch':pitch,'frames':len(audio),'cents_error':cents,'amplitude_modulation_db':modulation})
            # Established synthetic contracts, deliberately no audibility claim.
            assert abs(cents)<0.1, ('frequency',tempo,pitch,cents)
            assert modulation<0.5, ('amplitude modulation',tempo,pitch,modulation)
            click=[0.0]*(RATE*2)
            source_at=RATE+137
            click[source_at]=0.5
            rendered=render('attack'+str(n),click,tempo,pitch)
            expected=source_at/tempo
            center=round(expected)
            peak=max(range(center-240,center+241),key=lambda i:abs(rendered[i]))
            maximum=abs(rendered[peak])
            pre=max(abs(x) for x in rendered[max(0,peak-2400):peak-96])
            post=max(abs(x) for x in rendered[peak+97:peak+2401])
            db=lambda x:20*math.log10(max(x/maximum,1e-15))
            results['attacks'].append({'tempo':tempo,'pitch':pitch,'peak_error_frames':peak-expected,'pre_echo_peak_db_outside_2ms':db(pre),'post_echo_peak_db_outside_2ms':db(post)})
            assert abs(peak-expected)<=2, ('attack timing',tempo,pitch)
        # Broader deterministic stress signals for byte-exact comparisons.
        # Their energy is a boundedness diagnostic, not a sound-quality score.
        state=29
        noise=[]
        low=0.0
        for i in range(RATE*2):
            state=(1664525*state+1013904223)&0xffffffff
            white=state/4294967296-0.5
            low=0.98*low+0.02*white
            noise.append(0.3*white+low)
        sweep=[0.2*math.sin(2*math.pi*30*2/math.log(500)*(math.exp(math.log(500)*i/(RATE*2))-1)) for i in range(RATE*2)]
        mix=[noise[i]*math.exp(-50*((i/RATE)%0.25))+sum(0.08*math.sin(2*math.pi*f*i/RATE) for f in (110,164.81,220,277.18)) for i in range(RATE*2)]
        results['stress']=[]
        for name, values in [('noise',noise),('sweep',sweep),('mixture',mix)]:
            for n,(tempo,pitch) in enumerate([(0.73,2**(-5/12)),(1.25,1),(1,2**(5/12))]):
                audio=render(name+str(n),values,tempo,pitch)
                assert len(audio)==math.ceil(len(values)/tempo)
                assert all(math.isfinite(v) for v in audio)
                rms=math.sqrt(sum(v*v for v in audio)/len(audio))
                assert 0.001<rms<1, (name,tempo,pitch,rms)
                results['stress'].append({'signal':name,'tempo':tempo,'pitch':pitch,'rms':rms})
    with output.open('x') as file:
        json.dump(results,file,indent=2,allow_nan=False)
        file.write('\n')
    print('Synthetic frequency, amplitude stability, duration and attack timing passed; diagnostics:',output)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer',type=Path)
    parser.add_argument('output',type=Path,nargs='?')
    args=parser.parse_args()
    if args.output is not None:
        measure(args.renderer.resolve(),args.output)
    else:
        with tempfile.TemporaryDirectory(prefix="chronobent-metrics-") as temporary:
            measure(args.renderer.resolve(),Path(temporary)/"quality.json")
