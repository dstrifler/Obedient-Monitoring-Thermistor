#!/usr/bin/env python3
"""Round-trip firmware packets and console command builders without hardware."""
from pathlib import Path
import importlib.util
import json
import subprocess
import tempfile
from host_support import ROOT, prepare, compile_host
CPP=r'''
#include "encoder_service.h"
#include "settings_defaults.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
void emit(const uint8_t* b,size_t n){for(size_t i=0;i<n;++i)printf("%02x",b[i]);puts("");}
int main(int argc,char** argv){
 if(argc>1){
  std::string hex=argv[1];std::vector<uint8_t>b;
  for(size_t i=0;i<hex.size();i+=2)b.push_back(uint8_t(std::stoul(hex.substr(i,2),nullptr,16)));
  DecodedCommand c{};const bool ok=encoderDecode(b.data(),b.size(),&c);
  printf("%u %u %u %u %lu",unsigned(ok),unsigned(c.type),unsigned(c.requestId),unsigned(c.changedMask),(unsigned long)c.confirmationCode);
  for(auto v:c.values)printf(" %u",unsigned(v));puts("");return 0;
 }
 SensorSnapshot sample{};sample.environment={25,50,100000,65535,true};sample.airQuality={50,500,0.5f,7,0,false,false};
 uint8_t bytes[32];AppSettings s=settingsDefaults();
 for(unsigned temp=0;temp<2;++temp)for(unsigned pres=0;pres<5;++pres)for(unsigned gas=0;gas<2;++gas)for(unsigned voc=0;voc<2;++voc){
  s.temperature=TemperatureUnit(temp);s.pressure=PressureUnit(pres);s.gas=GasUnit(gas);s.voc=VocUnit(voc);
  const auto n=encoderSensorPacket(bytes,sizeof(bytes),sample,s);assert(n==25);emit(bytes,n);
 }
 ControlReply r{};r.kind=ReplyKind::Configuration;r.command=CommandType::GetConfiguration;r.requestId=12;r.settings=settingsDefaults();
 emit(bytes,encoderControlPacket(bytes,sizeof(bytes),r));
 r.kind=ReplyKind::ResetChallenge;r.command=CommandType::RequestReset;r.requestId=50;r.value=654321;
 emit(bytes,encoderControlPacket(bytes,sizeof(bytes),r));
 r={};r.kind=ReplyKind::Status;r.status={123456,1200,3600,127,0,3,129,-5,65535};
 emit(bytes,encoderControlPacket(bytes,sizeof(bytes),r));
}
'''
JS=r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');let c={};
vm.runInNewContext(fs.readFileSync(process.argv[2],'utf8'),c);
const packets=fs.readFileSync(0,'utf8').trim().split(/\s+/).map(h=>Array.from(Buffer.from(h,'hex')));
let i=0;const divisors=[100,3386.389,1,1000,133.3224];
for(let temp=0;temp<2;++temp)for(let pres=0;pres<5;++pres)for(let gas=0;gas<2;++gas)for(let voc=0;voc<2;++voc){
 const d=c.Decoder(packets[i++],1);assert(!d.error);
 assert.strictEqual(d.temperature_c,25);assert.strictEqual(d.temperature,temp?77:25);
 assert(Math.abs(d.pressure-100000/divisors[pres])<1e-8);
 assert.strictEqual(d.gas_resistance,65535/(gas?1000:1));
 assert.strictEqual(d.breath_voc_equivalent,voc?500:0.5);
}
assert.strictEqual(i,40);const cfg=c.Decoder(packets[i++],1);
assert.strictEqual(cfg.settings.report_mode,'dynamic');assert.strictEqual(cfg.settings.temperature_unit,'F');
assert.strictEqual(cfg.settings.pressure_unit,'inHg');assert.strictEqual(cfg.settings.serial_level,'INFO');
assert.strictEqual(c.Decoder(packets[i++],1).reset_confirmation_code,'654321');
const health=c.Decoder(packets[i++],1);assert.strictEqual(health.packet_type,'health');
assert.strictEqual(health.status.radio_error,-5);assert.strictEqual(health.status.sample_age_s,null);
assert.strictEqual(health.status.reset_confirmation_pending,true);
for(const b of [[],[161],packets[40].slice(0,16),[161,9,...Array(8).fill(0)],[161,0,0,0,0,9,0,0,0,0]]){
 const d=c.Decoder(b,1);assert(d.error||d.mac_only);
}
const bad=packets[0].slice();bad[24]=255;assert(c.Decoder(bad,1).error);
const set=c.encodeDownlink({data:{command:'set',request_id:1,settings:{report_mode:'fixed',interval_minutes:10}}});
assert.strictEqual(Buffer.from(set.bytes).toString('hex'),'c1010001020101020a');
for(const data of [{command:'set',request_id:1,settings:{}},{command:'set',request_id:1,settings:{interval_minutes:0}},
 {command:'set',request_id:1,settings:{foo:1}},{command:'reset_confirm',request_id:2,code:3}]){
 assert(c.encodeDownlink({data}).errors);
}
console.log('PASS: 40 presentation combinations, firmware configuration/status/challenge packets, malformed control data and JS commands');
'''
with tempfile.TemporaryDirectory(prefix='monitor-protocol-') as temp:
    folder=Path(temp);prepare(folder);(folder/'fixture.cpp').write_text(CPP);(folder/'check.js').write_text(JS)
    exe=compile_host(folder,['fixture.cpp','encoder_service.cpp','app_settings.cpp','sensor_types.cpp'],'protocol', ['-Wno-misleading-indentation'])
    packets=subprocess.check_output([str(exe)],text=True)
    subprocess.run(['node',str(folder/'check.js'),str(ROOT/'decoders/environment_v1.js')],input=packets,text=True,check=True)
    spec=importlib.util.spec_from_file_location('commands',ROOT/'tools/downlink.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    batch=module.encode('set',1,['report_mode=fixed','interval_minutes=10'])
    assert batch.hex()=='c1010001020101020a'
    parsed=subprocess.check_output([str(exe),batch.hex()],text=True).split()
    assert parsed[:5]==['1','1','1','3','0'] and parsed[5:7]==['1','10']
    confirmation=module.encode('reset-confirm',51,code=654321)
    parsed=subprocess.check_output([str(exe),confirmation.hex()],text=True).split()
    assert parsed[:5]==['1','6','51','0','654321']
    for malformed in ['c10100010201010100','c1010001010801','c1010001010200ff','01010001','c106000100000001']:
        assert subprocess.check_output([str(exe),malformed],text=True).split()[0]=='0'
    print('PASS: console Base64 builder/golden bytes and real firmware command decoding')
