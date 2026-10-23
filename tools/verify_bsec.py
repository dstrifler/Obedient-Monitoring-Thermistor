#!/usr/bin/env python3
"""Check the stock Bosch release/configuration used by this firmware."""
import hashlib
from pathlib import Path
import sys

root = Path(sys.argv[1])
expected = {
    'library.properties': '269cc7a2061ffd1136f474c5533b84747a6128943e5ff9562f0cbca7492e6861',
    'src/inc/bsec_interface.h': '7643d4dbc0958c9403d5477eb73f9453e54cf69d3ba99cab1c877a0cd4570ae1',
    'src/inc/bsec_datatypes.h': 'a632c78ca4e38ad7773fe3493ecb2ff2054a0a5eec429c091db9bfe7ed55f927',
    'src/cortex-m4/fpv4-sp-d16-hard/libalgobsec.a': 'ad831fcb676e7cafd491542b2277b773113ce5f92ec022945bebb53dd8595650',
    'src/config/bme688/bme688_sel_33v_3s_4d/bsec_selectivity.txt': '65b0ea03a890d06445603e015cd085198d00f114568099fe251f0b4d232a4711',
}
for name, digest in expected.items():
    path = root / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit('Install unmodified Bosch bsec2 1.10.2610; missing/mismatched: ' + str(path))
print('PASS: stock BSEC 2.6.1.0 hard-float binary, headers and 3.3 V LP profile')
