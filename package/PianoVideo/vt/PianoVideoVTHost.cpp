// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Native V5a endpoint of the approved UniXNU VT frontend (vt/UPSTREAM).
// This RAM qualification endpoint is one exclusive Baseline320x240 IDR AU,
// terminal drain, no tile/encoder/HEVC/streaming capability. Selector3 only
// copies/validates/enqueues. A serialized worker owns Iris and all guest waits.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#include "PianoVideoVTWire.h"
#include "PianoVideoVTContract.h"
using namespace vtwire;
using pianovideo::native::Frame;
namespace contract = pianovideo::vtcontract;
extern "C" {
volatile uint32_t droidvm_vthost_sessions, droidvm_vthost_frames_in, droidvm_vthost_frames_out, droidvm_vthost_errors;
volatile uint32_t droidvm_vthost_error_stage, droidvm_vthost_replies, droidvm_vthost_unknown, droidvm_vthost_last_unknown;
volatile int32_t droidvm_vthost_error_value;
int proc_selfpid(void);
}
namespace {
constexpr int kBadData = -12909, kUnsupported = -12910, kBadState = -12903, kDecodeError = -12911, kNoMemory = -12904;
struct Client;
struct Session {
  Client *client = nullptr;
  uint8_t uuid[16] = {};
  uint32_t native = 0;
  IOLock *lock = nullptr;
  contract::State state;
  uint8_t *input = nullptr;
  uint32_t inputBytes = 0;
  int64_t token = 0;
  bool inputReady = false, busy = false, workerRunning = false, stopping = false;
  bool finishWanted = false;
  uint64_t finishRequest = 0;
  int firstError = 0, closeError = 0;
  uint64_t nextRequest = 100, awaitedRequest = 0;
  uint32_t awaitedType = 0;
  bool replied = false, replyHasSurface = false;
  int replyError = 0;
  uint8_t replyUuid[16] = {};
  contract::Surface replySurface;
  IOUserClient *surfaces = nullptr;
};
struct Client {
  void *transport = nullptr;
  bool (*send)(void *, const uint8_t *, uint32_t) = nullptr;
  task_t task = nullptr;
  int pid = 0;
  IOLock *lock = nullptr;
  Session *session = nullptr;
};
void failure(Session *s, uint32_t stage, int error) {
  droidvm_vthost_errors = droidvm_vthost_errors + 1;
  droidvm_vthost_error_stage = stage;
  droidvm_vthost_error_value = error;
  if (s) { IOLockLock(s->lock); if (!s->firstError) s->firstError = error; IOLockUnlock(s->lock); }
  IOLog("PianoVideoVT V5a: first-error stage %u status %d session %u\n", stage, error, s ? s->native : 0);
}
bool send(Client *c, const Writer &w, const uint8_t *b) {
  const bool ok = w.ok() && c->send(c->transport,b,w.size());
  if (ok) droidvm_vthost_replies = droidvm_vthost_replies + 1;
  else failure(nullptr,20,kDecodeError);
  return ok;
}
bool status(Client *c, const Header &h, int error) {
  uint8_t b[128]; Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),error);
  IOLog("PianoVideoVT V5a: reply %c%c%c%c request %llu status %d\n",int(h.type>>24),int((h.type>>16)&255),int((h.type>>8)&255),int(h.type&255),h.request,error);
  return send(c,w,b);
}
void hash(const uint8_t *b, uint32_t n, char out[65]) {
  SHA256_CTX ctx;SHA256_Init(&ctx);SHA256_Update(&ctx,b,n);uint8_t digest[32];SHA256_Final(digest,&ctx);
  const char hex[]="0123456789abcdef";for(unsigned i=0;i<32;++i){out[2*i]=hex[digest[i]>>4];out[2*i+1]=hex[digest[i]&15];}out[64]=0;
}
IOService *surfaceRoot() {
  OSDictionary *matching=IOService::nameMatching("IOSurfaceRoot");
  IOService *root=matching?IOService::waitForMatchingService(matching,1000000000ULL):nullptr;
  OSSafeReleaseNULL(matching);return root;
}
bool lookupSurface(Session *s,uint32_t id,contract::Surface &v) {
  constexpr uint32_t bytes=0xc68;
  if (!s->surfaces) {
    IOService *root=surfaceRoot();
    const IOReturn rc=root?root->newUserClient(s->client->task,s->client->task,0,nullptr,&s->surfaces):kIOReturnNotFound;
    OSSafeReleaseNULL(root);
    IOLog("PianoVideoVT V5a: IOSurfaceRoot type0 pid %d status %08x client %u\n",s->client->pid,rc,unsigned(s->surfaces!=nullptr));
    if(rc || !s->surfaces){failure(s,40,int(rc));return false;}
  }
  uint8_t *result=static_cast<uint8_t *>(IOMallocZero(bytes));if(!result){failure(s,41,kNoMemory);return false;}
  uint64_t scalar=id;
  IOExternalMethodArguments a={};a.version=kIOExternalMethodArgumentsCurrentVersion;a.selector=4;a.scalarInput=&scalar;a.scalarInputCount=1;
  a.structureOutput=result;a.structureOutputSize=bytes;
  IOUserClient2022 *newClient=OSDynamicCast(IOUserClient2022,s->surfaces);
  const IOReturn rc=newClient?newClient->externalMethod(4,reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&a)):
    s->surfaces->callExternalMethod(4,&a);
  bool ok=!rc && a.structureOutputSize==bytes;
  if(ok){
    auto u64=[&](uint32_t at){uint64_t x;memcpy(&x,result+at,8);return x;};
    auto u32=[&](uint32_t at){uint32_t x;memcpy(&x,result+at,4);return x;};
    v.base=u64(0);v.id=u32(0x18);v.size=u64(0x20);v.format=u32(0x48);v.planes=u32(0x58);
    for(unsigned p=0;p<2;++p){const uint32_t at=0x88+p*0x98;v.plane[p]={u64(at),u64(at+8),u64(at+0x18),u64(at+0x20),u32(at+0x6c)};}
    ok=v.id==id && contract::surface(v);
  }
  IOLog("PianoVideoVT V5a: surface lookup id %u status %08x result-bytes %u format %08x planes %u valid %u\n",id,rc,unsigned(a.structureOutputSize),v.format,v.planes,unsigned(ok));
  IOFree(result,bytes);if(!ok)failure(s,42,rc?int(rc):kBadData);return ok;
}
bool copyFrame(Session *s,const Frame &f,const contract::Surface &v) {
  if(!f.memory || !f.cpu || !contract::layout(f.layout,f.length) || !contract::surface(v)){failure(s,60,kBadData);return false;}
  IOMemoryDescriptor *mem=IOMemoryDescriptor::withAddressRange(v.base,v.size,kIODirectionInOut,s->client->task);
  const IOReturn prepared=mem?mem->prepare():kIOReturnNoMemory;
  if(prepared){OSSafeReleaseNULL(mem);failure(s,61,int(prepared));return false;}
  IOMemoryMap *map=mem->createMappingInTask(kernel_task,0,kIOMapAnywhere);
  uint8_t *dst=map?reinterpret_cast<uint8_t *>(map->getVirtualAddress()):nullptr;
  const bool ok=dst && map->getLength()>=v.size;
  if(ok){
    const auto &l=f.layout;
    // WC source reads only; never create a cached arena alias or treat IOVA as PA.
    for(unsigned p=0;p<2;++p){
      const uint32_t off=p?l.uvOffset:l.yOffset,rows=p?l.height/2:l.height;
      for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<l.width;++x)
        dst[v.plane[p].offset+uint64_t(y)*v.plane[p].stride+x]=f.cpu[off+uint64_t(y)*l.stride+x];
    }
    __asm__ volatile("dsb sy" ::: "memory");
    IOLog("PianoVideoVT V5a: copy success token %llu active 320x240 bytes 115200 stride %u scanlines %u uv-offset %u IOSurface %u\n",f.token,l.stride,l.scanlines,l.uvOffset,v.id);
  }else failure(s,62,kNoMemory);
  OSSafeReleaseNULL(map);mem->complete();mem->release();return ok;
}
// Worker only. Its waits end at cancellation as well as timeout. Guest reply
// handling never takes Client::lock, allowing an outstanding request to settle.
bool request(Session *s,Writer &w,const uint8_t *b,uint64_t id,uint32_t type,bool wait=true) {
  IOLockLock(s->lock);s->awaitedRequest=id;s->awaitedType=type;s->replied=false;s->replyHasSurface=false;s->replyError=kDecodeError;
  IOLockUnlock(s->lock);
  const bool sent=send(s->client,w,b);
  IOLockLock(s->lock);
  uint64_t until;clock_interval_to_deadline(2000,kMillisecondScale,&until);
  while(sent && wait && !s->replied && !s->stopping)
    if(IOLockSleepDeadline(s->lock,&s->replied,until,THREAD_UNINT)==THREAD_TIMED_OUT)break;
  const bool ok=sent && (!wait || (s->replied && !s->replyError));
  s->awaitedRequest=0;s->awaitedType=0;IOLockUnlock(s->lock);
  if(!ok && !s->stopping)failure(s,30,kDecodeError);
  return ok;
}
void completeFrame(Session *s,int error,const uint8_t *uuid=nullptr,bool wait=true) {
  IOLockLock(s->lock);
  if(!s->state.complete()){IOLockUnlock(s->lock);return;}
  if(s->state.cancelled && !error)error=kBadState;
  IOLockUnlock(s->lock);
  uint8_t b[192];const uint64_t id=s->nextRequest++;Writer w(b,sizeof b);
  w.header(cc("Demt"),FlagHostRequest,s->uuid,id);w.s64(cc(",frm"),s->token);w.s32(cc(",err"),error);w.u32(cc(",inf"),error?2:1);
  if(uuid)w.uuid(cc(",uui"),uuid);
  const bool delivered=request(s,w,b,id,cc("Demt"),wait);
  if(!error && delivered)droidvm_vthost_frames_out=droidvm_vthost_frames_out+1;
  IOLog("PianoVideoVT V5a: Demt token %llu status %d transport %u completed-once 1\n",uint64_t(s->token),error,unsigned(delivered));
}
void frame(Session *s) {
  Frame f;const int rc=piano_video_native_decode(s->native,s->input,s->inputBytes,uint64_t(s->token),&f);
  IOFree(s->input,s->inputBytes);s->input=nullptr;s->inputBytes=0;
  IOLockLock(s->lock);bool cancelled=s->stopping;IOLockUnlock(s->lock);
  if(rc || cancelled){if(rc)failure(s,70,rc);completeFrame(s,rc?kDecodeError:kBadState,nullptr,!cancelled);}
  else{
    uint8_t b[128];const uint64_t id=s->nextRequest++;Writer w(b,sizeof b);w.header(cc("Dbuf"),FlagHostRequest,s->uuid,id);w.s64(cc(",frm"),s->token);
    const bool got=request(s,w,b,id,cc("Dbuf"));
    if(!got || !s->replyHasSurface)completeFrame(s,kDecodeError);
    else {
      uint8_t uuid[16];memcpy(uuid,s->replyUuid,16);const contract::Surface v=s->replySurface;
      IOLockLock(s->lock);cancelled=s->stopping;IOLockUnlock(s->lock);
      const bool copied=!cancelled && copyFrame(s,f,v);
      completeFrame(s,copied?0:kDecodeError,uuid,!cancelled);
    }
  }
  piano_video_native_frame_release(s->native,&f);
}
void worker(void *arg,wait_result_t) {
  Session *s=static_cast<Session *>(arg);thread_set_thread_name(current_thread(),"piano-video-vt");
  IOLockLock(s->lock);
  for(;;){
    while(!s->stopping && !s->inputReady && !s->finishWanted)IOLockSleep(s->lock,&s->busy,THREAD_UNINT);
    if(s->stopping)break;
    if(s->inputReady){s->inputReady=false;s->busy=true;IOLockUnlock(s->lock);frame(s);IOLockLock(s->lock);s->busy=false;continue;}
    if(s->finishWanted){
      const uint64_t finish=s->finishRequest;const bool accepted=s->state.accepted;s->finishWanted=false;s->state.finish();IOLockUnlock(s->lock);
      const int nativeRC=accepted?piano_video_native_finish(s->native):0;if(nativeRC)failure(s,71,nativeRC);
      const int rc=nativeRC?nativeRC:s->firstError;
      Header h;h.type=cc("dfin");h.request=finish;memcpy(h.uuid,s->uuid,16);status(s->client,h,rc?kDecodeError:0);
      IOLog("PianoVideoVT V5a: terminal dfin session %u status %d no-restart 1\n",s->native,rc);IOLockLock(s->lock);
    }
  }
  s->state.cancel();const bool pending=s->state.accepted&&!s->state.completed;
  const bool finish=s->finishWanted;const uint64_t finishID=s->finishRequest;s->finishWanted=false;
  IOLockUnlock(s->lock);
  if(pending)completeFrame(s,kBadState,nullptr,false);
  if(finish){Header h;h.type=cc("dfin");h.request=finishID;memcpy(h.uuid,s->uuid,16);status(s->client,h,kBadState);}
  if(s->input){IOFree(s->input,s->inputBytes);s->input=nullptr;s->inputBytes=0;}
  s->closeError=piano_video_native_close(s->native);
  IOLog("PianoVideoVT V5a: worker close session %u status %d pending-settled 1\n",s->native,s->closeError);
  IOLockLock(s->lock);s->native=0;s->workerRunning=false;IOLockWakeup(s->lock,&s->workerRunning,false);IOLockUnlock(s->lock);
  thread_terminate(current_thread());
}
void teardown(Session *s) {
  IOLockLock(s->lock);s->state.cancel();s->stopping=true;
  IOLockWakeup(s->lock,&s->busy,false);IOLockWakeup(s->lock,&s->replied,false);
  while(s->workerRunning)IOLockSleep(s->lock,&s->workerRunning,THREAD_UNINT);
  IOLockUnlock(s->lock);
  if(s->native){s->closeError=piano_video_native_close(s->native);s->native=0;}
  if(s->surfaces){s->surfaces->clientClose();OSSafeReleaseNULL(s->surfaces);}
  IOLog("PianoVideoVT V5a: invalidate safe worker-joined 1 surfaces-released 1 close-status %d\n",s->closeError);
}
void destroy(Session *s){if(!s)return;teardown(s);IOLockFree(s->lock);delete s;}
bool descriptor(const uint8_t *m,uint32_t n,ImageDescription &d) {
  Param p;return param(m,n,cc(",fds"),p)&&p.type==cc("=fds")&&imageDescription(p.value,p.size,d)&&be32(p.value)==p.size&&contract::format(d);
}
void attributes(Session *s) {
  Plist *p=new Plist();if(!p){failure(s,10,kNoMemory);return;}
  uint8_t b[640];const int fmt=p->integer(cc("420v"));
  const int keys[]={p->string("Width"),p->string("Height"),p->string("PixelFormatType"),p->string("BytesPerRowAlignment"),p->string("PlaneAlignment"),p->string("IOSurfaceProperties")};
  const int values[]={p->integer(320),p->integer(240),p->array(&fmt,1),p->integer(64),p->integer(256),p->dict(nullptr,nullptr,0)};
  const int attrs=p->dict(keys,values,6);Writer w(b,sizeof b);w.header(cc("Dpba"),0,s->uuid,0);w.plist(cc(",pba"),*p,attrs);send(s->client,w,b);delete p;
  IOLog("PianoVideoVT V5a: Dpba active 320x240 420v IOSurface pool\n");
}
int start(Session *s,const uint8_t *m,uint32_t n) {
  ImageDescription d;if(!descriptor(m,n,d))return kUnsupported;
  IOLockLock(s->lock);const bool started=s->state.started||s->state.cancelled;IOLockUnlock(s->lock);if(started)return kBadState;
  const int rc=piano_video_native_open(&s->native);if(rc)return kDecodeError;
  s->state.started=true;s->workerRunning=true;thread_t t=nullptr;
  if(kernel_thread_start(worker,s,&t)!=KERN_SUCCESS){s->workerRunning=false;s->state.cancel();piano_video_native_close(s->native);s->native=0;return kNoMemory;}
  thread_deallocate(t);attributes(s);
  IOLog("PianoVideoVT V5a: dsta native session %u exact-format 320x240 Baseline one-AU\n",s->native);return 0;
}
int queue(Session *s,const uint8_t *m,uint32_t n) {
  Param p;Atom sbuf,sdat;int64_t token=0;
  if(!paramS64(m,n,cc(",frm"),token)||!param(m,n,cc(",sbf"),p)||p.type!=cc("=sbf")||
     !contract::unique(p.value,p.size,cc("sbuf"),sbuf)||!contract::unique(sbuf.body,sbuf.size,cc("sdat"),sdat)||!contract::sample(sdat.body,sdat.size)){
    IOLog("PianoVideoVT V5a: input rejected malformed-or-unsupported before-DMA 1\n");return kBadData;
  }
  const uint32_t bytes=sdat.size+contract::kParameterBytes;
  uint8_t *input=static_cast<uint8_t *>(IOMalloc(bytes));if(!input)return kNoMemory;
  const uint32_t normalized=contract::normalize(sdat.body,sdat.size,input,bytes);
  if(!normalized){IOFree(input,bytes);return kBadData;}
  char sampleHash[65],auHash[65];hash(sdat.body,sdat.size,sampleHash);hash(input,normalized,auHash);
  IOLockLock(s->lock);
  if(!s->state.accept()){IOLockUnlock(s->lock);IOFree(input,bytes);IOLog("PianoVideoVT V5a: input rejected second-AU-or-terminal before-DMA 1\n");return kBadState;}
  s->input=input;s->inputBytes=bytes;s->token=token;s->inputReady=true;
  IOLockUnlock(s->lock);
  IOLog("PianoVideoVT V5a: sample token %llu bytes %u SHA256 %s\n",uint64_t(token),sdat.size,sampleHash);
  IOLog("PianoVideoVT V5a: normalized token %llu bytes %u SHA256 %s native-session %u\n",uint64_t(token),normalized,auHash,s->native);
  droidvm_vthost_frames_in=droidvm_vthost_frames_in+1;IOLockWakeup(s->lock,&s->busy,false);return 0;
}
void inventory(Client *c,const Header &h) {
  Plist *p=new Plist();if(!p){status(c,h,kNoMemory);return;}uint8_t b[2048];int entry=0;
  if(h.type==cc("decs")){
    const int type=p->string("avc1");const int keys[]={p->string("VTCodecType"),p->string("CMClassImplementationName"),p->string("IsPreferredInternalPlugin"),p->string("VTRating"),p->string("CMClassImplementationID")};
    const int values[]={p->array(&type,1),p->string("PianoVideo native AVC V5a qualification"),p->boolean(false),p->integer(400),p->string("com.unixnu.pianovideo.decoder.avc")};entry=p->dict(keys,values,5);
  }
  const int list=p->array(&entry,h.type==cc("decs")&&piano_video_native_ready()?1:0);
  Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.plist(h.type==cc("decs")?cc(",dec"):cc(",enc"),*p,list);w.u32(cc(",ver"),6);send(c,w,b);delete p;
  IOLog("PianoVideoVT V5a: inventory version6 avc1-only qualification encoders0\n");
}
void properties(Client *c,const Header &h,bool value) {
  Plist *p=new Plist();if(!p){status(c,h,kNoMemory);return;}uint8_t b[512];int root;
  if(value){
    Session *s=c->session;bool hardware=false;
    if(s){IOLockLock(s->lock);hardware=s->state.started&&!s->state.cancelled;IOLockUnlock(s->lock);}
    root=p->boolean(hardware);
  }
  else{
    const int keys[]={p->string("PropertyType"),p->string("ReadWriteStatus")};const int values[]={p->string("Boolean"),p->string("ReadOnly")};
    const int name=p->string("UsingHardwareAcceleratedVideoDecoder"),type=p->dict(keys,values,2);root=p->dict(&name,&type,1);
  }
  Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.plist(value?cc(",val"):cc(",sup"),*p,root);send(c,w,b);delete p;
}
}
extern "C" {
bool droidvm_vthost_available(void){return piano_video_native_ready();}
void *droidvm_vthost_client_open(void *transport,bool (*sendFn)(void *,const uint8_t *,uint32_t),task_t task,int pid) {
  Client *c=new Client();if(!c)return nullptr;c->lock=IOLockAlloc();if(!c->lock){delete c;return nullptr;}
  c->transport=transport;c->send=sendFn;c->task=task;c->pid=pid;return c;
}
void droidvm_vthost_client_close(void *host){Client *c=static_cast<Client *>(host);if(!c)return;destroy(c->session);IOLockFree(c->lock);delete c;}
int droidvm_vthost_message(void *host,const uint8_t *m,uint32_t n,const uint32_t *ids,unsigned surfaces) {
  Client *c=static_cast<Client *>(host);Header h;if(!c||!contract::message(m,n)||!header(m,n,h))return -1;
  Session *s=c->session;
  if(h.flags==FlagGuestReply){
    if(!s||memcmp(s->uuid,h.uuid,16))return -1;
    IOLockLock(s->lock);const bool expected=s->awaitedRequest==h.request&&s->awaitedType==h.type&&!s->replied&&!s->stopping;IOLockUnlock(s->lock);
    if(!expected)return -1;
    int32_t error=0;paramS32(m,n,cc(",err"),error);Param uuid;contract::Surface surface;bool hasSurface=false;
    if(h.type==cc("Dbuf")&&!error){
      hasSurface=surfaces==1&&param(m,n,cc(",uui"),uuid)&&uuid.type==cc("=uui")&&uuid.size==16&&lookupSurface(s,ids[0],surface);
      if(!hasSurface)error=kDecodeError;
    }
    IOLockLock(s->lock);
    if(s->awaitedRequest==h.request&&!s->stopping){s->replyError=error;s->replyHasSurface=hasSurface;if(hasSurface){s->replySurface=surface;memcpy(s->replyUuid,uuid.value,16);}s->replied=true;IOLockWakeup(s->lock,&s->replied,false);}
    IOLockUnlock(s->lock);
    IOLog("PianoVideoVT V5a: guest reply %c%c%c%c request %llu status %d surface %u\n",int(h.type>>24),int((h.type>>16)&255),int((h.type>>8)&255),int(h.type&255),h.request,error,unsigned(hasSurface));return 0;
  }
  if(h.flags!=FlagRequest&&h.flags!=0)return -1;
  IOLockLock(c->lock);
  s=c->session;const bool owned=s&&!memcmp(s->uuid,h.uuid,16);
  if(h.type==cc("decs")||h.type==cc("encs"))inventory(c,h);
  else if(h.type==cc("+dec")){
    int32_t codec=0;
    if(c->session||!paramS32(m,n,cc(",cod"),codec)||uint32_t(codec)!=cc("avc1"))status(c,h,kUnsupported);
    else{
      s=new Session();if(s)s->lock=IOLockAlloc();
      if(!s||!s->lock){delete s;status(c,h,kNoMemory);}
      else{
        s->client=c;memcpy(s->uuid,h.uuid,16);c->session=s;droidvm_vthost_sessions=droidvm_vthost_sessions+1;
        Plist *p=new Plist();if(!p)status(c,h,kNoMemory);else{
          uint8_t b[320];const int key=p->string("VTHostBuildVersion"),value=p->string("PianoVideo native V5a");const int info=p->dict(&key,&value,1);
          Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.u32(cc(",ver"),6);w.plist(cc(",hin"),*p,info);send(c,w,b);delete p;
        }
        IOLog("PianoVideoVT V5a: +dec selected com.unixnu.pianovideo.decoder.avc pid %d\n",c->pid);
      }
    }
  }else if(!owned){if(h.flags&FlagRequest)status(c,h,kBadState);}
  else switch(h.type){
    case cc("dsta"):status(c,h,start(s,m,n));break;
    case cc("dcan"):{ImageDescription d;IOLockLock(s->lock);const bool active=s->state.started&&!s->state.cancelled;IOLockUnlock(s->lock);const bool can=active&&descriptor(m,n,d);uint8_t b[128];Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.s32(cc(",can"),can?1:0);send(c,w,b);IOLog("PianoVideoVT V5a: dcan ,can %u side-effect-free 1\n",unsigned(can));break;}
    case cc("dsts"):case cc("dtil"):status(c,h,kUnsupported);IOLog("PianoVideoVT V5a: tiles rejected before-DMA 1\n");break;
    case cc("dfrm"):{const int error=queue(s,m,n);uint8_t b[128];Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),error);w.u32(cc(",inf"),error?0:1);send(c,w,b);break;}
    case cc("dfin"):{IOLockLock(s->lock);const bool schedule=s->workerRunning&&!s->stopping&&!s->finishWanted&&!s->state.terminal;if(schedule){s->finishWanted=true;s->finishRequest=h.request;s->state.finish();IOLockWakeup(s->lock,&s->busy,false);}IOLockUnlock(s->lock);if(!schedule)status(c,h,s->state.terminal?0:kBadState);break;}
    case cc("dinv"):teardown(s);status(c,h,s->closeError?kDecodeError:0);break;
    case cc("-dec"):c->session=nullptr;destroy(s);break;
    case cc("dsup"):properties(c,h,false);break;
    case cc("dcop"):{Param p;char key[96]="";if(param(m,n,cc(",key"),p)&&plistString(p.value,p.size,key,sizeof key)&&!strcmp(key,"UsingHardwareAcceleratedVideoDecoder"))properties(c,h,true);else status(c,h,-12900);break;}
    case cc("dset"):case cc("dmul"):status(c,h,0);break;
    default:droidvm_vthost_unknown=droidvm_vthost_unknown+1;droidvm_vthost_last_unknown=h.type;if(h.flags&FlagRequest)status(c,h,-12900);break;
  }
  IOLockUnlock(c->lock);return 0;
}
}
