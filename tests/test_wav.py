# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nicolas Wehmeyer
"""Independent RIFF fixtures for the command-line example; standard library only."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

EXE = sys.argv[1]


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + b'\0' * (len(data) % 2)


def riff(*chunks):
    body = b'WAVE' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def fmt(bits, code=1):
    return chunk(b'fmt ', struct.pack('<HHIIHH', code, 1, 48000, 48000*bits//8, bits//8, bits))


with tempfile.TemporaryDirectory(prefix='chronobent-wav-') as directory:
    root = Path(directory)
    source, dest = root / 'input.wav', root / 'output.wav'
    for bits, code in ((16, 1), (24, 1), (32, 1), (32, 3)):
        expected = [-1.0, -0.5, 0.0, 0.25, 0.5] * 30
        if code == 3:
            pcm = struct.pack('<'+'f'*len(expected), *expected)
        else:
            pcm = b''.join(int(v*(1 << (bits-1))).to_bytes(bits//8, 'little', signed=True) for v in expected)
        source.write_bytes(riff(chunk(b'JUNK', b'odd'), fmt(bits, code), chunk(b'data', pcm)))
        result = subprocess.run([EXE, str(source), str(dest), '1', '1'], capture_output=True)
        assert result.returncode == 0, result.stderr
        output = dest.read_bytes()
        assert output[:4] == b'RIFF' and struct.unpack_from('<I', output, 4)[0]+8 == len(output)
        assert output[48:52] == b'data'
        assert list(struct.unpack_from('<'+'f'*len(expected), output, 56)) == expected
        before = output
        assert subprocess.run([EXE, str(source), str(dest), '1', '1'], capture_output=True).returncode != 0
        assert dest.read_bytes() == before, 'existing output was overwritten'
        dest.unlink()
    valid = riff(fmt(16), chunk(b'data', b'\0\0'))
    malformed = [b'', b'RIFF', valid[:-1], valid+b'junk',
                 riff(fmt(16), fmt(16), chunk(b'data', b'\0\0')),
                 riff(fmt(16), chunk(b'data', b'x')),
                 riff(fmt(32, 3), chunk(b'data', struct.pack('<f', float('nan')))),
                 riff(fmt(32, 3), chunk(b'data', struct.pack('<f', float('inf')))),
                 riff(fmt(16), b'data'+struct.pack('<I', 0xffffffff)),
                 riff(chunk(b'data', b'\0\0'))]
    for data in malformed:
        source.write_bytes(data)
        result = subprocess.run([EXE, str(source), str(dest), '1', '1'], capture_output=True)
        assert result.returncode != 0, 'malformed input accepted'
        assert not dest.exists(), 'rejected input created output'
    source.write_bytes(valid)
    for tempo, pitch in [('nan', '1'), ('0', '1'), ('1', '16.1'), ('1junk', '1')]:
        assert subprocess.run([EXE, str(source), str(dest), tempo, pitch], capture_output=True).returncode != 0
        assert not dest.exists()
print('WAV: exact PCM16/24/32/float, odd chunks, malformed input and exclusive output passed')
