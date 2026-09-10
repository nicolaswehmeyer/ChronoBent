#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nicolas Wehmeyer
"""Fetch pinned plugin build dependencies into a new, separate local directory."""
import argparse
from pathlib import Path
import subprocess

PINS = {
    'iPlug2': ('https://github.com/iPlug2/iPlug2.git', 'd54f69050f517e43b941d88c2a170f0a840b9ee4'),
    'vst3sdk': ('https://github.com/steinbergmedia/vst3sdk.git', '3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    destination = args.directory.resolve()
    source = Path(__file__).resolve().parents[1]
    if destination == source or destination.is_relative_to(source):
        parser.error('Keep third-party checkouts outside the ChronoBent source tree')
    destination.mkdir(parents=True, exist_ok=False)
    for name, (url, revision) in PINS.items():
        checkout = destination / name
        subprocess.run(['git', 'init', str(checkout)], check=True)
        subprocess.run(['git', '-C', str(checkout), 'remote', 'add', 'origin', url], check=True)
        subprocess.run(['git', '-C', str(checkout), 'fetch', '--depth', '1', 'origin', revision], check=True)
        subprocess.run(['git', '-C', str(checkout), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
        actual = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise RuntimeError('Dependency revision mismatch')
        if name == 'vst3sdk':
            subprocess.run(['git', '-C', str(checkout), 'submodule', 'update', '--init', '--recursive', '--depth', '1'], check=True)
    link = destination / 'iPlug2/Dependencies/IPlug/VST3_SDK'
    if link.exists():
        link.rename(link.with_name('VST3_SDK.placeholder'))
    link.symlink_to(destination / 'vst3sdk', target_is_directory=True)
    print('Pinned external dependencies ready. IPLUG2_DIR=' + str(destination / 'iPlug2'))


if __name__ == '__main__':
    main()
