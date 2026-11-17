// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. Single owner/borrower contract, no link dependency.
#pragma once
#include <stdint.h>
namespace pianousb {
constexpr const char *kDomainFunction="piano-usb-domain";
enum class DomainState:uint32_t { cold, preparing, ready, failed, held };
struct DomainOwner {
  DomainState state=DomainState::cold;
  bool begin() {if(state!=DomainState::cold)return false;state=DomainState::preparing;return true;}
  void finish(bool ok,bool pinnedFailure) {state=ok?DomainState::ready:pinnedFailure?DomainState::held:DomainState::failed;}
  bool borrow() const {return state==DomainState::ready;}
};
struct DomainInfo {
  uint32_t version=1,state=0,slot=~0u,bank=~0u,asid=0,fsr=0;
  bool configured=false,net=false;
  uint64_t table=0;
};
} // namespace pianousb
