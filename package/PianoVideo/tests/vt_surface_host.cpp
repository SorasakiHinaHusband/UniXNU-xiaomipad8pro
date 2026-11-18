// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "PianoVideoVTSurface.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace pianovideo::vtsurface;

// The independently decoded 27.3 contract: lookup installs two references;
// invalidate removes the handle, release removes the buffer, stop revokes the
// creator task. This models that contract, not hardware or IOKit qualification.
struct Peer {
  bool creator=true, handle=false, buffer=false;
  uint32_t liveID=0, fail=0;
  std::vector<uint32_t> calls;
  bool lookup(uint32_t id) {
    if (!creator || handle || buffer) return false;
    liveID=id; handle=buffer=true; return true;
  }
  int call(uint32_t selector,uint32_t id) {
    calls.push_back(selector);
    assert(id==liveID);
    if (selector==fail) return -1;
    if (selector==Invalidate) { assert(handle); handle=false; }
    else { assert(selector==Release && buffer); buffer=false; }
    return 0;
  }
  void close() { creator=handle=buffer=false; }
};
int main() {
  Peer old;
  assert(old.lookup(30)); old.close(); assert(!old.lookup(30));
  Peer p; Lease lease;
  assert(!lease.acquire(0));
  for (unsigned frame=0;frame<120;++frame) {
    // Repeated IDs and fresh IDs; the client never stops between pictures.
    const uint32_t id=frame<60?30:frame;
    assert(p.lookup(id) && lease.acquire(id));
    assert(!lease.acquire(id+1)); // at most one output reference pair
    // Identical disposal on success, invalid layout, copy failure or cancel.
    assert(lease.dispose(p) && !lease.held() && !p.handle && !p.buffer);
    assert(p.creator);
    const auto calls=p.calls.size(); assert(lease.dispose(p));
    assert(p.calls.size()==calls); // no duplicate decrement
  }
  assert(p.calls.size()==240);
  for (unsigned i=0;i<240;i+=2)
    assert(p.calls[i]==Invalidate && p.calls[i+1]==Release);
  for (const uint32_t failure : {Invalidate,Release}) {
    Peer bad; Lease pending; bad.fail=failure;
    assert(bad.lookup(30) && pending.acquire(30));
    assert(!pending.dispose(bad));
    assert(bad.calls.size()==2); // second obligation survives first failure
    assert(pending.held() && !pending.acquire(31));
    assert(pending.handle==(failure==Invalidate));
    assert(pending.buffer==(failure==Release));
    bad.close(); pending.closed(); assert(!pending.held());
  }
  p.close(); lease.closed();
  puts("PianoVideo VT surface tests: repeated-ID/ref-pair/error/cancel bounds pass");
}
