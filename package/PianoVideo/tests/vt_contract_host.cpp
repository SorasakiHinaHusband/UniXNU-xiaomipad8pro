// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "PianoVideoVTContract.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace pianovideo;
static int failures;
#define CHECK(x) do { if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x); ++failures;} } while(0)
int main() {
  uint8_t sample[252], au[287];
  vtwire::put32(sample,248); memcpy(sample+4,session::kAccessUnit+38,248);
  CHECK(vtcontract::sample(sample,sizeof sample));
  CHECK(vtcontract::normalize(sample,sizeof sample,au,sizeof au)==287);
  CHECK(vtcontract::normalizedAu(au,sizeof au));
  CHECK(!vtcontract::sample(sample,251));
  sample[4]=0x61; CHECK(!vtcontract::sample(sample,252)); sample[4]=0x65;
  au[4]^=1; CHECK(!vtcontract::normalizedAu(au,287)); au[4]^=1;
  au[80]=0; au[81]=0; au[82]=1; CHECK(!vtcontract::normalizedAu(au,287));
  std::vector<uint8_t> huge(vtcontract::kMaxAuBytes+1); CHECK(!vtcontract::normalizedAu(huge.data(),uint32_t(huge.size())));
  native::Layout l; CHECK(vtcontract::layout(l,l.bytes)); l.uvOffset=92160; CHECK(!vtcontract::layout(l,l.bytes));
  CHECK(vtcontract::extent(0,384,240,320,92160)); CHECK(!vtcontract::extent(UINT64_MAX,384,240,320,92160));
  CHECK(!vtcontract::extent(0,UINT64_MAX,240,320,92160));
  vtcontract::Surface s; s.base=0x1000;s.size=115200;s.id=1;s.format=vtwire::cc("420v");s.planes=2;
  s.plane[0]={320,240,0,320,0};s.plane[1]={160,120,76800,320,0}; CHECK(vtcontract::surface(s));
  const auto allocation=s.size;s.size=uint64_t(vtcontract::kMaxSurfaceBytes)+1;CHECK(!vtcontract::surface(s));s.size=allocation;
  s.plane[1].nonlinear=1;CHECK(!vtcontract::surface(s));s.plane[1].nonlinear=0;
  s.plane[1].offset=76801;CHECK(!vtcontract::surface(s));
  uint8_t locked[0xc68]={};
  auto put64=[&](unsigned at,uint64_t x){memcpy(locked+at,&x,8);};
  auto put32=[&](unsigned at,uint32_t x){memcpy(locked+at,&x,4);};
  put64(0,0x1000);put32(0x18,27);put64(0x20,115200);put32(0x48,vtwire::cc("420v"));put32(0x58,2);
  for(unsigned p=0;p<2;++p){unsigned at=0x88+p*0x98;put64(at,p?160:320);put64(at+8,p?120:240);put64(at+0x18,p?76800:0);put64(at+0x20,320);locked[at+0x6d]=0xa5;}
  vtcontract::Surface parsed;CHECK(vtcontract::surfaceResult(locked,sizeof locked,27,parsed));
  CHECK(!vtcontract::surfaceResult(locked,sizeof locked-1,27,parsed));CHECK(!vtcontract::surfaceResult(locked,sizeof locked,28,parsed));
  locked[0x88+0x6c]=1;CHECK(!vtcontract::surfaceResult(locked,sizeof locked,27,parsed));locked[0x88+0x6c]=0;
  vtcontract::State st; CHECK(!st.accept());st.started=true;CHECK(st.accept());CHECK(!st.accept());CHECK(st.success());
  st.cancel();CHECK(!st.success());CHECK(st.complete());CHECK(!st.complete());CHECK(!st.accept());
  vtcontract::State finished;finished.started=true;finished.finish();CHECK(!finished.accept());
  uint8_t ext[46]={};vtwire::put32(ext,46);vtwire::put32(ext+4,vtwire::cc("avcC"));
  const uint8_t prefix[]={1,66,0xc0,13,0xff,0xe1,0,22};memcpy(ext+8,prefix,8);memcpy(ext+16,session::kAccessUnit+4,22);
  ext[38]=1;ext[39]=0;ext[40]=5;memcpy(ext+41,session::kAccessUnit+30,5);
  vtwire::ImageDescription d;d.codec=vtwire::cc("avc1");d.width=320;d.height=240;d.extensions=ext;d.extensionsSize=46;
  CHECK(vtcontract::format(d));d.width=321;CHECK(!vtcontract::format(d));d.width=320;ext[9]=100;CHECK(!vtcontract::format(d));
  uint8_t wire[256]={},uuid[16]={};vtwire::Writer msg(wire,sizeof wire);msg.header(vtwire::cc("dfrm"),vtwire::FlagRequest,uuid,1);msg.s64(vtwire::cc(",frm"),0);
  CHECK(vtcontract::message(wire,msg.size()));msg.s64(vtwire::cc(",frm"),1);CHECK(!vtcontract::message(wire,msg.size()));
  CHECK(!vtcontract::message(wire,msg.size()-1));
  uint8_t plist[48]={};memcpy(plist,"bplist00",8);plist[8]=0x5f;plist[9]=0x1f;plist[16]=8;
  plist[22]=1;plist[23]=1;vtwire::put64(plist+24,1);vtwire::put64(plist+32,0);vtwire::put64(plist+40,16);
  char name[96];CHECK(!vtwire::plistString(plist,sizeof plist,name,sizeof name));
  if(failures)return 1;std::puts("PianoVideo V5a contract: sample/all-NAL/format/layout/overflow/cancel tests PASS");return 0;
}
