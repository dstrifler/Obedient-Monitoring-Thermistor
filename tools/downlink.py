#!/usr/bin/env python3
"""Create Base64 application commands for Helium; never sends a command."""
import argparse
import base64
import json

COMMANDS = {'set': 1, 'configuration': 2, 'status': 3, 'defaults': 4,
            'reset-request': 5, 'reset-confirm': 6}
SETTINGS = {
    'report_mode': (1, {'dynamic': 0, 'fixed': 1}),
    'interval_minutes': (2, None),
    'temperature_unit': (3, {'C': 0, 'F': 1}),
    'pressure_unit': (4, {'hPa': 0, 'mbar': 0, 'mb': 0, 'inHg': 1, 'Pa': 2, 'kPa': 3, 'mmHg': 4}),
    'gas_unit': (5, {'ohm': 0, 'kohm': 1}),
    'voc_unit': (6, {'ppm': 0, 'ppb': 1}),
    'serial_level': (7, {name: i for i, name in enumerate(['OFF', 'ERROR', 'WARN', 'INFO', 'DEBUG', 'TRACE'])}),
}

def encode(command, request_id, changes=(), code=None):
    if command not in COMMANDS or not 0 <= request_id <= 65535:
        raise ValueError('Unknown command or request ID outside 0..65535')
    data = bytearray([0xC1, COMMANDS[command], request_id >> 8, request_id & 255])
    if command == 'set':
        pairs, seen = [], set()
        for change in changes:
            name, separator, value = change.partition('=')
            if not separator or name not in SETTINGS or name in seen:
                raise ValueError('Unknown, duplicate, or malformed setting: ' + change)
            key, choices = SETTINGS[name]
            if choices is None:
                number = int(value)
                if not 1 <= number <= 60:
                    raise ValueError('Interval must be 1..60 minutes')
            else:
                if value not in choices:
                    raise ValueError('Invalid setting value: ' + change)
                number = choices[value]
            pairs += [key, number]
            seen.add(name)
        if not seen:
            raise ValueError('Provide at least one --set name=value')
        data += bytes([len(seen)] + pairs)
    elif changes:
        raise ValueError('--set is only valid with the set command')
    if command == 'reset-confirm':
        if code is None or not 100000 <= code <= 999999:
            raise ValueError('Provide the current six-digit --code')
        data += code.to_bytes(4, 'big')
    elif code is not None:
        raise ValueError('--code is only valid with reset-confirm')
    return bytes(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=COMMANDS)
    parser.add_argument('--id', type=int, required=True, dest='request_id')
    parser.add_argument('--set', action='append', default=[], dest='changes')
    parser.add_argument('--code', type=int)
    args = parser.parse_args()
    try:
        data = encode(args.command, args.request_id, args.changes, args.code)
    except (ValueError, OverflowError) as error:
        parser.error(str(error))
    print(json.dumps({'fport': 1, 'hex': data.hex().upper(),
                      'base64': base64.b64encode(data).decode()}, indent=2))

if __name__ == '__main__':
    main()
