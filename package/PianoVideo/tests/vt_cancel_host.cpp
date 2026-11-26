// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Execute the production worker/ingress/emitter with scripted IOKit and native
// ownership. The peer models the 27.3 Demt callback target and dinv reply race;
// it is not an Apple userspace or firmware integration test.
#include "vt_cancel_kernel_shim.h"
#include <deque>
#include <map>
#include <vector>
#include "../vt/PianoVideoVTHost.cpp"

struct NativeScript {
  std::atomic<unsigned> loans{0},releases{0},closes{0},submits{0};
  std::atomic<bool> cancelled{false};
  void reset(){loans=0;releases=0;closes=0;submits=0;cancelled=false;}
} native;
extern "C" {
bool piano_video_native_ready(){return true;}
int piano_video_native_open(uint32_t *s){*s=0x56360021;return 0;}
int piano_video_native_prepare(uint32_t,const pianovideo::codec::Config *){return 0;}
int piano_video_native_submit(uint32_t,const uint8_t *,uint32_t,uint64_t,const pianovideo::codec::Config *){++native.submits;return 1;}
int piano_video_native_poll(uint32_t,pianovideo::native::Frame *){return 1;}
int piano_video_native_frame_release(uint32_t,pianovideo::native::Frame *f){
  assert(f->memory&&native.loans==1);--native.loans;++native.releases;
  f->memory->release();*f={};return 0;
}
void piano_video_native_cancel(uint32_t){native.cancelled=true;}
int piano_video_native_close(uint32_t){assert(native.cancelled&&!native.loans);++native.closes;return 0;}
}

struct LookupGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered=false,proceed=false;
  unsigned held=0,invalidated=0,released=0,closed=0;
} lookup;
struct TestSurface:IOUserClient {
  IOReturn callExternalMethod(uint32_t selector,IOExternalMethodArguments *a) override {
    std::unique_lock<std::mutex> l(lookup.mutex);
    assert(a->scalarInputCount==1&&a->scalarInput[0]==69);
    if(selector==4){
      lookup.entered=true;lookup.cv.notify_all();
      assert(lookup.cv.wait_for(l,std::chrono::seconds(3),[]{return lookup.proceed;}));
      assert(!lookup.held);++lookup.held;
      assert(a->structureOutput&&a->structureOutputSize==0xc68);
      auto *b=static_cast<uint8_t *>(a->structureOutput);std::memset(b,0,0xc68);
      auto put=[&](unsigned at,uint64_t v,unsigned n=8){std::memcpy(b+at,&v,n);};
      put(0,1);put(0x18,69,4);put(0x20,115200);put(0x48,cc("420v"),4);put(0x58,2,4);
      put(0x88,320);put(0x90,240);put(0xa8,320);
      put(0x120,160);put(0x128,120);put(0x138,76800);put(0x140,320);
    }else if(selector==56){assert(lookup.held==1);++lookup.invalidated;}
    else if(selector==1){assert(lookup.held==1&&lookup.invalidated==1);--lookup.held;++lookup.released;}
    else std::abort();
    return 0;
  }
  IOReturn clientClose() override {std::lock_guard<std::mutex> l(lookup.mutex);assert(!lookup.held);++lookup.closed;return 0;}
};

