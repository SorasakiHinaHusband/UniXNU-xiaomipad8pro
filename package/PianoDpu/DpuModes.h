// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu: the timing words of each panel refresh mode (144 / 120 / 90 / 60 Hz), per DSI link, as the INTF timing engine and the DSI
// 6G controller hold them. Portable and store-free (host tests in tests/dpu_modes_host.cpp). B1 milestone 1 logs these words next to
// the live ones (DpuSnapshot.h); milestone 3 writes them (docs/drivers/dpu.md §4.3).
//
// Facts:
//   - the modes are the Ubuntu piano panel driver's (DpuHw.h kModes, full-panel numbers): the link runs at a fixed 1197.5 Mbit/s per
//     lane, the horizontal front porch selects 144/120/90 Hz and 60 Hz is the 120 Hz timing with the vertical front porch lengthened
//     by one whole frame (the stock DT's dfps_immediate_porch_mode_vfp). In every mode the INTF clock stays 159 667 200 Hz, the stock
//     Android capture's pixel clock (Project-SunUEFI oem-refresh.md: 159666666 Hz); so a mode change is a porch change, no clock, no
//     DSC or PPS change. The stock Android display lists all seven of its modes (144/120/90/60/50/48/30) in one seamless group;
//   - the INTF formulas are msm dpu_hw_intf_setup_timing_engine's (DpuHw.h): HSYNC_CTL = htotal << 16 | hsync width;
//     VSYNC_PERIOD_F0 = vtotal * htotal; VSYNC_PULSE_WIDTH_F0 = vsync lines * htotal; DISPLAY_V_START_F0 = (vsync + vbp) * htotal + skew;
//     DISPLAY_V_END_F0 = (vtotal - vfp) * htotal + skew - 1; DISPLAY_HCTL = (htotal - hfp - 1) << 16 | (hsync + hbp);
//   - the DSI ones are msm dsi_timing_setup's: ACTIVE_H = end << 16 | start, TOTAL = (vtotal - 1) << 16 | (htotal - 1), per link (each
//     controller of the bonded pair carries half the horizontal timing, the active part DSC-compressed, the porches not);
//   - the per-link numbers are taken from the live registers, not assumed: the compressed active width and the sync/back porch from
//     DISPLAY_HCTL and ACTIVE_H, the skew from DISPLAY_V_START. Only the front porches come from the mode table (halved per link).
//
// So DISPLAY_HCTL, DSI ACTIVE_H/ACTIVE_V and DATA_HCTL are the same in every mode; HSYNC_CTL, VSYNC_PERIOD_F0, VSYNC_PULSE_WIDTH_F0,
// DISPLAY_V_START_F0, DISPLAY_V_END_F0 and DSI TOTAL change with the horizontal front porch; with the vertical one (60 Hz) only
// VSYNC_PERIOD_F0 and DSI TOTAL (DpuRefresh.h's plan writes exactly those).
#pragma once
#include <stdint.h>
#include "DpuHw.h"

