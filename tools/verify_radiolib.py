#!/usr/bin/env python3
"""Verify the stock RadioLib version and formerly patched files without editing them."""
import argparse
import hashlib
from pathlib import Path

UPSTREAM_HASHES = {
    'src/RadioLib.h': '33dd4e67c3a45df9c1311c4aa03b70a9f9e73a817a1022e2ee03ffa8eef1552b',
    'src/protocols/LoRaWAN/LoRaWAN.cpp': '99ecfbef09677232b27a36c42326e27d96786e4111e40696316e1ea1ecb30c61',
}


def verify(upstream):
    if 'version=7.1.2' not in (upstream / 'library.properties').read_text().splitlines():
        raise ValueError('RadioLib 7.1.2 is required; review firmware compatibility before upgrading')
    for relative, expected in UPSTREAM_HASHES.items():
        actual = hashlib.sha256((upstream / relative).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError('Expected unmodified RadioLib 7.1.2: ' + relative)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('upstream', type=Path)
    args = parser.parse_args()
    verify(args.upstream)
    print('Verified unmodified RadioLib 7.1.2; no dependency files changed')