struct Harness {
  std::mutex queueMutex,selector;
  std::condition_variable cv;
  std::deque<std::vector<uint8_t>> messages;
  Client *client=nullptr;
  std::map<uint64_t,unsigned> completions;
  std::vector<Header> terminalHeaders;
  unsigned pending=0,images=0,errors=0,dinvReplies=0,dfinReplies=0,originalErrors=0;
  int dinvStatus=999,dfinStatus=999;
  bool target=true,invalidating=false,holdAck=false,dropAck=false,failSend=false;
  uint64_t epoch=20;
  std::thread workerThread;
  Harness(){client=static_cast<Client *>(droidvm_vthost_client_open(this,enqueue,nullptr,1));assert(client);create();}
  ~Harness(){assert(!workerThread.joinable());droidvm_vthost_client_close(client);}
  Session *session(){return client->session;}
  static bool enqueue(void *p,const uint8_t *b,uint32_t n){
    auto &h=*static_cast<Harness *>(p);Header header;assert(vtwire::header(b,n,header));
    if(h.failSend&&header.type==cc("Demt"))return false;
    std::lock_guard<std::mutex> l(h.queueMutex);h.messages.emplace_back(b,b+n);h.cv.notify_all();return true;
  }
  int ingress(const Writer &w,const uint8_t *b){
    // Models Apple's global send mutex and the UC's selector lock. The real
    // dinv handler must return without waiting for worker/guest reentry.
    std::lock_guard<std::mutex> l(selector);return droidvm_vthost_message(client,b,w.size(),nullptr,0);
  }
  void requestType(uint32_t type,uint64_t id){
    uint8_t b[128];Writer w(b,sizeof b);w.header(type,FlagRequest,session()->uuid,id);assert(!ingress(w,b));
  }
  void create(){
    uint8_t b[128],uuid[16]={};std::memcpy(uuid,&epoch,sizeof epoch);Writer w(b,sizeof b);
    w.header(cc("+dec"),FlagRequest,uuid,1);w.s32(cc(",cod"),int32_t(cc("avc1")));assert(!ingress(w,b));
    auto *s=session();assert(s);s->started=true;s->native=0x56360020+uint32_t(epoch-20);
    s->config.s.width=320;s->config.s.height=240;
  }
  uint64_t token(unsigned i)const{return (epoch<<32)|i;}
  void admit(unsigned i,uint32_t flags=9,bool held=false){
    auto *s=session();IOLockLock(s->lock);auto *f=s->frames.accept(token(i),flags,false);assert(f);
    f->completion.begin(flags,100+i,nowMs());++pending;completions[token(i)]=0;
    if(flags&1){if(held){auto &in=s->inputs[unsigned(f-s->frames.frames)];in.reply=100+i;in.deadline=nowMs()+900;}
      else assert(answerAdmission(s,*f,100+i));}
    IOLockUnlock(s->lock);
  }
  int acknowledge(const Header &h,int error=0){
    uint8_t b[128];Writer w(b,sizeof b);w.header(h.type,FlagGuestReply,h.uuid,h.request);
    if(error!=INT32_MAX)w.s32(cc(",err"),error);return ingress(w,b);
  }
  bool pump(bool wait=true,int ackError=0){
    std::vector<uint8_t> b;
    {std::unique_lock<std::mutex> l(queueMutex);
      if(wait)assert(cv.wait_for(l,std::chrono::seconds(3),[&]{return !messages.empty();}));
      if(messages.empty())return false;b=std::move(messages.front());messages.pop_front();}
    Header h;assert(header(b.data(),uint32_t(b.size()),h));
    int32_t status=0;paramS32(b.data(),uint32_t(b.size()),cc(",err"),status);
    if(h.type==cc("Demt")){
      int64_t t=0;int32_t flags=0;Param uuid;
      assert(h.flags==FlagHostRequest&&paramS64(b.data(),uint32_t(b.size()),cc(",frm"),t));
      assert(paramS32(b.data(),uint32_t(b.size()),cc(",inf"),flags));
      const bool image=param(b.data(),uint32_t(b.size()),cc(",uui"),uuid);
      assert(target&&completions.count(uint64_t(t))&&completions[uint64_t(t)]++==0&&pending);--pending;
      if(status){
        assert(invalidating&&status==kDecodeError&&flags==3&&!image&&b.size()==112);
        assert(native.cancelled&&native.closes==1&&!native.loans);
        {std::lock_guard<std::mutex> l(lookup.mutex);assert(!lookup.held);}
        ++errors;terminalHeaders.push_back(h);
      }else{assert(!invalidating&&image&&flags==1);++images;}
      if(holdAck||dropAck)return true;
      assert(!acknowledge(h,ackError));
    }else if(h.type==cc("dinv")){
      assert(h.flags==FlagReply);++dinvReplies;dinvStatus=status;
      if(!status)assert(!pending);target=false;
    }else if(h.type==cc("dfin")){++dfinReplies;dfinStatus=status;}
    else if(h.type==cc("dfrm")&&status){assert(status==kDecodeError);++originalErrors;assert(pending);--pending;}
    return true;
  }
  void drain(){while(pump(false)){};}
  void prime(unsigned i){
    admit(i);auto *s=session();s->current=s->currentAccepted=true;s->currentToken=token(i);
    const uint8_t uuid[16]={1};emitFrame(s,0,uuid);drain();
    IOLockLock(s->lock);assert(s->frames.find(token(i))->completion.ack());IOLockUnlock(s->lock);settleCurrent(s);
  }
  void boundary(bool midLookup){
    native.reset();{std::lock_guard<std::mutex> l(lookup.mutex);lookup.entered=lookup.proceed=false;lookup.held=lookup.invalidated=lookup.released=lookup.closed=0;}
    for(unsigned i=1;i<=4;++i)prime(i);
    for(unsigned i=5;i<=8;++i)admit(i);
    auto *s=session();assert(s->frames.accepted==8&&s->frames.pending==4&&images==4&&pending==4);
    for(unsigned i=5;i<=7;++i)s->frames.find(token(i))->phase=pianovideo::stream::Phase::Submitted;
    // Token 8 is accepted/published but still queued in software, never ETBed.
    auto *f=s->frames.find(token(8));auto &in=s->inputs[unsigned(f-s->frames.frames)];
    in.bytes=static_cast<uint8_t *>(IOMalloc(contract::kMaxAuBytes));assert(in.bytes);in.size=64;
    if(midLookup){
      s->surfaces=new TestSurface;s->surfaceOpens=1;s->current=s->currentAccepted=true;s->currentToken=token(5);
      s->frames.find(token(5))->phase=pianovideo::stream::Phase::Delivering;
      s->frame.memory=new IOMemoryDescriptor;s->frame.token=token(5);native.loans=1;
      assert(s->guest.begin(s->nextRequest++,cc("Dbuf"),nowMs()));
      assert(s->guest.reply(s->guest.request,cc("Dbuf")));s->replyError=0;s->replyHasSurface=true;s->replySurfaceID=69;
    }
  }
  void launch(){auto *s=session();s->workerRunning=true;workerThread=std::thread([s]{worker(s,0);});}
  void invalidate(bool midLookup){
    if(midLookup){launch();std::unique_lock<std::mutex> l(lookup.mutex);assert(lookup.cv.wait_for(l,std::chrono::seconds(3),[]{return lookup.entered;}));}
    invalidating=true;holdAck=true;
    if(!midLookup)session()->workerRunning=true; // queue dinv before worker runs
    requestType(cc("dinv"),115);assert(!dinvReplies);
    const uint8_t lateUuid[16]={1};emitFrame(session(),0,lateUuid); // fenced even during Dbuf
    // The selector is available even while invalidate is waiting on the guest.
    assert(selector.try_lock());selector.unlock();
    if(midLookup){std::lock_guard<std::mutex> l(lookup.mutex);lookup.proceed=true;lookup.cv.notify_all();}
    else workerThread=std::thread([this]{worker(session(),0);});
    while(terminalHeaders.empty())pump();
    drain();assert(!dinvReplies);holdAck=false;
    assert(!acknowledge(terminalHeaders.front()));
    while(!dinvReplies)pump();workerThread.join();drain();
    assert(dinvStatus==0&&!pending&&errors==4&&native.submits==0&&native.closes==1);
    assert(!session()->workerRunning&&session()->frames.pending==0&&session()->frames.cancelled==4);
    for(const auto &[t,n]:completions){(void)t;assert(n==1);}
    assert(acknowledge(terminalHeaders.front())==-1); // duplicate cannot re-retire
    requestType(cc("dfin"),116);drain();assert(dfinStatus==kBadState);
    requestType(cc("dinv"),117);drain();assert(dinvReplies==2&&errors==4); // repeat is idempotent
    if(midLookup){std::lock_guard<std::mutex> l(lookup.mutex);assert(lookup.invalidated==1&&lookup.released==1&&lookup.closed==1&&native.releases==1);}
  }
  void reopen(){
    const Header old=terminalHeaders.front();requestType(cc("-dec"),118);assert(!client->session);
    ++epoch;target=true;invalidating=false;native.reset();create();drain();
    assert(acknowledge(old)==-1);const unsigned before=images;prime(1);assert(images==before+1&&!pending);
    invalidating=true;session()->workerRunning=true;requestType(cc("dinv"),119);
    workerThread=std::thread([this]{worker(session(),0);});const unsigned beforeReplies=dinvReplies;
    while(dinvReplies==beforeReplies)pump();workerThread.join();assert(dinvStatus==0&&!pending);
  }
};

