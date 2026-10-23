#!/usr/bin/env python3
"""Check the console decoder, including packets made by the actual C++ encoder."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NODE = shutil.which('node')
assert NODE, 'Node.js is required to check the JavaScript decoder'

ENCODER_FIXTURE = r'''
#include "payload_service.h"
#include <cmath>
#include <cstdio>
void emit(const SensorData& sample) {
 uint8_t bytes[ENVIRONMENT_PACKET_SIZE];
 if(payloadEncodeEnvironment(bytes,sizeof(bytes),&sample)!=11)throw 1;
 for(uint8_t byte:bytes)printf("%02x",unsigned(byte));
 puts("");
}
int main(){
 for(unsigned mask=0;mask<16;++mask){
  SensorData sample{(mask&1)?25.25f:NAN,(mask&2)?50.5f:NAN,(mask&4)?100123.0f:NAN,12345,bool(mask&8)};
  emit(sample);
 }
 emit(SensorData{-0.25f,100,110000,0xf1234567U,true});
 emit(SensorData{-40,0,30000,UINT32_MAX,true});
 emit(SensorData{0,0,100000,1,true});
}
'''

NODE_TEST = r'''
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const context = {};
vm.runInNewContext(fs.readFileSync(process.argv[2], 'utf8'), context);
const plain = value => JSON.parse(JSON.stringify(value));
const hex = text => Array.from(Buffer.from(text, 'hex'));
const decode = bytes => plain(context.Decoder(bytes, 1));
const packets = fs.readFileSync(0, 'utf8').trim().split(/\s+/).map(hex);
assert.strictEqual(packets.length, 19);
for (let mask = 0; mask < 16; mask++) {
  const bytes = packets[mask], original = bytes.slice();
  const expected = {
    format_version: 1, valid_mask: mask,
    temperature_c: (mask & 1) ? 25.25 : null,
    humidity_pct: (mask & 2) ? 50.5 : null,
    pressure_pa: (mask & 4) ? 100124 : null,
    pressure_hpa: (mask & 4) ? 1001.24 : null,
    gas_ohms: (mask & 8) ? 12345 : null
  };
  assert.deepStrictEqual(decode(bytes), expected);
  assert.deepStrictEqual(plain(context.Decoder(bytes, 1, { fcnt: 42, port: 1 })), expected);
  assert.deepStrictEqual(plain(context.decodeUplink({ bytes, fPort: 1 })), { data: expected });
  assert.deepStrictEqual(decode(new Uint8Array(bytes)), expected);
  assert.deepStrictEqual(bytes, original);
}
assert.deepStrictEqual(packets[15], hex('1f09dd13bac38e00003039'));
assert.strictEqual(decode(packets[16]).temperature_c, -0.25);
assert.strictEqual(decode(packets[16]).gas_ohms, 4045620583);
assert.strictEqual(decode(packets[17]).temperature_c, -40);
assert.strictEqual(decode(packets[17]).gas_ohms, 4294967295);
assert.strictEqual(decode(packets[18]).temperature_c, 0);
assert.strictEqual(decode(packets[18]).humidity_pct, 0);
// Validity bits, rather than the slot values, decide availability.
const unavailable = new Array(11).fill(255); unavailable[0] = 0x10;
for (const field of ['temperature_c','humidity_pct','pressure_pa','pressure_hpa','gas_ohms']) {
  assert.strictEqual(decode(unavailable)[field], null);
}
assert.deepStrictEqual(plain(context.Decoder([], 0)), { mac_only: true });
assert.deepStrictEqual(plain(context.decodeUplink({ bytes: [], fPort: 0 })), { data: { mac_only: true } });
for (const length of [0,1,10,12,14]) assert.throws(() => decode(new Array(length).fill(0)));
for (const port of [undefined,0,2,'1']) assert.throws(() => context.Decoder(packets[15], port));
for (let version=0; version<16; version++) {
  if (version===1) continue;
  const bytes=packets[15].slice();bytes[0]=(version<<4)|15;
  assert.throws(() => decode(bytes), /version/);
}
assert.throws(() => decode(hex('48656c6c6f20576f726c64')), /version/);
for (const bad of [-1,256,1.5,NaN,Infinity,'1',undefined,null]) {
  const bytes=packets[15].slice();bytes[5]=bad;
  assert.throws(() => decode(bytes), /Invalid byte/);
}
for (const bad of [null,undefined,'12345678901',{}]) assert.throws(() => decode(bad));
for (const bad of [null, undefined, {}, { bytes: [], fPort: 1 }]) {
  const result=plain(context.decodeUplink(bad));
  assert.strictEqual(result.errors.length,1);assert.strictEqual(typeof result.errors[0],'string');
  assert.strictEqual(result.data,undefined);
}
console.log('PASS: actual firmware encoder -> both decoder APIs, all validity masks, signed/unsigned boundaries, zero readings, MAC-only and malformed inputs');
'''

with tempfile.TemporaryDirectory(prefix='thermistor-decoder-') as tmp:
    folder = Path(tmp)
    # This encoder includes public headers only; no provisioning config is read.
    (folder / 'Arduino.h').write_text('#pragma once\n')
    (folder / 'encoder.cpp').write_text(ENCODER_FIXTURE)
    (folder / 'check.js').write_text(NODE_TEST)
    program = folder / 'encoder'
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I', str(folder), '-I', str(ROOT), str(folder / 'encoder.cpp'),
                    str(ROOT / 'payload_service.cpp'), '-o', str(program)], check=True)
    packets = subprocess.check_output([str(program)], text=True)
    subprocess.run([NODE, str(folder / 'check.js'), str(ROOT / 'decoders/environment_v1.js')],
                   input=packets, text=True, check=True)
