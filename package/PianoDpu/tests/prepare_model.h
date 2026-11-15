// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests only: a surface's memory descriptor as far as XNU's read-only prepare rule goes (its observed behaviour, modelled; the
// tablet's "IOMemoryDescriptor ... prepared read only" report of 2026-11-15 is this rule refusing):
//   - a prepare while nothing holds the memory wires it; when it is kIODirectionOut the memory is marked "prepared read only";
//   - a prepare while it is held is refused (kIODirectionNone/InOut: one that may write) as long as that mark is set;
//   - the last complete clears the mark.
// writable = false: memory that cannot be wired for writing at all (a prepare that may write fails even unheld); broken: no prepare
// succeeds.
// Used by tests/dpu_flip_host.cpp (PianoDpu's windows) and NativeGopDisplay's tests/gop_host.cpp (the copy's kept maps), through
// PianoDpuFlipClient.h's pianoPrepareSurface, the rule both drivers prepare a shared surface by.
#pragma once

struct PrepareModel {
  unsigned wired = 0;
  bool readOnly = false;
  bool writable = true;
  bool broken = false;     // every prepare fails
  unsigned refusals = 0;   // the "prepared read only" reports

  bool prepare(bool out)
  {
    if (broken) return false;
    if (wired) {
      if (readOnly && !out) {
        ++refusals;
        return false;
      }
    } else {
      if (!out && !writable) return false;
      readOnly = out;
    }
    ++wired;
    return true;
  }
  void complete()
  {
    if (wired && !--wired) readOnly = false;
  }
  // The rule both drivers use (PianoDpuFlipClient.h).
  PianoPrepared prepareShared()
  {
    return pianoPrepareSurface([this](bool out) { return prepare(out); });
  }
};
