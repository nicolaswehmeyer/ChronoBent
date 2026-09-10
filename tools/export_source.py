#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nicolas Wehmeyer
"""Export reviewed sources and exact owned artwork, without VCS or build files."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REVIEWED_IMAGES = {
    'docs/images/chronobent-0.5-hero.png':
        '827cd2f568d294b173be7f762ae5fb8d558b50c8e5053ed4c43554d93784c4c2',
}


def export(destination, scanner):
    names = (ROOT / 'export-files.txt').read_text(encoding='utf-8').splitlines()
    if names != sorted(set(names)) or not names:
        raise ValueError('Export inventory must be nonempty, sorted and unique')
    with tempfile.TemporaryDirectory(prefix='chronobent-export-') as temporary:
        temp = Path(temporary)
        package = temp / 'chronobent'
        package.mkdir()
        contents = {}
        for name in names:
            relative = PurePosixPath(name)
            if (str(relative) != name or relative.is_absolute() or
                    (any(part.startswith('.') for part in relative.parts) and name not in {'.gitignore', '.github/workflows/ci.yml'}) or
                    any(part in {'build', '__pycache__'} for part in relative.parts) or
                    (relative.suffix not in {'.h', '.hpp', '.c', '.cpp', '.mm', '.py', '.md', '.txt', '.mk', '.in', '.yml', '.html', '.css', '.js', '.plist', ''} and name not in REVIEWED_IMAGES)):
                raise ValueError(f'Invalid source inventory path: {name}')
            source = ROOT / name
            if source.is_symlink() or not source.is_file() or not source.resolve().is_relative_to(ROOT):
                raise ValueError(f'Not a regular contained source file: {name}')
            data = source.read_bytes()
            if name in REVIEWED_IMAGES:
                if (len(data) > 8 * 1024 * 1024 or not data.startswith(b'\x89PNG\r\n\x1a\n') or
                        hashlib.sha256(data).hexdigest() != REVIEWED_IMAGES[name]):
                    raise ValueError(f'Unreviewed image bytes: {name}')
            else:
                if len(data) > 1000000 or b'\0' in data:
                    raise ValueError(f'Non-text or oversized source: {name}')
                data.decode('utf-8')
            target = package / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            contents[name] = data
        # Force built-in rules and empty exclusions, independent of environment,
        # local ignore files or inline suppressions. No scan artifacts are packed.
        config = temp / 'scanner.toml'
        config.write_text('[extend]\nuseDefault = true\n')
        ignore = temp / 'empty-ignore'
        ignore.write_text('')
        env = {key: value for key, value in os.environ.items() if not key.startswith('GITLEAKS_')}
        subprocess.run([scanner, 'dir', str(package), '--redact=100', '--no-banner',
                        '--ignore-gitleaks-allow', '--config', str(config),
                        '--gitleaks-ignore-path', str(ignore)], cwd=temp, env=env, check=True)
        # Exclusive creation prevents overwriting an earlier source export.
        with destination.open('xb') as stream:
            with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_STORED) as archive:
                for name, data in contents.items():
                    entry = zipfile.ZipInfo('chronobent/' + name, (1980, 1, 1, 0, 0, 0))
                    entry.create_system = 3
                    entry.external_attr = 0o100644 << 16
                    archive.writestr(entry, data)
    print(f'{len(names)} source files: {destination}')
    print('SHA-256: ' + hashlib.sha256(destination.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scanner', default=shutil.which('gitleaks'))
    args = parser.parse_args()
    if not args.scanner:
        parser.error('Gitleaks is required; install it or supply --scanner /path/to/gitleaks')
    executable = shutil.which(args.scanner)
    if not executable:
        parser.error('The selected Gitleaks executable is unavailable')
    export(args.output, str(Path(executable).resolve()))


if __name__ == '__main__':
    main()
