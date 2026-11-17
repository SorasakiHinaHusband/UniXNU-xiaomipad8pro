// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Single-writer ownership model shared by the firmware pump and host tests.
#pragma once
#include <stdint.h>
namespace pianovideo { namespace stream {
constexpr unsigned Credits = 8, Inputs = 2, Raws = 64;
enum class Phase : uint8_t { Free, Queued, Submitted, Picture, Delivering };
struct Record {
  uint64_t token = 0, serial = 0;
  uint32_t flags = 0;
  Phase phase = Phase::Free;
};
struct Ledger {
  Record frames[Credits] = {};
  bool inputOwned[Inputs] = {}, rawHeld[Raws] = {};
  uint64_t inputToken[Inputs] = {}, rawToken[Raws] = {};
  unsigned inputUses[Inputs] = {}, rawUses[Raws] = {};
  unsigned pending = 0, highWater = 0, inputReuse = 0, rawReuse = 0, retired = 0, cancelled = 0;
  uint64_t accepted = 0, barrier = 0;
  bool draining = false, ack = false, last = false, paused = false, stopped = false;
  Record *find(uint64_t t) { for(auto &f:frames) if(f.phase!=Phase::Free && f.token==t)return &f; return nullptr; }
  Record *accept(uint64_t t, uint32_t flags = 1) {
    if(stopped || draining || pending==Credits || find(t))return nullptr;
    for(auto &f:frames)if(f.phase==Phase::Free){f={t,++accepted,flags,Phase::Queued};++pending;if(pending>highWater)highWater=pending;return &f;}
    return nullptr;
  }
  int freeInput() const {for(unsigned i=0;i<Inputs;++i)if(!inputOwned[i])return int(i);return -1;}
  bool submit(uint64_t t,unsigned i) {
    Record *f=find(t);if(stopped||!f||f->phase!=Phase::Queued||i>=Inputs||inputOwned[i])return false;
    inputOwned[i]=true;inputToken[i]=t;if(inputUses[i]++)++inputReuse;f->phase=Phase::Submitted;return true;
  }
  bool ebd(unsigned i) {if(i>=Inputs||!inputOwned[i])return false;inputOwned[i]=false;return true;}
  bool picture(uint64_t t,unsigned i) {
    Record *f=find(t);if(!f||f->phase!=Phase::Submitted||i>=Raws||rawHeld[i])return false;
    f->phase=Phase::Picture;rawHeld[i]=true;rawToken[i]=t;if(rawUses[i]++)++rawReuse;return true;
  }
  bool release(unsigned i,uint64_t t) {
    if(i>=Raws||!rawHeld[i]||rawToken[i]!=t)return false;
    rawHeld[i]=false;return true;
  }
  bool retire(uint64_t t) {
    Record *f=find(t);if(!f)return false;f->phase=Phase::Free;--pending;++retired;return true;
  }
  bool inputsEmpty() const {for(bool x:inputOwned)if(x)return false;return true;}
  bool loansEmpty() const {for(bool x:rawHeld)if(x)return false;return true;}
  bool beginDrain() {if(stopped)return false;if(draining||paused)return true;draining=true;barrier=accepted;ack=last=false;return true;}
  bool drainReady() const {return draining&&ack&&last&&inputsEmpty();}
  bool drained() {if(!drainReady()||pending||!loansEmpty())return false;draining=false;paused=true;return true;}
  bool resume() {if(stopped||draining||pending||!paused)return false;paused=ack=last=false;return true;}
  void cancel() {stopped=true;for(auto &f:frames)if(f.phase!=Phase::Free){f.phase=Phase::Free;++cancelled;}pending=0;}
};
// Exclusivity is independent of per-picture credits. Quiesce failure is sticky.
struct Owner {
  enum Phase { Idle, Reserved, Running, Closing, Poisoned } phase = Idle;
  uint32_t id=0, serial=0;
  bool owns(uint32_t s) const {return s&&s==id&&(phase==Reserved||phase==Running);}
  int acquire(uint32_t *s) {if(!s)return -22;if(phase!=Idle||serial==0xffff)return -16;*s=id=0x56360000u|++serial;phase=Reserved;return 0;}
  bool start(uint32_t s) {if(!owns(s)||phase!=Reserved)return false;phase=Running;return true;}
  int closing(uint32_t s,bool loans) {if(!owns(s))return -22;if(loans)return -16;phase=Closing;return 0;}
  void closed(bool held) {if(phase!=Closing)return;phase=held?Poisoned:Idle;id=0;}
};
} }
