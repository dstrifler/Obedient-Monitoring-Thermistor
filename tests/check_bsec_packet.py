#!/usr/bin/env python3
"""Round-trip actual v3 firmware packets through the real console decoder."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CPP = r'''
#include "encoder_service.h"
#include "settings_defaults.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cmath>
void emit(const SensorSnapshot* sample) {
 uint8_t data[26];memset(data,0xa5,sizeof(data));
 SensorSnapshot absent={};absent.environment.temperatureC=NAN;absent.environment.humidityPct=NAN;
 assert(encoderSensorPacket(data,25,sample?*sample:absent,settingsDefaults())==25 && data[25]==0xa5);
 for(unsigned i=0;i<25;++i)printf("%02x",unsigned(data[i]));puts("");
}
int main(){
 SensorSnapshot sample{};
 sample.environment={-0.25f,100,110000,UINT32_MAX,true};
 sample.airQuality={42.25f,1234.5f,1.23456789f,7,3,true,true};
 sample.sampleUptimeSeconds=0xf1234567;
 for(unsigned mask=0;mask<8;++mask){sample.airQuality.validMask=mask;emit(&sample);}
 sample.airQuality.validMask=7;
 const float values[]={0,0.5f,1000.125f,std::numeric_limits<float>::denorm_min(),
   std::numeric_limits<float>::min(),std::numeric_limits<float>::max()};
 for(float value:values){sample.airQuality.breathVocEquivalentPpm=value;emit(&sample);}
 sample.airQuality.breathVocEquivalentPpm=NAN;emit(&sample);
 sample.airQuality.breathVocEquivalentPpm=INFINITY;emit(&sample);
 sample.airQuality.breathVocEquivalentPpm=-1;emit(&sample);
 sample.airQuality.breathVocEquivalentPpm=0;
 sample.airQuality.iaq=501;sample.airQuality.co2EquivalentPpm=65536;emit(&sample);
 emit(nullptr);
 uint8_t shortBuffer[24];memset(shortBuffer,0xa5,sizeof(shortBuffer));
 assert(!encoderSensorPacket(shortBuffer,23,sample,settingsDefaults()) && shortBuffer[0]==0xa5);
 assert(!encoderSensorPacket(nullptr,25,sample,settingsDefaults()));
}
'''
JS = r'''
const assert=require('assert'),fs=require('fs'),vm=require('vm');
const api={};vm.runInNewContext(fs.readFileSync(process.argv[2],'utf8'),api);
const packets=fs.readFileSync(0,'utf8').trim().split(/\s+/).map(s=>Array.from(Buffer.from(s,'hex')));
const decode=bytes=>JSON.parse(JSON.stringify(api.Decoder(bytes,1)));
assert.strictEqual(packets.length,19);
for(let mask=0;mask<8;mask++){
 const bytes=packets[mask],d=decode(bytes);
 assert.strictEqual(bytes.length,25);assert.strictEqual(bytes[0],0x3f);
 assert.strictEqual(d.format_version,3);assert.strictEqual(d.valid_mask,15);
 const legacy=bytes.slice(0,24);legacy[0]=0x20|(legacy[0]&15);const d2=decode(legacy);
 assert.strictEqual(d2.format_version,2);
 for(const name of ['temperature_c','humidity_pct','pressure_pa','gas_ohms','iaq','co2_equivalent_ppm','breath_voc_equivalent_ppm','sample_uptime_s'])assert.strictEqual(d2[name],d[name]);
 assert.strictEqual(d.temperature_c,-0.25);assert.strictEqual(d.humidity_pct,100);
 assert.strictEqual(d.pressure_pa,110000);assert.strictEqual(d.gas_ohms,4294967295);
 assert.strictEqual(d.air_quality_valid_mask,mask);
 assert.strictEqual(d.iaq,mask&1?42.3:null);
 assert.strictEqual(d.co2_equivalent_ppm,mask&2?1235:null);
 assert.strictEqual(d.breath_voc_equivalent_ppm,mask&4?Math.fround(1.23456789):null);
 assert.strictEqual(d.iaq_accuracy,mask&1?3:null);
 assert.strictEqual(d.sensor_stabilized,true);assert.strictEqual(d.sensor_run_in_complete,true);
 assert.strictEqual(d.sample_uptime_s,4045620583);
 assert.deepStrictEqual(JSON.parse(JSON.stringify(api.decodeUplink({bytes,fPort:1}))),{data:d});
}
const values=[0,0.5,1000.125,Math.pow(2,-149),Math.pow(2,-126),3.4028234663852886e38];
for(let i=0;i<values.length;i++){
 const bytes=packets[8+i],buffer=Buffer.from(bytes);
 assert.strictEqual(buffer.readFloatBE(15),values[i]);
 assert.strictEqual(decode(bytes).breath_voc_equivalent_ppm,values[i]);
}
for(let i=14;i<17;i++){
 const d=decode(packets[i]);assert.strictEqual(d.breath_voc_equivalent_ppm,null);
 assert.strictEqual(d.air_quality_valid_mask,3);
}
assert.strictEqual(decode(packets[17]).iaq,null);
assert.strictEqual(decode(packets[17]).co2_equivalent_ppm,null);
const absent=decode(packets[18]);
assert.strictEqual(absent.valid_mask,0);assert.strictEqual(absent.air_quality_valid_mask,0);
assert.strictEqual(absent.breath_voc_equivalent_ppm,null);
// Never throw/disable Helium on malformed v2 inputs, then recover on valid data.
for(const exponent of [0x7f800000,0x7fc00000,0xff800000,0xbf800000]){
 const buffer=Buffer.from(packets[7]);buffer.writeUInt32BE(exponent,15);
 assert.strictEqual(typeof decode(Array.from(buffer)).error,'string');
 assert.strictEqual(decode(packets[7]).iaq,42.3);
}
for(const bytes of [packets[7].slice(0,23),[0x20,...new Array(10).fill(0)],
                   [0x10,...new Array(23).fill(0)]]){
 assert.strictEqual(typeof decode(bytes).error,'string');
}
const compensated=packets[7].slice();compensated[19]|=128;
assert.strictEqual(decode(compensated).temperature_humidity_compensated,true);
assert.strictEqual(decode(packets[7]).temperature_humidity_compensated,false);
assert.deepStrictEqual(decode([]),{mac_only:true});
console.log('PASS: v3 actual encoder/decoder, all derived validity masks, IEEE-754 precision/range, unsigned uptime, missing data, malformed packets');
'''
with tempfile.TemporaryDirectory(prefix='bsec-packet-') as tmp:
    folder=Path(tmp)
    from host_support import prepare
    prepare(folder)
    (folder/'fixture.cpp').write_text(CPP)
    (folder/'check.js').write_text(JS)
    exe=folder/'packet'
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                    '-I',str(folder),'-I',str(ROOT),str(folder/'fixture.cpp'),
                    str(folder/'encoder_service.cpp'),str(folder/'common.cpp'),'-o',str(exe)],check=True)
    data=subprocess.check_output([str(exe)],text=True)
    subprocess.run(['node',str(folder/'check.js'),str(ROOT/'decoders/environment_v1.js')],
                   input=data,text=True,check=True)
