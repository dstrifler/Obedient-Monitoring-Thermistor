#!/usr/bin/env python3
"""Configure Arduino IDE 1.8.19 to link BSEC for the expLoRaBLE board."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BEGIN = '# BEGIN Obedient Monitoring Thermistor BSEC\n'
END = '# END Obedient Monitoring Thermistor BSEC\n'
FLAG_KEY = 'lora_thing_plus.compiler.ld.extra_flags'
SIZE_KEY = 'lora_thing_plus.upload.maximum_size'


def property_value(text, key, default=''):
    value = default
    for line in text.splitlines():
        if line.lstrip().startswith(('#', '!')):
            continue
        name, separator, candidate = line.partition('=')
        if separator and name.strip() == key:
            value = candidate.strip()
    return value


def without_managed_block(text):
    if not text.count(BEGIN) and not text.count(END):
        return text
    if text.count(BEGIN) != 1 or text.count(END) != 1:
        raise ValueError('Malformed existing Thermistor configuration block; file left unchanged.')
    start, finish = text.index(BEGIN), text.index(END)
    if finish < start:
        raise ValueError('Malformed existing Thermistor configuration block; file left unchanged.')
    return text[:start] + text[finish + len(END):]


def atomic_write(path, text):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix=path.name + '.', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        if path.exists():
            shutil.copymode(path, temporary)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def detect_platform(home):
    candidates = sorted((home / '.arduino15/packages/SparkFun/hardware/apollo3').glob('*/platform.txt'))
    if len(candidates) != 1:
        raise ValueError('Expected one installed SparkFun Apollo3 package. Use --platform-dir PATH '
                         'to select the folder containing platform.txt and boards.txt.')
    return candidates[0].parent


def detect_sketchbook(home):
    preferences = home / '.arduino15/preferences.txt'
    value = property_value(preferences.read_text(), 'sketchbook.path') if preferences.exists() else ''
    return Path(value).expanduser() if value else home / 'Arduino'


def detect_bsec(sketchbook):
    candidates = list((sketchbook / 'libraries').glob('*/src/cortex-m4/fpv4-sp-d16-hard/libalgobsec.a'))
    if len(candidates) != 1:
        raise ValueError('Expected one installed bsec2 library. Use --bsec-dir PATH to select '
                         'the library folder containing library.properties.')
    return candidates[0].parents[3]


def configure(platform, bsec, project=ROOT):
    platform, bsec, project = platform.resolve(), bsec.resolve(), project.resolve()
    archive = bsec / 'src/cortex-m4/fpv4-sp-d16-hard/libalgobsec.a'
    guard = project / 'tools/nonce_guard.ld'
    for path in [archive, guard]:
        if not path.is_file():
            raise ValueError('Required file not found: ' + str(path))
        if any(character in str(path) for character in '\r\n"'):
            raise ValueError('Linker paths cannot contain quotes or line breaks: ' + str(path))
    platform_text = (platform / 'platform.txt').read_text()
    if '{compiler.ld.extra_flags}' not in property_value(platform_text, 'compiler.ld.flags'):
        raise ValueError('Unexpected Apollo3 linker recipe; original files left unchanged.')
    boards_text = (platform / 'boards.txt').read_text()
    if not property_value(boards_text, 'lora_thing_plus.name'):
        raise ValueError('This package does not define the expLoRaBLE board.')
    target = platform / 'boards.local.txt'
    existing = target.read_text() if target.exists() else ''
    original = without_managed_block(existing)
    flags = property_value(platform_text, 'compiler.ld.extra_flags')
    global_config = platform / 'platform.local.txt'
    if global_config.exists():
        flags = property_value(global_config.read_text(), 'compiler.ld.extra_flags', flags)
    flags = property_value(boards_text, FLAG_KEY, flags)
    flags = property_value(original, FLAG_KEY, flags)
    maximum = property_value(original, SIZE_KEY,
                             property_value(boards_text, SIZE_KEY, '958464'))
    if not maximum.isdecimal():
        raise ValueError('Existing upload maximum is not numeric; configuration left unchanged.')
    maximum = min(int(maximum), 958464)
    prefix = original if not original or original.endswith('\n') else original + '\n'
    block = (BEGIN + FLAG_KEY + '=' + flags + (' ' if flags else '') +
             '"' + str(archive) + '" "-Wl,-T,' + str(guard) + '"\n' +
             SIZE_KEY + '=' + str(maximum) + '\n' + END)
    result = prefix + block
    if result != existing:
        backup = platform / 'boards.local.txt.thermistor-backup'
        if target.exists() and not backup.exists():
            shutil.copy2(target, backup)
        atomic_write(target, result)
    return target


def remove(platform):
    target = platform / 'boards.local.txt'
    if not target.exists():
        return target
    existing = target.read_text()
    original = without_managed_block(existing)
    if original != existing:
        if original:
            atomic_write(target, original)
        else:
            target.unlink()
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform-dir', type=Path)
    parser.add_argument('--sketchbook-dir', type=Path)
    parser.add_argument('--bsec-dir', type=Path)
    parser.add_argument('--remove', action='store_true', help='Remove only this helper\'s board overrides')
    args = parser.parse_args()
    try:
        platform = args.platform_dir or detect_platform(Path.home())
        if args.remove:
            print('Removed Thermistor overrides from:', remove(platform))
            return
        sketchbook = args.sketchbook_dir or detect_sketchbook(Path.home())
        bsec = args.bsec_dir or detect_bsec(sketchbook)
        subprocess.run([sys.executable, str(ROOT / 'tools/verify_bsec.py'), str(bsec)], check=True)
        print('Configured:', configure(platform, bsec))
        print('Restart Arduino IDE, select LoRa Thing Plus expLoRaBLE, then Verify/Upload normally.')
        print('Rerun this helper after moving the repository/library or updating the board package.')
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
