#!/usr/bin/env python3
"""Prepare an isolated, checked RadioLib 7.1.2 copy with protocol fixes."""
import argparse
import hashlib
from pathlib import Path
import shutil

UPSTREAM_SHA256 = '99ecfbef09677232b27a36c42326e27d96786e4111e40696316e1ea1ecb30c61'


def patch_source(source):
    """Keep fixes explicit and reject any different upstream source."""
    if hashlib.sha256(source.encode()).hexdigest() != UPSTREAM_SHA256:
        raise ValueError('Unsupported RadioLib source: expected pristine 7.1.2 LoRaWAN.cpp')
    edits = [
        ('return(maxLen - 13 - this->fOptsUpLen);',
         'return(maxLen > 13 + this->fOptsUpLen ? maxLen - 13 - this->fOptsUpLen : 0);', 2),
        ('return(curLen - 13 - this->fOptsUpLen);',
         'return(curLen > 13 + this->fOptsUpLen ? curLen - 13 - this->fOptsUpLen : 0);', 1),
        ('if(this->rxDelayEnd < this->rxDelayStart)',
         'if(static_cast<int32_t>(this->rxDelayEnd - this->rxDelayStart) < 0)', 1),
        ('if(this->tUplink < tNow)', 'if(static_cast<int32_t>(tNow - this->tUplink) > 0)', 1),
        ('if(this->tUplink > tNow)', 'if(static_cast<int32_t>(this->tUplink - tNow) > 0)', 2),
        ('if(this->tUplink > mod->hal->millis())',
         'if(static_cast<int32_t>(this->tUplink - mod->hal->millis()) > 0)', 2),
        ('if(this->rxDelayStart + (RadioLibTime_t)dutyCycleInterval(this->dutyCycle, this->lastToA) > this->tUplink)',
         'if(static_cast<int32_t>(this->rxDelayStart + (RadioLibTime_t)dutyCycleInterval(this->dutyCycle, this->lastToA) - this->tUplink) > 0)', 1),
        ('if(now > tReference + dlDelays[1] - this->scanGuard)',
         'if(static_cast<int32_t>(now - (tReference + dlDelays[1] - this->scanGuard)) > 0)', 1),
        ('if(now < tReference + dlDelays[numWindows])',
         'if(static_cast<int32_t>(tReference + dlDelays[numWindows] - now) > 0)', 1),
    ]
    for old, new, count in edits:
        if source.count(old) != count:
            raise ValueError('RadioLib patch context changed: ' + old)
        source = source.replace(old, new)
    start = source.index('RadioLibTime_t LoRaWANNode::timeUntilUplink()')
    end = source.index('uint8_t LoRaWANNode::getMaxPayloadLen()', start)
    source = source[:start] + '''RadioLibTime_t LoRaWANNode::timeUntilUplink() {
  Module* mod = this->phyLayer->getMod();
  if(!this->dutyCycleEnabled) { return(0); }
  RadioLibTime_t interval = dutyCycleInterval(this->dutyCycle, this->lastToA);
  RadioLibTime_t elapsed = mod->hal->millis() - this->rxDelayStart;
  return(elapsed >= interval ? 0 : interval - elapsed);
}

''' + source[end:]
    return source


def prepare(upstream, output):
    if 'version=7.1.2' not in (upstream / 'library.properties').read_text().splitlines():
        raise ValueError('RadioLib 7.1.2 is required')
    relative = Path('src/protocols/LoRaWAN/LoRaWAN.cpp')
    patched = patch_source((upstream / relative).read_text())
    if upstream.resolve() == output.resolve():
        raise ValueError('Output must be isolated from the installed upstream library')
    shutil.copytree(upstream, output, dirs_exist_ok=True)
    (output / relative).write_text(patched)
    header = output / 'src/RadioLib.h'
    header.write_text('#define RADIOLIB_THERMISTOR_PROTOCOL_FIXES 1\n' + header.read_text())
    print('Prepared isolated RadioLib 7.1.2 with payload and rollover fixes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('upstream', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    prepare(args.upstream, args.output)
