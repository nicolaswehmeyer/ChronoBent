#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nicolas Wehmeyer
"""Generate a diagnostic pitch/tempo matrix. Requires NumPy, no external audio.

Metrics describe specific numerical properties, not a perceptual quality score.
The new output directory retains inputs, renders, exact commands and hashes.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import struct
import subprocess
import sys

import numpy as np
from measure_quality import read_float_wav


def write_wav(path, samples, rate):
    pcm = np.asarray(samples, dtype='<f4')
    channels = 1 if pcm.ndim == 1 else pcm.shape[1]
    data = pcm.tobytes()
    fmt = struct.pack('<HHIIHH', 3, channels, rate, rate*channels*4, channels*4, 32)
    path.write_bytes(b'RIFF'+struct.pack('<I', 36+len(data))+b'WAVEfmt '+struct.pack('<I', 16)+fmt+b'data'+struct.pack('<I', len(data))+data)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def basic(samples):
    if not np.isfinite(samples).all():
        raise ValueError('Nonfinite rendered sample')
    return {'frames': len(samples), 'peak': float(np.max(np.abs(samples))),
            'rms': float(np.sqrt(np.mean(samples.astype(np.float64)**2))),
            'samples_above_unity': int(np.count_nonzero(np.abs(samples)>1))}


def tone_metrics(samples, rate, expected):
    middle = np.asarray(samples[len(samples)//4:len(samples)*3//4], dtype=np.float64)
    indices = np.flatnonzero((middle[:-1]<=0) & (middle[1:]>0))
    crossings = indices-middle[indices]/(middle[indices+1]-middle[indices])
    if len(crossings)<3:
        raise ValueError('Too few tone crossings')
    frequency = (len(crossings)-1)*rate/(crossings[-1]-crossings[0])
    # Crossing interpolation is biased near Nyquist. Refine the tone fit inside
    # the main spectral lobe before calling its residual an extra signal.
    time = (np.arange(len(middle))-len(middle)/2)/rate
    energy_squared = float(np.sum(middle*middle))
    def fit(hz):
        c = np.cos(2*np.pi*hz*time)
        s = np.sin(2*np.pi*hz*time)
        cc, ss, cs = np.sum(c*c), np.sum(s*s), np.sum(c*s)
        xc, xs = np.sum(middle*c), np.sum(middle*s)
        denominator = cc*ss-cs*cs
        a, b = (xc*ss-xs*cs)/denominator, (xs*cc-xc*cs)/denominator
        return float(np.sum((middle-a*c-b*s)**2))
    low, high = frequency-rate/len(middle)/2, frequency+rate/len(middle)/2
    golden = (math.sqrt(5)-1)/2
    x1, x2 = high-golden*(high-low), low+golden*(high-low)
    y1, y2 = fit(x1), fit(x2)
    for _ in range(36):
        if y1 < y2:
            high, x2, y2 = x2, x1, y1
            x1 = high-golden*(high-low)
            y1 = fit(x1)
        else:
            low, x1, y1 = x1, x2, y2
            x2 = low+golden*(high-low)
            y2 = fit(x2)
    fitted_frequency = (low+high)/2
    residual = math.sqrt(fit(fitted_frequency)/max(energy_squared, 1e-30))
    return {'cents_error': 1200*math.log2(fitted_frequency/expected),
            'crossing_cents_error': 1200*math.log2(frequency/expected),
            'fitted_tone_residual_db': 20*math.log10(max(residual, 1e-20))}



def measure(renderer, directory, quick):
    directory.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'synthetic numerical diagnostics; no listening or device qualification',
              'renderer_sha256': digest(renderer), 'platform': platform.platform(),
              'python': platform.python_version(), 'numpy': np.__version__,
              'completed': False, 'renders': [], 'failures': []}
    def render(name, source, rate, tempo, pitch, mode='transients', formant='shift', envelope=2, profile='balanced'):
        source_path = directory/(name+'-input.wav')
        target = directory/(name+'.wav')
        write_wav(source_path, source, rate)
        command = [str(renderer), str(source_path), str(target), str(tempo), str(pitch), mode, str(formant), str(envelope), profile]
        run = subprocess.run(command, capture_output=True, text=True)
        (directory/(name+'.log')).write_text(run.stdout+run.stderr)
        if run.returncode:
            raise RuntimeError(f'{name}: renderer exited {run.returncode}')
        channels = 1 if source.ndim == 1 else source.shape[1]
        data = np.asarray(read_float_wav(target)).reshape(-1, channels)
        row = dict(name=name, sample_rate=rate, channels=channels, tempo=tempo, pitch=pitch,
                   transient_mode=mode, formant_scale=formant, envelope_ms=envelope, profile=profile,
                   input_sha256=digest(source_path), output_sha256=digest(target), command=command,
                   **basic(data))
        report['renders'].append(row)
        if len(data) != math.ceil(len(source)/tempo):
            report['failures'].append({'name': name, 'reason': 'duration'})
        return data, row
    try:
        ratios = [(1, 2**(st/12)) for st in (-12, -5, 5, 12)]
        ratios += [(t, 1) for t in (.25, .5, .8, 1.25, 2, 4)]
        ratios += [(t, p) for t in (.25, 4) for p in (.5, 2)]
        if quick:
            ratios = [(1,.5), (1,2), (.5,1), (2,1)]
        for rate in ([48000] if quick else [44100, 48000, 96000]):
            time = np.arange(rate*2)/rate
            for hz in ([110, 997] if quick else [40, 997, 9000]):
                source = .2*np.sin(2*np.pi*hz*time)
                for i, (tempo, pitch) in enumerate(ratios):
                    data, row = render(f'tone-{rate}-{hz}-{i}', source, rate, tempo, pitch)
                    row.update(tone_metrics(data[:,0], rate, hz*pitch))
                    # A broad admission bound; tighter established 997 Hz gates
                    # remain in measure_quality.py. Residual energy is reported.
                    if abs(row['cents_error']) > 2:
                        report['failures'].append({'name': row['name'], 'reason': 'tone error exceeds 2 cents'})
        rate = 48000
        time = np.arange(rate*2)/rate
        rng = np.random.default_rng(20260910)
        noise = rng.uniform(-1, 1, len(time))
        mix = .1*np.sin(2*np.pi*110*time)+.08*np.sin(2*np.pi*329.63*time)+.15*noise*np.exp(-40*(time%.4))
        stereo = np.stack((mix, -mix), axis=1)
        for mode in ['tonal', 'transients', 'mixed']:
            for profile in (['balanced'] if quick else ['compact', 'balanced', 'detailed']):
                for tempo, pitch in [(0.5,1), (1,2), (2,.5)]:
                    data, row = render(f'stereo-{mode}-{profile}-{tempo}-{pitch}', stereo, rate, tempo, pitch, mode, 1.25, 2, profile)
                    row['opposite_phase_residual_peak'] = float(np.max(np.abs(data[:,0]+data[:,1])))
                    if row['opposite_phase_residual_peak'] > 1e-6:
                        report['failures'].append({'name': row['name'], 'reason': 'stereo phase relation'})
        # A known source-filter model, with independently moved formants.
        harmonics = np.arange(1, 49)
        def envelope(f):
            return .01+np.exp(-.5*((f-700)/140)**2)+.7*np.exp(-.5*((f-1250)/180)**2)+.4*np.exp(-.5*((f-2600)/250)**2)
        vowel = np.sum(envelope(harmonics[:,None]*100)*np.sin(2*np.pi*harmonics[:,None]*100*time+harmonics[:,None]*.7)/16, axis=0)
        for pitch in ([1] if quick else [.5, 1, 2]):
            for scale in [.75, 1, 1.5]:
                data, row = render(f'formant-{pitch}-{scale}', vowel, rate, 1, pitch, 'tonal', scale)
                selected = harmonics[(harmonics*100*pitch>250) & (harmonics*100*pitch<3500)]
                middle = data[len(data)//4:len(data)*3//4,0]
                phases = 2*np.pi*selected[:,None]*100*pitch*time[len(data)//4:len(data)*3//4]
                amplitudes = np.abs(2*np.mean(middle*np.exp(-1j*phases), axis=1))
                wanted = envelope(selected*100*pitch/scale)/16
                row['model_log_envelope_error_db'] = float(np.sqrt(np.mean((20*np.log10(np.maximum(amplitudes, 1e-7)/wanted))**2)))
        report['completed'] = True
    finally:
        (directory/'report.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(f"{len(report['renders'])} renders; {len(report['failures'])} contract failures. See {directory/'report.json'}")
    return bool(report['failures'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer', type=Path)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--quick', action='store_true')
    args = parser.parse_args()
    sys.exit(measure(args.renderer.resolve(), args.directory.resolve(), args.quick))