static void failureCases(){
  // An image already delivered before the fence may still owe its ACK. Join
  // that ACK without replacing its successful Demt with a second completion.
  {Harness h;native.reset();h.admit(1);auto *s=h.session();s->current=s->currentAccepted=true;s->currentToken=h.token(1);
    h.holdAck=true;const uint8_t uuid[16]={1};emitFrame(s,0,uuid);h.drain();assert(h.images==1&&!h.pending);
    const Header delivered={cc("Demt"),FlagHostRequest,{},s->guest.request};
    Header ack=delivered;std::memcpy(ack.uuid,s->uuid,16);
    h.invalidating=true;s->workerRunning=true;h.requestType(cc("dinv"),115);
    h.workerThread=std::thread([&]{worker(s,0);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!native.closes){assert(std::chrono::steady_clock::now()<deadline);IOSleep(1);}
    h.drain();assert(!h.dinvReplies);Header wrong=ack;++wrong.request;assert(h.acknowledge(wrong)==-1);
    wrong=ack;wrong.uuid[0]^=1;assert(h.acknowledge(wrong)==-1);
    assert(!h.acknowledge(ack));while(!h.dinvReplies)h.pump();h.workerThread.join();
    assert(h.dinvStatus==0&&h.images==1&&!h.errors&&!h.pending);
  }
  // A failed enqueue or a nonzero guest ACK cannot produce a successful dinv,
  // retry an already delivered message, or keep native/surface loans alive.
  for(int ackError:{kBadState,INT32_MAX,0}){
    const bool sendFailure=ackError==0;
    Harness h;native.reset();h.admit(1);h.drain();h.invalidating=true;h.failSend=sendFailure;
    h.session()->workerRunning=true;h.requestType(cc("dinv"),115);
    h.workerThread=std::thread([&]{worker(h.session(),0);});
    while(!h.dinvReplies)h.pump(true,ackError);h.workerThread.join();
    assert(h.dinvStatus==kDecodeError&&h.errors==(sendFailure?0u:1u)&&native.closes==1);
  }
  // A lost ACK is bounded by the aggregate invalidate deadline (900 ms, inside
  // Apple's 1.1 s timeout), even though the ordinary guest deadline is 2 s.
  {Harness h;native.reset();h.admit(1);h.drain();h.invalidating=true;h.dropAck=true;
    h.session()->workerRunning=true;h.requestType(cc("dinv"),115);
    h.workerThread=std::thread([&]{worker(h.session(),0);});
    while(!h.dinvReplies)h.pump();h.workerThread.join();assert(h.dinvStatus==kDecodeError&&h.errors==1);}
  // An ACK at/after the PV deadline cannot prove retirement: Apple's cleared
  // target also ACKs 0. Even a matching successful ACK must fail this join.
  {Harness h;native.reset();h.admit(1);h.drain();h.invalidating=true;h.holdAck=true;
    auto *s=h.session();s->workerRunning=true;h.requestType(cc("dinv"),115);
    h.workerThread=std::thread([&]{worker(s,0);});while(h.terminalHeaders.empty())h.pump();
    IOLockLock(s->lock);s->invalidateDeadline=nowMs();IOLockUnlock(s->lock);
    // The ACK may race the worker clearing its guest record; either transport
    // result is safe, but dinv must never claim success.
    h.acknowledge(h.terminalHeaders.front());while(!h.dinvReplies)h.pump();h.workerThread.join();
    assert(h.dinvStatus==kDecodeError&&h.errors==1);
  }
  // Client destruction abandons a lost peer instead of spending its ACK budget.
  {Harness h;native.reset();h.admit(1);h.drain();h.invalidating=true;h.holdAck=true;
    h.session()->workerRunning=true;h.requestType(cc("dinv"),115);
    h.workerThread=std::thread([&]{worker(h.session(),0);});
    while(h.terminalHeaders.empty())h.pump();teardown(h.session());h.workerThread.join();h.drain();assert(!h.dinvReplies);}
  // Held async admissions and synchronous originals have their original reply
  // owner. They get no second error Demt on invalidate.
  for(uint32_t flags:{0u,9u}){
    Harness h;native.reset();h.admit(1,flags,true);h.drain();h.invalidating=true;
    h.session()->workerRunning=true;h.requestType(cc("dinv"),115);
    h.workerThread=std::thread([&]{worker(h.session(),0);});
    while(!h.dinvReplies)h.pump();h.workerThread.join();assert(h.dinvStatus==0&&h.originalErrors==1&&!h.errors&&!h.pending);
  }
}
int main(){
  for(bool midLookup:{false,true}){Harness h;h.boundary(midLookup);h.invalidate(midLookup);h.reopen();}
  failureCases();
  std::puts("VT cancel host PASS real-worker 8-accepted 4-images 4-terminal Dbuf queued-before-submit ACK-before-dinv no-double no-late-success bounded-failure epoch21");
}