namespace pianodpu {
namespace modes {

constexpr unsigned kModeCount = sizeof kModes / sizeof kModes[0];
constexpr uint64_t kPixelClockHz = 159667200;   // 576 x 2310 x 120, the same for every mode

// The live words of one link a plan is derived from and compared with.
struct LinkLive {
  uint32_t hsyncCtl = 0, period = 0, vsyncWidth = 0, dispVStart = 0, dispVEnd = 0, dispHctl = 0;   // INTF1 / INTF2
  uint32_t dsiTotal = 0, dsiActiveH = 0, dsiActiveV = 0;                                            // DSI0 / DSI1
};

// The per-link geometry the live words give.
struct Geometry {
  bool ok = false;
  const char *why = "not derived";
  unsigned hsync = 0, hsyncBp = 0, hactive = 0, vsync = 0, vbp = 0, vactive = 0, skew = 0;
  unsigned dsiHstart = 0, dsiHend = 0;
};

// One mode's words for one link.
struct LinkWords {
  unsigned hz = 0, htotal = 0, vtotal = 0, hfp = 0, vfp = 0;
  uint32_t hsyncCtl = 0, period = 0, vsyncWidth = 0, dispVStart = 0, dispVEnd = 0, dispHctl = 0;
  uint32_t dsiTotal = 0, dsiActiveH = 0, dsiActiveV = 0;
  uint64_t pixelClockHz = 0;   // htotal x vtotal x hz
};

// Geometry from the live words: hsync width from HSYNC_CTL, the sync + back porch and the active width from DISPLAY_HCTL, the
// vertical sync + back porch from DISPLAY_V_START (minus the skew HSYNC_SKEW would add: 0 on piano), the vertical sync width from
// VSYNC_PULSE_WIDTH. The panel's vertical facts (DpuHw.h) must agree with them.
inline Geometry geometry(const LinkLive &l, uint32_t skew = 0)
{
  Geometry g;
  const unsigned htotal = l.hsyncCtl >> 16;
  if (!htotal) {
    g.why = "HSYNC_CTL holds no line length";
    return g;
  }
  g.hsync = l.hsyncCtl & 0xffffu;
  g.hsyncBp = l.dispHctl & 0xffffu;
  const unsigned hend = l.dispHctl >> 16;
  if (hend < g.hsyncBp || hend >= htotal || g.hsyncBp < g.hsync) {
    g.why = "DISPLAY_HCTL is not inside the line";
    return g;
  }
  g.hactive = hend - g.hsyncBp + 1;
  if (l.vsyncWidth % htotal || l.dispVStart < skew || (l.dispVStart - skew) % htotal) {
    g.why = "the vertical words are not whole lines of HSYNC_CTL's length";
    return g;
  }
  g.skew = skew;
  g.vsync = l.vsyncWidth / htotal;
  const unsigned syncBp = (l.dispVStart - skew) / htotal;
  if (g.vsync != kVsync || syncBp != kVsync + kVbp) {
    g.why = "the vertical sync or back porch is not the panel's (2 + 104 lines)";
    return g;
  }
  g.vbp = syncBp - g.vsync;
  g.vactive = kPanelHeight;
  g.dsiHstart = l.dsiActiveH & 0xffffu;
  g.dsiHend = l.dsiActiveH >> 16;
  if (g.dsiHend <= g.dsiHstart || g.dsiHend - g.dsiHstart != g.hactive || g.dsiHstart != g.hsyncBp) {
    g.why = "DSI ACTIVE_H does not carry the INTF's active width at the INTF's start";
    return g;
  }
  g.ok = true;
  g.why = "ok";
  return g;
}

// A mode's words on a link of geometry g. hfp/vfp: the mode table's full-panel porches; the horizontal one halved per link.
inline LinkWords plan(const Geometry &g, const Mode &m)
{
  LinkWords w;
  w.hz = m.hz;
  w.hfp = m.hfp / 2;
  w.vfp = m.vfp;
  w.htotal = g.hsyncBp + g.hactive + w.hfp;
  w.vtotal = g.vsync + g.vbp + g.vactive + w.vfp;
  w.hsyncCtl = uint32_t(w.htotal) << 16 | g.hsync;
  w.period = w.htotal * w.vtotal;
  w.vsyncWidth = g.vsync * w.htotal;
  w.dispVStart = (g.vsync + g.vbp) * w.htotal + g.skew;
  w.dispVEnd = (w.vtotal - w.vfp) * w.htotal + g.skew - 1;
  w.dispHctl = uint32_t(w.htotal - w.hfp - 1) << 16 | g.hsyncBp;
  // The DSI line: its active part where the INTF's is, the same front porch.
  const unsigned dsiHtotal = g.dsiHend + w.hfp;
  w.dsiTotal = uint32_t(w.vtotal - 1) << 16 | uint32_t(dsiHtotal - 1);
  w.dsiActiveH = uint32_t(g.dsiHend) << 16 | g.dsiHstart;
  w.dsiActiveV = uint32_t(g.vsync + g.vbp + g.vactive) << 16 | uint32_t(g.vsync + g.vbp);
  w.pixelClockHz = uint64_t(w.htotal) * w.vtotal * m.hz;
  return w;
}

inline const Mode *modeByHz(unsigned hz)
{
  for (const Mode &m : kModes)
    if (m.hz == hz) return &m;
  return nullptr;
}

// Which word of a plan differs from the live link (nullptr: none).
inline const char *firstDifference(const LinkWords &w, const LinkLive &l)
{
  if (w.hsyncCtl != l.hsyncCtl) return "HSYNC_CTL";
  if (w.period != l.period) return "VSYNC_PERIOD_F0";
  if (w.vsyncWidth != l.vsyncWidth) return "VSYNC_PULSE_WIDTH_F0";
  if (w.dispVStart != l.dispVStart) return "DISPLAY_V_START_F0";
  if (w.dispVEnd != l.dispVEnd) return "DISPLAY_V_END_F0";
  if (w.dispHctl != l.dispHctl) return "DISPLAY_HCTL";
  if (w.dsiTotal != l.dsiTotal) return "DSI TOTAL";
  if (w.dsiActiveH != l.dsiActiveH) return "DSI ACTIVE_H";
  if (w.dsiActiveV != l.dsiActiveV) return "DSI ACTIVE_V";
  return nullptr;
}

// The live mode of both links: the rate whose plan both links match word for word, 0 when none does (why says what differs).
struct Classified {
  unsigned hz = 0;
  const char *why = "not classified";
  unsigned link = 0;   // the link of the first difference against the 120 Hz plan, when hz == 0
};
inline Classified classify(const LinkLive live[2], uint32_t skew = 0)
{
  Classified c;
  Geometry g[2];
  for (unsigned l = 0; l < 2; ++l) {
    g[l] = geometry(live[l], skew);
    if (!g[l].ok) {
      c.why = g[l].why;
      c.link = l;
      return c;
    }
  }
  for (const Mode &m : kModes) {
    if (!firstDifference(plan(g[0], m), live[0]) && !firstDifference(plan(g[1], m), live[1])) {
      c.hz = m.hz;
      c.why = "every timing word matches the plan on both links";
      return c;
    }
  }
  const Mode &ref = *modeByHz(120);
  for (unsigned l = 0; l < 2; ++l)
    if (const char *d = firstDifference(plan(g[l], ref), live[l])) {
      c.why = d;
      c.link = l;
      return c;
    }
  c.why = "no mode matches";
  return c;
}

// The words a switch from one mode to another changes on a link (a bit set over the order of LinkWords' nine words, bit 0 HSYNC_CTL).
inline unsigned changedWords(const LinkWords &a, const LinkWords &b)
{
  const uint32_t x[9] = {a.hsyncCtl, a.period, a.vsyncWidth, a.dispVStart, a.dispVEnd, a.dispHctl, a.dsiTotal, a.dsiActiveH, a.dsiActiveV};
  const uint32_t y[9] = {b.hsyncCtl, b.period, b.vsyncWidth, b.dispVStart, b.dispVEnd, b.dispHctl, b.dsiTotal, b.dsiActiveH, b.dsiActiveV};
  unsigned bits = 0;
  for (unsigned k = 0; k < 9; ++k)
    if (x[k] != y[k]) bits |= 1u << k;
  return bits;
}
constexpr unsigned kWordHsyncCtl = 1u << 0, kWordPeriod = 1u << 1, kWordVsyncWidth = 1u << 2, kWordVStart = 1u << 3, kWordVEnd = 1u << 4,
                   kWordHctl = 1u << 5, kWordDsiTotal = 1u << 6, kWordDsiActiveH = 1u << 7, kWordDsiActiveV = 1u << 8;

// ------------------------------------------------------------------------------------------------- the panel's rate group
// Only a switch between the 120 Hz group (120, 60) and 144 or 90 changes the panel's rate-group registers. The Ubuntu driver writes
// them in its init after selecting command page 0x10 (FF 10, FB 01 just before; panel-novatek-nt36532.c piano_init_sequence); the
// stock driver sends the same values as its timing-switch command set in high speed while video runs (oem-refresh.md:
// DSI_CMD_SET_TIMING_VIDEO_SWITCH, timing-switch-video-command-state dsi_hs_mode). Module order (Ubuntu c82da418, stock): the BOE
// 35-02-0b (panel ID pin high) writes B2 then B3, the CSOT 42-02-0a B3 then B2. Each entry is a DCS short write with one parameter.
struct DcsWrite {
  uint8_t command, param;
};
struct RateGroup {
  DcsWrite w[4];
  unsigned count = 0;
};
inline bool sameGroup(const Mode &a, const Mode &b) { return a.b2 == b.b2 && a.b3 == b.b3; }
inline RateGroup rateGroup(const Mode &m, bool b2First)
{
  RateGroup r;
  r.w[r.count++] = {0xff, 0x10};
  r.w[r.count++] = {0xfb, 0x01};
  if (b2First) {
    r.w[r.count++] = {0xb2, m.b2};
    r.w[r.count++] = {0xb3, m.b3};
  } else {
    r.w[r.count++] = {0xb3, m.b3};
    r.w[r.count++] = {0xb2, m.b2};
  }
  return r;
}

}  // namespace modes
}  // namespace pianodpu
