// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#pragma once
#include <stdint.h>

namespace pianovideo { namespace vtsurface {
// The 27.3 lookup acquires a handle and a client-buffer reference. Its public
// framework drops these with trap4 then trap5; method56 then method1 are the
// equivalent external methods. Closing the user client also stops its task's
// creator authorization, so it is never a per-picture release operation.
constexpr uint32_t Invalidate = 56, Release = 1;
struct Lease {
  uint32_t id = 0;
  bool handle = false, buffer = false;
  bool held() const { return handle || buffer; }
  bool acquire(uint32_t value) {
    if (!value || held()) return false;
    id = value; handle = buffer = true; return true;
  }
  template<class Methods> bool dispose(Methods &methods) {
    // Attempt both even if invalidation fails. Keep failed obligations visible
    // until the worker's final client close; never overwrite them with a new ID.
    if (handle && methods.call(Invalidate, id) == 0) handle = false;
    if (buffer && methods.call(Release, id) == 0) buffer = false;
    if (!held()) id = 0;
    return !held();
  }
  void closed() { id = 0; handle = buffer = false; }
};
} }
