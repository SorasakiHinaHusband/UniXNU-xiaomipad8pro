// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Scripted firmware peer for the real persistent HFI state machine. These are
// ownership/protocol checks, not evidence that any frame decoded on hardware.
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <array>
#include <string>
#include <CommonCrypto/CommonDigest.h>
#include "VideoSession.h"
using namespace pianovideo;
using namespace pianovideo::session;
static unsigned failures;
#define CHECK(x) do { if (!(x)) {std::printf("FAIL line %u: %s\n",__LINE__,#x);++failures;} } while(0)
struct Hardware {
  uint64_t clock=0;
  std::string log;
  void barrier() {}
  void stage(int,const char *) {}
  void sleepMs(unsigned n) {clock+=uint64_t(n)*1000;}
  uint64_t nowUs() const {return clock;}
  void note(const char *fmt,...) {
    char line[1024];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
    log+=line;log+='\n';if(std::getenv("VIDEO_STREAM_ECHO"))std::puts(line);
  }
  void hashActive(const volatile uint8_t *base,char text[65]) {
    CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);
    activeRows(base,[&](const uint8_t *p,unsigned n){CC_SHA256_Update(&ctx,p,n);});
    uint8_t digest[32];CC_SHA256_Final(digest,&ctx);hexDigest(digest,text);
  }
};
struct Registers {
  bool fault=false,watchdog=false;
  uint32_t rd(uint64_t pa) {return pa==kIris+kWrapperIntrStatus?(watchdog?8:0):(fault&&(pa&4095)==vsmmu::kFsr?2:0);}
};
using Words=std::vector<uint32_t>;
struct Peer {
  Hardware hw;Registers ops;vsmmu::Plan plan;hfi::Mem mem;hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords]={},rx[hfi::kBufferWords]={};
  struct {volatile uint8_t *arena=nullptr;} dma;
  struct {uint32_t fsr=0;} rep;
  unsigned watchdogSamples=0,watchdogSeen=0,rxEvents=0;
  std::vector<uint32_t> uc;
  std::vector<uint8_t> arena;
  std::array<Words,2> inputs;
  std::array<bool,2> inputPending={};
  std::array<Words,64> raws;
  std::array<bool,64> rawPending={};
  std::vector<Words> releasePending;
  std::vector<uint32_t> headers,packets,comvCounts,comvSizes;
  uint32_t sid=0;
  unsigned n=4,etbs=0,ftbs=0,dpbs=0,opens=0,closes=0,stops=0,drains=0;
  unsigned settingsResume=0,drainInputResume=0,drainRawResume=0;
  uint32_t pscPoc=0,pscProfile=1,pscLevel=4;
  uint32_t pscResolution=0x014000f0,pscDepth=0x00080008,pscCoded=1,pscCrop0=0,pscCrop1=0;
  bool omitOptional=false;
  unsigned decodeOrderProperties=0;
  bool autoEbd=true,autoDrainAck=true,autoDrainLast=true,dropStop=false,dropClose=false,lateClose=false;
  bool lastWithPicture=false;
  uint64_t finalToken=0;
  bool pictureBeforeEbd=false;
  Peer():uc(kUcBytes/4),arena(kArenaBytes) {mem.uc=uc.data();dma.arena=arena.data();hfi::initQueues(mem);plan.bank=3;plan.pages=128;}
  static Words packet(uint32_t type,uint32_t flags,uint32_t port,uint32_t payload,const Words &data={}) {
    Words p={uint32_t(32+4*data.size()),type,flags,payload,port,0x1234,0,0};p.insert(p.end(),data.begin(),data.end());return p;
  }
  void reply(const std::vector<Words> &parts,uint32_t session=0) {
    Words b={32,session?session:sid,0xabc,0,0,0,0,uint32_t(parts.size())};
    for(const auto &p:parts)b.insert(b.end(),p.begin(),p.end());b[0]=uint32_t(4*b.size());
    CHECK(hfi::write(mem,hfi::kMsgq,b.data(),b[0],[]{})==0);
  }
  void psc() {
    std::vector<Words> parts={packet(Settings,0,1,0),packet(0x03000103,0,1,1,{pscResolution}),packet(0x03000105,0,1,13,{pscCrop0,pscCrop1}),
      packet(0x0300010f,0,1,1,{pscDepth}),packet(0x03000120,0,1,1,{pscCoded}),packet(0x03000124,0,1,1,{n})};
    if(!omitOptional){parts.push_back(packet(0x03000128,0,1,1,{pscPoc}));parts.push_back(packet(0x03000107,0,1,1,{pscProfile}));
      parts.push_back(packet(0x03000108,0,1,1,{pscLevel}));}
    parts.push_back(packet(0x03000155,0,1,1,{0x14000003}));reply(parts);
  }
  void ebd(unsigned index,bool duplicate=false) {
    CHECK(index<inputs.size()&&!inputs[index].empty());
    if(index>=inputs.size()||inputs[index].empty())return;
    CHECK(inputPending[index]||duplicate);auto b=inputs[index];b[7]=b[8]=b[9]=0;b[10]=0;
    inputPending[index]=false;reply({packet(Buffers,0,1,5,b)});
  }
  unsigned freeRaw() const {for(unsigned i=0;i<n;++i)if(rawPending[i])return i;return 64;}
  void output(uint64_t token,uint32_t picture=2,uint32_t flags=0,unsigned index=64,bool unknown=false) {
    if(index==64)index=freeRaw();
    CHECK(index<raws.size()&&!raws[index].empty());
    if(index>=raws.size()||raws[index].empty())return;
    CHECK(rawPending[index]||unknown);auto b=raws[index];rawPending[index]=false;
    b[7]=kRawBytes;b[8]=uint32_t(token);b[9]=uint32_t(token>>32);b[10]=flags;
    // Distinguish every output's active rows; consumer can prove retained RAW
    // did not change while other slots were pumped and recycled.
    auto *base=arena.data()+b[2]-kArenaBase;
    for(unsigned y=0;y<240;++y)for(unsigned x=0;x<320;++x)base[y*384+x]=uint8_t(token+x+y);
    for(unsigned y=0;y<120;++y)for(unsigned x=0;x<320;++x)base[kUvOffset+y*384+x]=uint8_t((token>>8)+x+y);
    reply({packet(Buffers,0,2,5,b),packet(0x03000162,0,2,1,{picture})});
  }
  void emptyLast() {
    const unsigned index=freeRaw();CHECK(index<raws.size());if(index>=raws.size())return;
    auto b=raws[index];rawPending[index]=false;b[7]=b[8]=b[9]=0;b[10]=0x10000000;
    reply({packet(Buffers,0,2,5,b)});
  }
  void packetTrace(const char *,unsigned,const uint32_t *,unsigned) {}
  void drainDebug() {}
  int send(unsigned bytes,const char *) {
    CHECK(hfi::write(mem,hfi::kCmdq,tx,bytes,[]{})==0);
    uint32_t data[hfi::kBufferWords],got=0;
    CHECK(hfi::read(mem,hfi::kCmdq,data,sizeof data,&got,[]{})==0);
    CHECK(got==bytes&&data[7]==1);
    const auto *p=data+8;const auto type=p[1],flags=p[2],port=p[4],payload=p[3];
    headers.push_back(data[2]);packets.push_back(p[5]);
    if(type==Open){CHECK(data[1]==0&&flags==3&&payload==1);sid=p[8];++opens;return 0;}
    CHECK(data[1]==sid);
    if(type==Start){CHECK(flags==3&&payload==0&&(port==1||port==2));return 0;}
    if(type==Resume){
      CHECK(flags==3&&payload==1);
      if(p[8]==Settings){CHECK(port==1);++settingsResume;for(auto b:releasePending){b[10]=1;reply({packet(Buffers,0,1,5,b)});}releasePending.clear();}
      else if(p[8]==Drain){if(port==1)++drainInputResume;else if(port==2)++drainRawResume;else CHECK(false);}
      else CHECK(false);
      return 0;
    }
    if(type==Buffers){
      CHECK(payload==5&&p[0]==96);Words b(p+8,p+24);CHECK((b[10]&0x200)&&!b[3]&&!b[4]&&!b[6]);
      if(b[10]&1){CHECK(flags==3);releasePending.push_back(b);return 0;}
      CHECK(flags==1);
      if(b[0]==Bitstream){
        CHECK(b[1]<2&&!inputPending[b[1]]&&b[7]&&b[7]<=(1u<<20));
        inputs[b[1]]=b;inputPending[b[1]]=true;++etbs;
        if(autoEbd)ebd(b[1]);if(etbs==1)psc();
      } else if(b[0]==Raw){CHECK(b[1]<64&&!rawPending[b[1]]&&b[7]==0);raws[b[1]]=b;rawPending[b[1]]=true;++ftbs;}
      else if(b[0]==Dpb){CHECK(b[1]<n);++dpbs;}
      else if(b[0]==Comv)comvSizes.push_back(b[5]);
      return 0;
    }
    if(type==Drain){
      CHECK(flags==7&&port==1&&payload==0);++drains;
      if(autoDrainAck)reply({packet(Drain,1,1,0)});
      if(autoDrainLast){if(lastWithPicture)output(finalToken,4,0x10000000);else emptyLast();}
      return 0;
    }
    if(type==Stop){CHECK(flags==7);++stops;if(!dropStop)reply({packet(Stop,1,port,0)});return 0;}
    if(type==Close){CHECK(flags==7&&port==0);++closes;if(!dropClose){reply({packet(Close,1,0,0)});if(lateClose)reply({packet(0x03000162,0,2,1,{2})});}return 0;}
    if(type==0x03000193)comvCounts.push_back(p[8]);
    if(type==0x0300015b&&port==1){CHECK(p[8]==1&&payload==1&&etbs==0);++decodeOrderProperties;}
    if(type==Subscribe)CHECK(flags==3&&payload==11);
    else CHECK(type>0x03000000&&type<0x03ffffff&&flags==0);
    return 0;
  }
};
struct Fixture {
  Peer peer;Layout layout;SelfTest<Peer> tx;
  uint32_t sid=0x56361234;
  Fixture():tx(peer,layout) {CHECK(layout.init());tx.streaming=true;tx.checkGolden=false;tx.expectedPoc=0;tx.expectedProfile=1;tx.expectedLevel=4;}
  void start(uint64_t token=0x100000001ull) {CHECK(tx.nativeInput(sid,kAccessUnit,sizeof kAccessUnit,token));CHECK(tx.transaction()==0);CHECK(tx.r.psc==1&&peer.opens==1&&peer.drains==0&&peer.decodeOrderProperties==1);}
  void pump() {CHECK(tx.pump()==0);}
  void releaseAll() {SelfTest<Peer>::OutputFrame f;while(tx.popOutput(f))CHECK(tx.releaseOutput(f.index,f.token)==0);}
  void drain() {CHECK(tx.beginDrain()==0);pump();CHECK(tx.drainStatus()==1);}
  void close() {CHECK(tx.shutdown()==0);CHECK(peer.stops==2&&peer.closes==1&&tx.r.closed);}
};
static void ledgerTests() {
  stream::Ledger l;
  CHECK(l.freeInput()==0&&!l.find(1));
  CHECK(!l.submit(1,0)&&!l.ebd(0)&&!l.picture(1,0)&&!l.release(0,1)&&!l.retire(1));
  for(unsigned i=0;i<stream::Credits;++i)CHECK(l.accept(i+1));
  CHECK(!l.accept(99)&&!l.accept(1)&&l.pending==8&&l.highWater==8&&l.accepted==8);
  CHECK(l.submit(1,0)&&l.submit(2,1));CHECK(l.freeInput()==-1&&!l.submit(3,0)&&!l.submit(3,2));
  CHECK(l.picture(2,1)&&l.picture(1,0));CHECK(!l.picture(1,2)&&!l.picture(3,2));
  CHECK(l.ebd(1)&&!l.ebd(1));CHECK(l.freeInput()==1&&l.submit(3,1));CHECK(l.inputReuse==1);
  CHECK(!l.release(0,2)&&l.release(0,1)&&!l.release(0,1));
  CHECK(l.retire(1)&&!l.retire(1)&&l.pending==7);
  CHECK(l.ebd(0)&&l.ebd(1));
  CHECK(l.beginDrain()&&l.barrier==8&&!l.accept(10));CHECK(l.beginDrain()&&!l.drainReady());
  l.ack=true;CHECK(!l.drainReady());l.last=true;CHECK(l.drainReady()&&!l.drained());
  CHECK(l.release(1,2));for(unsigned i=2;i<=8;++i)CHECK(l.retire(i));
  CHECK(l.drained()&&l.paused&&!l.draining&&!l.drained());CHECK(l.beginDrain()&&l.paused);
  CHECK(l.resume()&&!l.resume()&&!l.ack&&!l.last);
  CHECK(l.accept(11)&&l.submit(11,0)&&l.picture(11,0)&&l.rawReuse==1);l.cancel();
  CHECK(l.stopped&&l.cancelled==1&&!l.pending&&!l.accept(12)&&!l.submit(11,1)&&!l.beginDrain());
  // Cancellation settles identities while physical loans remain owned until
  // the consumer releases them or final quiesce fences the entire arena.
  CHECK(!l.loansEmpty()&&l.release(0,11)&&!l.release(0,11)&&l.loansEmpty());
}
static void guestTests() {
  stream::Guest guest;
  constexpr uint32_t dbuf=0x44627566,demt=0x44656d74;
  CHECK(!guest.matches(100,dbuf)&&!guest.reply(100,dbuf)&&!guest.expired(999999));
  CHECK(!guest.begin(0,dbuf,10)&&!guest.begin(100,0,10)&&guest.begin(100,dbuf,10));
  CHECK(guest.deadline==2010&&guest.matches(100,dbuf)&&!guest.matches(99,dbuf)&&!guest.matches(100,demt));
  CHECK(!guest.begin(101,demt,20)&&!guest.reply(99,dbuf)&&!guest.reply(100,demt));
  CHECK(!guest.expired(2009)&&guest.expired(2010));
  CHECK(!(guest.matches(100,dbuf)&&!guest.expired(2010))); // actual handler rejects overdue replies
  guest.clear();CHECK(!guest.type&&!guest.request&&!guest.expired(9000));
  CHECK(guest.begin(101,dbuf,9000)&&guest.reply(101,dbuf)&&!guest.reply(101,dbuf)&&!guest.matches(101,dbuf));
  CHECK(!guest.expired(999999));
  // The next stage can replace a replied request, while stale/duplicate Dbuf
  // replies cannot settle the Demt acknowledgement for the same frame.
  CHECK(guest.begin(102,demt,9100)&&!guest.reply(101,dbuf)&&!guest.reply(102,dbuf)&&guest.reply(102,demt));
  guest.clear();CHECK(guest.begin(103,dbuf,9200));guest.cancel();
  CHECK(guest.stopped&&!guest.type&&!guest.reply(103,dbuf)&&!guest.begin(104,demt,9300));
  // New native epoch uses a distinct Guest object. Cancelling its predecessor
  // is irreversible, even if a new session starts its request sequence at100.
  stream::Guest replacement;CHECK(replacement.begin(100,dbuf,9300));CHECK(!guest.reply(100,dbuf));
  CHECK(replacement.reply(100,dbuf));replacement.clear();
  // A send failure leaves an owned pending stage until its deadline/cancel;
  // clearing it prematurely would make a late reply match a newer output.
  CHECK(replacement.begin(101,demt,10000)&&!replacement.begin(102,dbuf,10001));
  CHECK(!replacement.expired(11999)&&replacement.expired(12000));replacement.cancel();
  CHECK(!replacement.reply(101,demt)&&!replacement.expired(12001));
}
static void ownerTests() {
  stream::Owner o;uint32_t a=0,b=0;
  CHECK(o.acquire(nullptr)==-22&&o.acquire(&a)==0&&o.owns(a));
  CHECK(o.acquire(&b)==-16&&!o.owns(a+1)&&!o.start(a+1));CHECK(o.start(a)&&!o.start(a));
  CHECK(o.closing(a,true)==-16&&o.closing(a+1,false)==-22&&o.closing(a,false)==0&&!o.owns(a));
  CHECK(o.acquire(&b)==-16);o.closed(false);CHECK(o.phase==stream::Owner::Idle&&o.acquire(&b)==0&&b!=a);
  CHECK(o.closing(b,false)==0);o.closed(true);CHECK(o.phase==stream::Owner::Poisoned&&o.acquire(&a)==-16);
}
static void persistentTests() {
  Fixture f;f.start();f.pump();CHECK(f.tx.r.ebd==1);
  CHECK(f.peer.comvCounts==std::vector<uint32_t>({4,4})&&f.peer.comvSizes==std::vector<uint32_t>({172544,172544}));
  f.peer.autoEbd=false;
  const uint8_t au[]={0,0,0,1,0x41,0x9a,0x22};
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000002ull)==0);
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000003ull)==0);
  const auto etbs=f.tx.r.etb;
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000004ull)==1&&f.tx.r.etb==etbs&&f.tx.ledger.pending==3);
  f.peer.output(0x100000003ull,4);f.peer.output(0x100000002ull,2);f.peer.output(0x100000001ull,1);f.pump();
  CHECK(f.tx.r.decoded==3&&f.tx.outputCount==3&&f.tx.r.ebd==1);
  SelfTest<Peer>::OutputFrame a,b,c;
  CHECK(f.tx.popOutput(a)&&f.tx.popOutput(b)&&f.tx.popOutput(c)&&!f.tx.popOutput(c));
  CHECK(a.token==0x100000003ull&&b.token==0x100000002ull&&c.token==0x100000001ull);
  char before[65],after[65];auto *held=f.layout.find(Raw,a.index);f.peer.hw.hashActive(f.peer.dma.arena+held->off,before);
  CHECK(f.tx.releaseOutput(b.index,b.token)==0&&f.tx.releaseOutput(c.index,c.token)==0);
  // FBD never frees an input slot. Both DMA slots stay read-only to the host
  // until their independent, timestamp-free EBDs are parsed.
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000004ull)==1);
  f.peer.ebd(1);f.pump();CHECK(f.tx.r.ebd==2&&f.tx.ledger.inputOwned[0]&&!f.tx.ledger.inputOwned[1]);
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000004ull)==0);f.peer.output(0x100000004ull,2);f.pump();
  CHECK(f.tx.ledger.rawHeld[a.index]&&f.tx.ledger.pending==2);
  f.peer.hw.hashActive(f.peer.dma.arena+held->off,after);CHECK(!std::strcmp(before,after));
  f.releaseAll();CHECK(f.tx.releaseOutput(a.index,a.token)==0);
  f.peer.ebd(0);f.peer.ebd(1);f.pump();CHECK(f.tx.r.ebd==4&&f.tx.r.decoded==4);
  f.drain();CHECK(f.tx.r.drainAck&&f.tx.r.last&&f.peer.drains==1);
  CHECK(f.tx.beginDrain()==0&&f.tx.drainStatus()==1&&f.peer.drains==1);
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000005ull)==0);
  CHECK(f.peer.drainInputResume==1&&f.peer.drainRawResume==1&&!f.tx.r.last&&!f.tx.r.drainAck);
  f.peer.output(0x100000005ull,2);f.peer.ebd(0);f.pump();f.releaseAll();f.drain();
  CHECK(f.peer.opens==1&&f.peer.dpbs==4&&f.peer.settingsResume==1&&f.tx.r.psc==1);
  CHECK(f.tx.ledger.inputReuse>0&&f.tx.ledger.rawReuse>0&&f.tx.ledger.pending==0&&f.tx.ledger.loansEmpty());
  f.close();CHECK(f.peer.closes==1&&f.peer.drains==2);
  for(unsigned i=0;i<f.peer.headers.size();++i){CHECK(f.peer.headers[i]==i&&f.peer.packets[i]==i);}
}
static void firstPscTests() {
  for(unsigned n:{1u,9u,64u}){
    Fixture f;f.peer.n=n;f.start(77);f.peer.output(77,1);f.pump();f.releaseAll();f.drain();f.close();
    CHECK(f.tx.psc.n==n&&f.peer.dpbs==n&&f.peer.comvCounts==std::vector<uint32_t>({4,n}));
  }
  for(unsigned kind=0;kind<9;++kind){
    Fixture f;
    switch(kind){case 0:f.peer.n=0;break;case 1:f.peer.n=65;break;case 2:f.peer.pscPoc=2;break;
      case 3:f.peer.pscProfile=0;break;case 4:f.peer.pscLevel=5;break;case 5:f.peer.pscResolution=0x01400100;break;
      case 6:f.peer.pscDepth=0x000a000a;break;case 7:f.peer.pscCoded=0;break;case 8:f.peer.pscCrop1=1;break;}
    CHECK(f.tx.nativeInput(f.sid,kAccessUnit,sizeof kAccessUnit,78));CHECK(f.tx.transaction()!=0&&f.tx.r.error);
    CHECK(!f.tx.rawStarted&&f.peer.dpbs==0&&f.peer.ftbs==0&&f.peer.drains==0);
    CHECK(f.tx.shutdown()==0&&f.peer.stops==1&&f.peer.closes==1);
  }
}
static void omittedPscTests() {
  Fixture f;f.peer.omitOptional=true;
  f.tx.psc.poc=f.tx.expectedPoc;f.tx.psc.profile=f.tx.expectedProfile;f.tx.psc.level=f.tx.expectedLevel;
  f.start(97);CHECK(!(f.tx.psc.returned&((1u<<6)|(1u<<7)|(1u<<8))));
  CHECK(f.tx.psc.poc==0&&f.tx.psc.profile==1&&f.tx.psc.level==4);
  f.peer.output(97,8);f.pump();f.releaseAll();f.drain();f.close();
}
static void delayedGuestTests() {
  Fixture f;f.start(101);f.peer.output(101,2);f.pump();
  SelfTest<Peer>::OutputFrame held;CHECK(f.tx.popOutput(held));
  stream::Guest guest;CHECK(guest.begin(100,0x44627566,0));
  const uint8_t au[]={0,0,0,1,0x41,0x9a,0x22};
  for(unsigned i=102;i<108;++i){CHECK(f.tx.streamSubmit(au,sizeof au,i)==0);if(i<105)f.peer.output(i,4);f.pump();}
  CHECK(f.tx.r.ebd==7&&f.tx.r.decoded==4&&f.tx.ledger.pending==7&&f.tx.ledger.rawHeld[held.index]);
  // Guest has not replied, but HFI input ownership and output progress continue.
  CHECK(guest.matches(100,0x44627566)&&!guest.expired(1999));
  CHECK(guest.reply(100,0x44627566)&&guest.begin(101,0x44656d74,1999));
  CHECK(f.tx.releaseOutput(held.index,held.token)==0);f.releaseAll();
  CHECK(guest.reply(101,0x44656d74));guest.clear();
  for(unsigned i=105;i<108;++i){f.peer.output(i,4);f.pump();f.releaseAll();}
  CHECK(f.tx.r.decoded==7&&f.tx.ledger.pending==0);f.drain();f.close();
}
static void drainPictureTests() {
  Fixture f;f.start(91);f.peer.lastWithPicture=true;f.peer.finalToken=91;
  CHECK(f.tx.beginDrain()==0);f.pump();CHECK(f.tx.r.decoded==1&&f.tx.r.rawReturns==1&&f.tx.outputCount==1);
  CHECK(f.tx.drainStatus()==2&&!f.tx.ledger.paused);
  SelfTest<Peer>::OutputFrame out;CHECK(f.tx.popOutput(out)&&out.token==91);
  const auto ftbs=f.peer.ftbs;CHECK(f.tx.releaseOutput(out.index,out.token)==0&&f.peer.ftbs==ftbs);
  CHECK(f.tx.drainStatus()==1&&f.tx.ledger.paused&&f.tx.r.last&&f.tx.r.drainAck);f.close();
}
static void noOutputTests() {
  Fixture f;f.start(81);CHECK(f.tx.retireNoOutput(81)==-22);
  CHECK(f.tx.beginDrain()==0);f.pump();CHECK(f.tx.r.decoded==0&&f.tx.outputCount==0&&f.tx.drainStatus()==2);
  CHECK(f.tx.retireNoOutput(999)==-22&&f.tx.retireNoOutput(81)==0&&f.tx.retireNoOutput(81)==-22);
  CHECK(f.tx.drainStatus()==1&&f.tx.ledger.pending==0);f.close();
}
static void capacityTests() {
  Fixture f;f.start(1);f.pump();const uint8_t au[]={0,0,0,1,0x41,0x9a,0x22};
  for(unsigned token=2;token<=8;++token){CHECK(f.tx.streamSubmit(au,sizeof au,token)==0);f.pump();}
  CHECK(f.tx.ledger.pending==8&&f.tx.ledger.highWater==8&&f.tx.streamSubmit(au,sizeof au,9)==1&&f.tx.r.etb==8);
  // Credits count pictures, not EBDs. An idle but firmware-pending picture
  // still has a deadline; a completely idle session does not.
  CHECK(f.tx.ledger.inputsEmpty());f.peer.hw.clock+=5000001;CHECK(f.tx.pump()==kErrHfiTimeout);
  CHECK(f.peer.dma.arena==f.peer.arena.data());CHECK(f.tx.shutdown()==0);
}
static void negativeTests(unsigned kind) {
  Fixture f;f.start(0x123456789ull);f.pump();
  switch(kind){
    case 0:f.peer.output(999,2);break; // unknown identity
    case 1:f.peer.output(0x123456789ull,2);f.pump();f.peer.output(0x123456789ull,2);break; // duplicate frame on another RAW
    case 2:f.peer.output(0x123456789ull,2,0x10);break; // READONLY must never be recycled
    case 3:f.peer.psc();break; // in-place DRC deliberately unsupported
    case 4:f.peer.reply({Peer::packet(0x03000162,0,2,1,{2})},f.sid+1);break;
    case 5:f.peer.ebd(0,true);break;
    case 6:{auto *b=f.layout.find(Dpb,0);Words v={Dpb,0,b->iova(),0,0,b->size,0,0,0,0,0,0,0,0,0,0};f.peer.reply({Peer::packet(Buffers,0,2,5,v)});break;}
    case 7:f.peer.reply({Peer::packet(0x04000001,4,0,0)});break;
    case 8:f.peer.reply({Peer::packet(0x05000001,8,0,0)});break;
    case 9:f.peer.ops.watchdog=true;break;
    case 10:f.peer.ops.fault=true;break;
    case 11:f.peer.output(0x123456789ull,2,0x20000000);break; // PSC_LAST without supported DRC
    case 12:f.peer.output(0x123456789ull,0);break;
    case 13:f.peer.output(0x123456789ull,0x40);break; // HEVC picture kind
    case 14:{auto *b=f.layout.find(Raw,0);Words v={Raw,0,b->iova()+4096,0,0,b->size,0,kRawBytes,0x23456789,1,0,0,0,0,0,0};f.peer.reply({Peer::packet(Buffers,0,2,5,v),Peer::packet(0x03000162,0,2,1,{2})});break;}
    case 15:f.peer.output(0x123456789ull,2,0x10000000);break; // unsolicited LAST
  }
  CHECK(f.tx.pump()!=0&&f.tx.r.error!=0&&f.peer.dma.arena==f.peer.arena.data());
  f.tx.shutdown();CHECK(f.peer.stops==2&&f.peer.closes==1);
}
static void staleRetiredTests() {
  Fixture f;f.start(73);f.peer.output(73,2);f.pump();SelfTest<Peer>::OutputFrame out;CHECK(f.tx.popOutput(out));
  CHECK(f.tx.releaseOutput(out.index,out.token)==0&&f.tx.ledger.pending==0);
  // A requeued RAW index does not make a late token from its old generation
  // valid; the frame ledger, not RAW FIFO order, controls identity.
  f.peer.output(73,2,0,out.index);CHECK(f.tx.pump()!=0&&f.tx.r.error);f.tx.shutdown();
}
static void timeoutTests(bool ack) {
  Fixture f;f.start(71);f.peer.output(71,1);f.pump();f.releaseAll();
  f.peer.autoDrainAck=!ack;f.peer.autoDrainLast=ack;
  CHECK(f.tx.beginDrain()==0);f.pump();CHECK(f.tx.drainStatus()==0);
  f.peer.hw.clock+=5000001;CHECK(f.tx.pump()==kErrHfiTimeout&&f.tx.r.error==kErrHfiTimeout);
  CHECK(f.peer.dma.arena==f.peer.arena.data());f.tx.shutdown();
}
static void cancelTests() {
  Fixture f;f.start(51);f.peer.output(51,2);f.pump();SelfTest<Peer>::OutputFrame out;
  CHECK(f.tx.popOutput(out));const auto count=f.tx.r.decoded;
  // Entering shutdown is the engine's cancellation fence. Pending RAW/DPB and
  // loan metadata cannot authorize another success after that fence.
  f.tx.cleanup=true;f.tx.ledger.cancel();f.peer.output(999,2);CHECK(f.tx.pump()==0&&f.tx.r.decoded==count);
  CHECK(f.tx.ledger.stopped&&f.tx.ledger.cancelled==1);
  CHECK(f.tx.shutdown()==0&&f.peer.closes==1&&f.peer.dma.arena==f.peer.arena.data());
}
static void idleTests() {
  Fixture f;f.start(61);f.peer.output(61,1);f.pump();f.releaseAll();f.pump();
  CHECK(f.tx.deadline==UINT64_MAX);f.peer.hw.clock+=90000000;f.pump();CHECK(!f.tx.r.error);f.close();
}
static void admissionPublication() {
  stream::Ledger l;
  auto *f=l.accept(0x1234,1,false);
  CHECK(f&&f->phase==stream::Phase::Admitting&&l.pending==1);
  CHECK(!l.submit(0x1234,0)); // worker cannot DMA before the dfrm reply exists
  CHECK(l.publish(0x1234)&&!l.publish(0x1234)&&l.submit(0x1234,0));
  CHECK(!l.accept(0x1234,1,false));
  auto *failed=l.accept(0x5678,1,false);CHECK(failed&&failed->phase==stream::Phase::Admitting);
  failed->phase=stream::Phase::Emitted; // transport fallback owns this failed enqueue
  CHECK(!l.publish(0x5678)&&!l.submit(0x5678,1));
  l.cancel();CHECK(l.pending==0&&!l.publish(0x5678));
}
int main() {
  admissionPublication();
  ledgerTests();guestTests();ownerTests();persistentTests();firstPscTests();omittedPscTests();delayedGuestTests();drainPictureTests();noOutputTests();capacityTests();
  for(unsigned i=0;i<16;++i)negativeTests(i);
  staleRetiredTests();timeoutTests(true);timeoutTests(false);cancelTests();idleTests();
  std::printf("V6 persistent host: %s (real HFI parser/ledger; scripted peer, never hardware evidence)\n",failures?"FAIL":"PASS");return failures?1:0;
}
