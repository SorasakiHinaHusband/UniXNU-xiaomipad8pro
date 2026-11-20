/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Speaker protection on the audio DSP: see AudioSpkProt.h. */
#include "AudioSpkProt.h"
#include "AudioSession.h"

#define PARAM_HEADER 16u
#define PARAM_SG_CONFIG 0x08001001u
#define PARAM_CONTAINER_CONFIG 0x08001000u
#define PARAM_MODULE_LIST 0x08001002u
#define PARAM_MODULE_PROP 0x08001003u
#define PARAM_MODULE_CONN 0x08001004u
#define PARAM_CTRL_LINK 0x08001061u
#define PROP_SG_PERF 0x0800100Eu
#define PROP_SG_DIRECTION 0x0800100Fu
#define PROP_SG_SCENARIO 0x08001010u
#define PROP_CONT_CAPS 0x08001011u
#define PROP_CONT_POS 0x08001012u
#define PROP_CONT_STACK 0x08001013u
#define PROP_CONT_DOMAIN 0x08001014u
#define PROP_CONT_HEAP 0x08001174u
#define PROP_CONT_PARENT 0x080010CBu
#define PROP_CONT_PEER_HEAP 0x0800124Du
#define PROP_PORT_INFO 0x08001015u
#define PROP_LINK_INTENTS 0x08001062u
#define PROP_LINK_0x136F 0x0800136Fu    /* unnamed in the public API; 1 in the stock link */
#define LINK_INTENT_FSADSP 0x08001231u  /* unnamed in the public API; the stock link's one intent */
#define PARAM_HW_EP_MF 0x08001017u
#define PARAM_FRAME_SIZE_FACTOR 0x08001018u
#define PARAM_TDM_INTF 0x0800101Bu
#define PARAM_MFC_OUTPUT 0x08001024u
#define PARAM_TDM_LANE 0x0800106Au
#define PARAM_SRC_CHANNEL_MAP 0x0800108Au
#define CONTAINER_GC 0x0B001001u
#define PERF_LOW_LATENCY 2u
#define DIR_TX 1u
#define DIR_RX 2u
#define SCENARIO_ANY 0xffffffffu
#define POS_GLOBAL_DEV 4u
#define STACK 4096u
#define DOMAIN_ADSP 2u
#define CTRL_PORT 0x80000000u
#define CH_VI_FIRST 48u                 /* the V/I slots' channel types (the ACDB's source channel map: 48, 49, 50, 51) */

typedef struct {uint8_t *p; uint32_t n, cap; int bad;} W;
static void put8(W *w, uint32_t v) {if (w->n + 1 > w->cap) {w->bad = 1; return;} w->p[w->n++] = (uint8_t)v;}
static void put16(W *w, uint32_t v) {put8(w, v); put8(w, v >> 8);}
static void put32(W *w, uint32_t v) {put16(w, v); put16(w, v >> 16);}
static void pad_to(W *w, uint32_t end) {while (!w->bad && w->n < end) put8(w, 0);}
static uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}
static uint32_t align32(uint32_t v) {return (v + 31u) & ~31u;}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void set32(W *w, uint32_t at, uint32_t v)
{
    if (at + 4 <= w->n) {w->p[at] = (uint8_t)v; w->p[at + 1] = (uint8_t)(v >> 8); w->p[at + 2] = (uint8_t)(v >> 16); w->p[at + 3] = (uint8_t)(v >> 24);}
}
static void mem_zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}

/* A buffer (an out-of-band payload) or a GPR packet (capped at GPR_MAX_PACKET; size patched in at the end). */
static void buf_begin(W *w, uint8_t *buf, uint32_t cap) {w->p = buf; w->n = 0; w->cap = cap; w->bad = 0;}
static void pkt_begin(W *w, uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t opcode)
{
    buf_begin(w, out, cap < GPR_MAX_PACKET ? cap : GPR_MAX_PACKET);
    put32(w, 0); put32(w, GPR_DOMAIN_ADSP | GPR_DOMAIN_APPS << 8); put32(w, port); put32(w, AR_APM_IID); put32(w, token); put32(w, opcode);
}
static uint32_t pkt_finish(W *w)
{
    if (w->bad) return 0;
    set32(w, 0, (GPR_VERSION & 0xfu) | (GPR_HEADER_BYTES / 4) << 4 | w->n << 8);
    return w->n;
}
/* In-band APM command header; its payload size patched at the end. */
static uint32_t cmd_begin(W *w) {const uint32_t at = w->n; put32(w, 0); put32(w, 0); put32(w, 0); put32(w, 0); return at;}
static void cmd_end(W *w, uint32_t at) {set32(w, at + 12, w->n - at - 16u);}
/* One parameter block as the stock graph services lay them out: the header with the data's exact size, the data, zeros to 8. */
static uint32_t blk_begin(W *w, uint32_t iid, uint32_t pid) {const uint32_t at = w->n; put32(w, iid); put32(w, pid); put32(w, 0); put32(w, 0); return at;}
static void blk_end(W *w, uint32_t at)
{
    set32(w, at + 8, w->n - at - PARAM_HEADER);
    pad_to(w, at + align8(w->n - at));
}
static void prop(W *w, uint32_t id, uint32_t v) {put32(w, id); put32(w, 4); put32(w, v);}

/* ------------------------------------------------------------------------------------------------------- the private files */
typedef struct {uint32_t iid, pid, size; uint8_t want[16];} Expect;
/* The driver's own values (spk_endpoint_config): a file that carries these parameters must carry exactly them. */
static const Expect kExpect[] = {
    {SPK_IID_VI_TDM, PARAM_HW_EP_MF, 12, {0x80, 0xbb, 0, 0, 32, 0, 4, 0, 1, 0, 0, 0}},
    {SPK_IID_TDM, PARAM_HW_EP_MF, 12, {0x80, 0xbb, 0, 0, 32, 0, 4, 0, 1, 0, 0, 0}},
    {SPK_IID_VI_TDM, PARAM_TDM_LANE, 4, {SPK_LANE_VI, 0, 0, 0}},
    {SPK_IID_TDM, PARAM_TDM_LANE, 4, {SPK_LANE_RX, 0, 0, 0}},
    {SPK_IID_VI_TDM, PARAM_SRC_CHANNEL_MAP, 10, {4, 0, CH_VI_FIRST, 0, CH_VI_FIRST + 1, 0, CH_VI_FIRST + 2, 0, CH_VI_FIRST + 3, 0}},
    {SPK_IID_MFC, PARAM_MFC_OUTPUT, 16, {0x80, 0xbb, 0, 0, 32, 0, 4, 0, AR_CH_FL, 0, AR_CH_FR, 0, AR_CH_LS, 0, AR_CH_RS, 0}},
};

int spk_iid_graph(uint32_t iid)
{
    switch (iid) {
    case SPK_IID_VI_TDM: case SPK_IID_VI_LOG: case SPK_IID_VI_SPLIT: case SPK_IID_SP_TX: return 1;
    case SPK_IID_MFC: case SPK_IID_SP_RX: case SPK_IID_SPLIT: case SPK_IID_LOG: case SPK_IID_TDM: return 2;
    default: return 0;
    }
}

static int fail(const char **why, const char *text) {if (why) *why = text; return -1;}

int spk_params_parse(SpkProt *p, const uint8_t *file, uint32_t bytes, const char **why)
{
    p->have_params = 0; p->count = 0; p->bytes = 0;
    if (!file || bytes < 16) return fail(why, "spk-prot.bin: missing or shorter than its header");
    if (le32(file) != SPK_PARAMS_MAGIC) return fail(why, "spk-prot.bin: not a PSPK file");
    if (le32(file + 4) != SPK_PARAMS_VERSION) return fail(why, "spk-prot.bin: another version");
    const uint32_t count = le32(file + 8);
    if (!count || count > SPK_MAX_ENTRIES || le32(file + 12)) return fail(why, "spk-prot.bin: bad entry count");
    uint32_t at = 16;
    int rx = 0, tx = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < 12) return fail(why, "spk-prot.bin: truncated entry");
        SpkParam *e = &p->e[i];
        e->iid = le32(file + at); e->pid = le32(file + at + 4); e->size = le32(file + at + 8);
        at += 12;
        if (!e->size || e->size > SPK_MAX_PARAM || bytes - at < ((e->size + 3u) & ~3u)) return fail(why, "spk-prot.bin: bad parameter size");
        e->data = file + at;
        at += (e->size + 3u) & ~3u;
        if (!spk_iid_graph(e->iid)) return fail(why, "spk-prot.bin: a module that is not in the protection graphs");
        if (e->pid == PARAM_TDM_INTF) return fail(why, "spk-prot.bin: carries a TDM interface (the driver sends its own)");
        for (uint32_t k = 0; k < i; ++k)
            if (p->e[k].iid == e->iid && p->e[k].pid == e->pid) return fail(why, "spk-prot.bin: a parameter twice");
        for (unsigned k = 0; k < sizeof kExpect / sizeof kExpect[0]; ++k) {
            const Expect *x = &kExpect[k];
            if (x->iid != e->iid || x->pid != e->pid) continue;
            if (e->size != x->size) return fail(why, "spk-prot.bin: an endpoint/MFC parameter of another size than the driver's");
            for (uint32_t b = 0; b < x->size; ++b)
                if (e->data[b] != x->want[b]) return fail(why, "spk-prot.bin: an endpoint/MFC format or lane that is not the driver's");
        }
        rx |= e->iid == SPK_IID_SP_RX;
        tx |= e->iid == SPK_IID_SP_TX;
        p->bytes += e->size;
    }
    if (at != bytes) return fail(why, "spk-prot.bin: bytes after the last entry");
    if (!rx || !tx) return fail(why, "spk-prot.bin: no SP_RX or no SP_TX parameter (not the protection graphs' calibration)");
    p->count = count;
    p->have_params = 1;
    return 0;
}

int spk_re25_parse(SpkProt *p, const uint8_t *file, uint32_t bytes, const char **why)
{
    p->have_re25 = 0;
    if (!file || bytes != SPK_RE25_FILE_BYTES) return fail(why, "spk-re25.bin: missing or not 16 bytes");
    for (unsigned c = 0; c < 4; ++c) {
        const uint32_t r = le32(file + 4 * c);
        if (r < SPK_RE25_MIN || r > SPK_RE25_MAX) return fail(why, "spk-re25.bin: a channel's Re25 outside 2..16 ohm");
        p->re25[c] = r;
    }
    p->have_re25 = 1;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------ the graphs */
typedef struct {uint32_t id, peer_heap;} Cont;
typedef struct {uint32_t module, iid, container, inputs, outputs;} Mod;
typedef struct {uint32_t from, from_port, to, to_port;} Link;
typedef struct {uint32_t sg, direction; const Cont *c; unsigned containers; const Mod *m; unsigned modules; const Link *l; unsigned links;} Graph;

static const Cont kViCont[] = {{SPK_CONT_VI, 1}};
static const Mod kViMods[] = {
    {AR_MOD_LOG, SPK_IID_VI_LOG, SPK_CONT_VI, 1, 1}, {SPK_MOD_SPLITTER, SPK_IID_VI_SPLIT, SPK_CONT_VI, 1, 2},
    {SPK_MOD_TDM_SOURCE, SPK_IID_VI_TDM, SPK_CONT_VI, 0, 1}, {SPK_MOD_SP_TX, SPK_IID_SP_TX, SPK_CONT_VI, 1, 1},
};
static const Link kViLinks[] = {{SPK_IID_VI_TDM, 1, SPK_IID_VI_LOG, 2}, {SPK_IID_VI_LOG, 1, SPK_IID_VI_SPLIT, 2}, {SPK_IID_VI_SPLIT, 1, SPK_IID_SP_TX, 2}};
static const Cont kDevCont[] = {{SPK_CONT_MFC, 2}, {SPK_CONT_RX, 2}, {SPK_CONT_TAIL, 2}};
static const Mod kDevMods[] = {
    {AR_MOD_MFC, SPK_IID_MFC, SPK_CONT_MFC, 1, 1}, {SPK_MOD_SP_RX, SPK_IID_SP_RX, SPK_CONT_RX, 1, 1},
    {SPK_MOD_SPLITTER, SPK_IID_SPLIT, SPK_CONT_TAIL, 1, 24}, {AR_MOD_LOG, SPK_IID_LOG, SPK_CONT_TAIL, 1, 1},
    {AR_MOD_TDM_SINK, SPK_IID_TDM, SPK_CONT_TAIL, 1, 0},
};
static const Link kDevLinks[] = {{SPK_IID_MFC, 1, SPK_IID_SP_RX, 2}, {SPK_IID_SP_RX, 1, SPK_IID_SPLIT, 2}, {SPK_IID_SPLIT, 1, SPK_IID_LOG, 2},
                                 {SPK_IID_LOG, 1, SPK_IID_TDM, 2}};
#define N(a) (sizeof a / sizeof a[0])
static const Graph kVi = {SPK_SG_VI, DIR_TX, kViCont, N(kViCont), kViMods, N(kViMods), kViLinks, N(kViLinks)};
static const Graph kDev = {SPK_SG_DEV, DIR_RX, kDevCont, N(kDevCont), kDevMods, N(kDevMods), kDevLinks, N(kDevLinks)};

uint32_t spk_open_payload(uint8_t *buf, uint32_t cap, int vi)
{
    const Graph *g = vi ? &kVi : &kDev;
    W w;
    buf_begin(&w, buf, cap);
    uint32_t at = blk_begin(&w, AR_APM_IID, PARAM_SG_CONFIG);
    put32(&w, 1);
    put32(&w, g->sg); put32(&w, 3);
    prop(&w, PROP_SG_PERF, PERF_LOW_LATENCY); prop(&w, PROP_SG_DIRECTION, g->direction); prop(&w, PROP_SG_SCENARIO, SCENARIO_ANY);
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_CONTAINER_CONFIG);
    put32(&w, g->containers);
    for (unsigned i = 0; i < g->containers; ++i) {
        put32(&w, g->c[i].id); put32(&w, 7);
        put32(&w, PROP_CONT_CAPS); put32(&w, 8); put32(&w, 1); put32(&w, CONTAINER_GC);
        prop(&w, PROP_CONT_POS, POS_GLOBAL_DEV); prop(&w, PROP_CONT_STACK, STACK); prop(&w, PROP_CONT_DOMAIN, DOMAIN_ADSP);
        prop(&w, PROP_CONT_HEAP, 1); prop(&w, PROP_CONT_PARENT, 0xffffffffu); prop(&w, PROP_CONT_PEER_HEAP, g->c[i].peer_heap);
    }
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_MODULE_LIST);
    put32(&w, g->containers);                 /* every container holds a module */
    for (unsigned i = 0; i < g->containers; ++i) {
        unsigned n = 0;
        for (unsigned k = 0; k < g->modules; ++k) n += g->m[k].container == g->c[i].id;
        put32(&w, g->sg); put32(&w, g->c[i].id); put32(&w, n);
        for (unsigned k = 0; k < g->modules; ++k) if (g->m[k].container == g->c[i].id) {put32(&w, g->m[k].module); put32(&w, g->m[k].iid);}
    }
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_MODULE_PROP);
    put32(&w, g->modules);
    for (unsigned k = 0; k < g->modules; ++k) {
        put32(&w, g->m[k].iid); put32(&w, 1);
        put32(&w, PROP_PORT_INFO); put32(&w, 8); put32(&w, g->m[k].inputs); put32(&w, g->m[k].outputs);
    }
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_MODULE_CONN);
    put32(&w, g->links);
    for (unsigned k = 0; k < g->links; ++k) {put32(&w, g->l[k].from); put32(&w, g->l[k].from_port); put32(&w, g->l[k].to); put32(&w, g->l[k].to_port);}
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_CTRL_LINK);
    put32(&w, 1);
    put32(&w, SPK_IID_SP_TX); put32(&w, CTRL_PORT); put32(&w, SPK_IID_SP_RX); put32(&w, CTRL_PORT); put32(&w, 2);
    put32(&w, PROP_LINK_INTENTS); put32(&w, 8); put32(&w, 1); put32(&w, LINK_INTENT_FSADSP);
    prop(&w, PROP_LINK_0x136F, 1);
    blk_end(&w, at);
    return w.bad ? 0 : w.n;
}

uint32_t spk_oob(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t opcode, uint64_t address, uint32_t handle,
                 uint32_t bytes)
{
    if (!handle || !bytes || (bytes & 3u) || (address & 31u)) return 0;   /* the APM's rules for an out-of-band payload */
    W w;
    pkt_begin(&w, out, cap, port, token, opcode);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes);
    return pkt_finish(&w);
}

static void tdm_block(W *w, uint32_t iid, const ArTdm *t)
{
    const uint32_t at = blk_begin(w, iid, PARAM_TDM_INTF);
    put32(w, t->lpaif_type); put32(w, t->intf_idx); put16(w, t->sync_src); put16(w, t->data_out_enable);
    put32(w, t->slot_mask); put16(w, t->slots); put16(w, t->slot_width); put16(w, t->sync_mode); put16(w, t->invert_sync);
    put16(w, t->data_delay); put16(w, 0);
    blk_end(w, at);
}
static void word_block(W *w, uint32_t iid, uint32_t pid, uint32_t v) {const uint32_t at = blk_begin(w, iid, pid); put32(w, v); blk_end(w, at);}

uint32_t spk_endpoint_config(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, int vi, const ArTdm *t, const ArFormat *f)
{
    if (!t || !f || f->channels != 4 || t->slots != 4 || f->bits != 32 || t->slot_width != 32) return 0;
    const uint32_t iid = vi ? SPK_IID_VI_TDM : SPK_IID_TDM;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    tdm_block(&w, iid, t);
    uint32_t at = blk_begin(&w, iid, PARAM_HW_EP_MF);
    put32(&w, f->rate); put16(&w, f->bits); put16(&w, f->channels); put32(&w, 1);
    blk_end(&w, at);
    word_block(&w, iid, PARAM_FRAME_SIZE_FACTOR, 1);
    word_block(&w, iid, PARAM_TDM_LANE, vi ? SPK_LANE_VI : SPK_LANE_RX);
    if (vi) {
        at = blk_begin(&w, iid, PARAM_SRC_CHANNEL_MAP);
        put16(&w, 4);
        for (unsigned c = 0; c < 4; ++c) put16(&w, CH_VI_FIRST + c);
        blk_end(&w, at);
    } else {
        uint8_t map[4] = {0};
        (void)ar_channel_map(4, map);
        at = blk_begin(&w, SPK_IID_MFC, PARAM_MFC_OUTPUT);
        put32(&w, f->rate); put16(&w, f->bits); put16(&w, f->channels);
        for (unsigned c = 0; c < 4; ++c) put16(&w, map[c]);
        blk_end(&w, at);
    }
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

static void data_block(W *w, uint32_t iid, uint32_t pid, const uint8_t *data, uint32_t size)
{
    const uint32_t at = blk_begin(w, iid, pid);
    for (uint32_t i = 0; i < size && !w->bad; ++i) put8(w, data[i]);
    blk_end(w, at);
}

uint32_t spk_param_block(uint8_t *buf, uint32_t cap, uint32_t iid, uint32_t pid, const uint8_t *data, uint32_t size)
{
    W w;
    buf_begin(&w, buf, cap);
    data_block(&w, iid, pid, data, size);
    return w.bad ? 0 : w.n;
}

uint32_t spk_set_params(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const SpkProt *p, const uint8_t *order,
                        unsigned first, unsigned count)
{
    if (!p || !count || first + count > p->count) return 0;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    for (unsigned i = first; i < first + count; ++i) {
        const SpkParam *e = &p->e[order[i]];
        data_block(&w, e->iid, e->pid, e->data, e->size);
    }
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

uint32_t spk_re25(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const uint32_t re25[4])
{
    for (unsigned c = 0; c < 4; ++c) if (re25[c] < SPK_RE25_MIN || re25[c] > SPK_RE25_MAX) return 0;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    const uint32_t at = blk_begin(&w, SPK_IID_SP_RX, SPK_PID_RE25);
    put16(&w, 0); put16(&w, 4);
    for (unsigned c = 0; c < 4; ++c) {put32(&w, 0); put32(&w, re25[c]);}
    blk_end(&w, at);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

uint32_t spk_vbat(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t microvolts)
{
    if (microvolts < 2500000u || microvolts > 5000000u) return 0;   /* one cell, as the gauges read it */
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    const uint32_t at = blk_begin(&w, SPK_IID_SP_RX, SPK_PID_VBAT);
    put32(&w, 1); put32(&w, microvolts); put32(&w, 0); put32(&w, 0);
    blk_end(&w, at);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

void spk_live_stage(uint8_t slot[SPK_LIVE_SLOT])
{
    W w;
    buf_begin(&w, slot, SPK_LIVE_SLOT);
    put32(&w, SPK_IID_SP_RX); put32(&w, SPK_PID_LIVE); put32(&w, SPK_LIVE_BYTES); put32(&w, 0);
    pad_to(&w, SPK_LIVE_SLOT);
}

int spk_live_parse(const uint8_t slot[SPK_LIVE_SLOT], SpkLive *out)
{
    if (le32(slot) != SPK_IID_SP_RX || le32(slot + 4) != SPK_PID_LIVE || le32(slot + 8) != SPK_LIVE_BYTES || le32(slot + 12)) return -1;
    const uint8_t *d = slot + PARAM_HEADER;
    for (unsigned c = 0; c < 4; ++c, d += 24) {
        out->re[c] = le32(d); out->temp[c] = le32(d + 4); out->w2[c] = le32(d + 8);
        out->f0[c] = le32(d + 12); out->q[c] = le32(d + 16); out->w5[c] = le32(d + 20);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------- the plan */
static const SpkPlan *plan(const ArSeq *s) {return (const SpkPlan *)s->context;}

uint64_t spk_plan_address(const SpkPlan *p, uint32_t offset)
{
    uint64_t base;
    if (ar_dsp_address(p->base.buffer_phys, p->base.stream_id, &base)) return 0;
    return base + p->param_offset + offset;
}

static uint32_t clock_on(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_clock(o, c, s->prm_port, t, AR_CLK_SEC_TDM_IBIT, AR_TDM_BIT_CLOCK);}
static uint32_t clock_off(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_clock(o, c, s->prm_port, t, AR_CLK_SEC_TDM_IBIT, 0);}
static uint32_t map_region(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    uint64_t address;
    if (p->base.buffer_phys % 4096u || ar_dsp_address(p->base.buffer_phys, p->base.stream_id, &address)) return 0;
    return ar_map(o, c, s->port, t, address, p->region_bytes);
}
static uint32_t unmap_region(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return s->handle ? ar_unmap(o, c, s->port, t, s->handle) : 0;}
static uint32_t open_vi(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    return spk_oob(o, c, s->port, t, AR_OP_GRAPH_OPEN, spk_plan_address(p, p->vi_open), s->handle, p->vi_open_bytes);
}
static uint32_t open_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    return spk_oob(o, c, s->port, t, AR_OP_GRAPH_OPEN, spk_plan_address(p, p->dev_open), s->handle, p->dev_open_bytes);
}
#define GRAPH_CMD(name, op, sg) static uint32_t name(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, op, sg);}
GRAPH_CMD(close_vi, AR_OP_GRAPH_CLOSE, SPK_SG_VI)
GRAPH_CMD(prep_vi, AR_OP_GRAPH_PREPARE, SPK_SG_VI)
GRAPH_CMD(start_vi, AR_OP_GRAPH_START, SPK_SG_VI)
GRAPH_CMD(stop_vi, AR_OP_GRAPH_STOP, SPK_SG_VI)
GRAPH_CMD(close_dev, AR_OP_GRAPH_CLOSE, SPK_SG_DEV)
GRAPH_CMD(prep_dev, AR_OP_GRAPH_PREPARE, SPK_SG_DEV)
GRAPH_CMD(start_dev, AR_OP_GRAPH_START, SPK_SG_DEV)
GRAPH_CMD(stop_dev, AR_OP_GRAPH_STOP, SPK_SG_DEV)
GRAPH_CMD(close_str, AR_OP_GRAPH_CLOSE, AR_SG_PLAY)
GRAPH_CMD(prep_str, AR_OP_GRAPH_PREPARE, AR_SG_PLAY)
GRAPH_CMD(start_str, AR_OP_GRAPH_START, AR_SG_PLAY)
GRAPH_CMD(stop_str, AR_OP_GRAPH_STOP, AR_SG_PLAY)
static uint32_t vi_endpoint(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_endpoint_config(o, c, s->port, t, 1, &plan(s)->base.tdm, &plan(s)->base.slots);}
static uint32_t dev_endpoint(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_endpoint_config(o, c, s->port, t, 0, &plan(s)->base.tdm, &plan(s)->base.slots);}
/* A parameter chunk: which one is the step being built (ar_seq_next builds step s->done). */
static uint32_t chunk_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    if (s->done >= p->count || p->step_chunk[s->done] < 0) return 0;
    const SpkChunk *k = &p->chunk[p->step_chunk[s->done]];
    if (k->oob) return spk_oob(o, c, s->port, t, AR_OP_SET_CFG, spk_plan_address(p, k->offset), s->handle, k->bytes);
    return spk_set_params(o, c, s->port, t, p->prot, p->order, k->first, k->count);
}
static uint32_t re25_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_re25(o, c, s->port, t, plan(s)->prot->re25);}
static uint32_t open_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_open_playback_stream_to(o, c, s->port, t, SPK_IID_MFC);}
static uint32_t str_shmem(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_shmem(o, c, s->port, t, &plan(s)->base.stream);}
static uint32_t str_pcm(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_stream(o, c, s->port, t, &plan(s)->base.stream);}
static uint32_t str_log(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_logger(o, c, s->port, t);}
static int amps_on(const ArSeq *s) {const SpkPlan *p = plan(s); return p->base.amps_on ? p->base.amps_on(p->base.owner) : -1;}
static int amps_off(const ArSeq *s) {const SpkPlan *p = plan(s); return p->base.amps_off ? p->base.amps_off(p->base.owner) : -1;}

static int add(SpkPlan *p, const char *name, ArBuild build, uint32_t opcode, int answer, ArLocal local, ArBuild undo, uint32_t undo_opcode,
               int undo_answer, ArLocal undo_local, int prm, int chunk)
{
    if (p->count >= SPK_MAX_STEPS) return -1;
    ArStep *st = &p->steps[p->count];
    st->name = name; st->build = build; st->opcode = opcode; st->answer = answer; st->local = local;
    st->undo = undo; st->undo_opcode = undo_opcode; st->undo_answer = undo_answer; st->undo_local = undo_local; st->prm = prm;
    p->step_chunk[p->count++] = (int8_t)chunk;
    return 0;
}
#define CMD(p, name, build, op) add(p, name, build, op, ArBasicResult, 0, 0, 0, 0, 0, 0, -1)
#define CMD_UNDO(p, name, build, op, undo, uop) add(p, name, build, op, ArBasicResult, 0, undo, uop, ArBasicResult, 0, 0, -1)

static const char *const kChunkNames[2][2] = {{"speaker parameters (in band)", "speaker parameter (out of band)"},
                                              {"V/I parameters (in band)", "V/I parameter (out of band)"}};

int spk_plan_init(SpkPlan *p, const SpkProt *prot, uint8_t *area, uint32_t area_bytes, const char **why)
{
    p->prot = prot;
    p->region_bytes = AUDIO_DMA_PARAM + AUDIO_DMA_PARAM_BYTES;
    p->param_offset = AUDIO_DMA_PARAM;
    p->chunks = p->vi_chunks = p->oob_chunks = p->count = 0;
    uint64_t address;
    if (!prot || !prot->have_params || !prot->have_re25) return fail(why, "the private files are not both valid");
    if (!area || area_bytes < AUDIO_DMA_PARAM_BYTES) return fail(why, "no parameter area");
    if (p->base.buffer_bytes > p->param_offset || p->base.buffer_phys % 4096u || ar_dsp_address(p->base.buffer_phys, p->base.stream_id, &address) ||
        (uint64_t)(uint32_t)address + p->region_bytes > 0x100000000ull)
        return fail(why, "the region is not DSP-addressable");
    /* the order: per graph (V/I first), the parameters that fit in band, then those that go out of band */
    unsigned n = 0;
    for (int g = 1; g <= 2; ++g)
        for (int big = 0; big < 2; ++big)
            for (unsigned i = 0; i < prot->count; ++i)
                if (spk_iid_graph(prot->e[i].iid) == g && (PARAM_HEADER + align8(prot->e[i].size) > SPK_INBAND_MAX) == big) p->order[n++] = (uint8_t)i;
    if (n != prot->count) return fail(why, "a parameter of no graph");
    /* the parameter area: both opens, the live slot, then every out-of-band parameter */
    uint32_t at = 0;
    p->vi_open = at;
    p->vi_open_bytes = spk_open_payload(area + at, area_bytes - at, 1);
    at = align32(at + p->vi_open_bytes);
    p->dev_open = at;
    p->dev_open_bytes = at < area_bytes ? spk_open_payload(area + at, area_bytes - at, 0) : 0;
    at = align32(at + p->dev_open_bytes);
    if (!p->vi_open_bytes || !p->dev_open_bytes || at + SPK_LIVE_SLOT > area_bytes) return fail(why, "the opens do not fit the parameter area");
    p->live_slot = at;
    spk_live_stage(area + at);
    at = align32(at + SPK_LIVE_SLOT);
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; ++i) {
        const SpkParam *e = &prot->e[p->order[i]];
        const int vi = spk_iid_graph(e->iid) == 1;
        const uint32_t block = PARAM_HEADER + align8(e->size);
        SpkChunk *last = p->chunks ? &p->chunk[p->chunks - 1] : 0;
        if (block <= SPK_INBAND_MAX && last && !last->oob && last->vi == vi && sum + block <= SPK_INBAND_MAX) {
            last->count++;
            sum += block;
            continue;
        }
        if (p->chunks >= SPK_MAX_CHUNKS) return fail(why, "too many parameter chunks");
        SpkChunk *k = &p->chunk[p->chunks++];
        mem_zero(k, sizeof *k);
        k->vi = (uint8_t)vi;
        k->first = (uint16_t)i; k->count = 1;
        if (block > SPK_INBAND_MAX) {
            k->oob = 1;
            k->offset = at;
            k->bytes = at < area_bytes ? spk_param_block(area + at, area_bytes - at, e->iid, e->pid, e->data, e->size) : 0;
            if (!k->bytes) return fail(why, "the out-of-band parameters do not fit the parameter area");
            at = align32(at + k->bytes);
            p->oob_chunks++;
        }
        sum = block;
        p->vi_chunks += (unsigned)vi;
    }
    p->staged = at;
    /* the steps */
    int r = 0;
    r |= add(p, "tdm bit clock 6.144 MHz", clock_on, AR_OP_PRM_REQUEST, ArPrmDone, 0, clock_off, AR_OP_PRM_RELEASE, ArPrmDone, 0, 1, -1);
    r |= add(p, "map the periods and the parameter area", map_region, AR_OP_MEM_MAP, ArMapped, 0, unmap_region, AR_OP_MEM_UNMAP,
             ArBasicResult, 0, 0, -1);
    r |= CMD_UNDO(p, "open V/I graph b00000d5 (out of band)", open_vi, AR_OP_GRAPH_OPEN, close_vi, AR_OP_GRAPH_CLOSE);
    r |= CMD(p, "V/I TDM source: interface, format, lane 0, V/I channels", vi_endpoint, AR_OP_SET_CFG);
    for (unsigned k = 0; k < p->chunks; ++k)
        if (p->chunk[k].vi) r |= add(p, kChunkNames[1][p->chunk[k].oob], chunk_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0, (int)k);
    r |= CMD(p, "prepare V/I", prep_vi, AR_OP_GRAPH_PREPARE);
    r |= CMD_UNDO(p, "start V/I (before the speaker)", start_vi, AR_OP_GRAPH_START, stop_vi, AR_OP_GRAPH_STOP);
    r |= CMD_UNDO(p, "open speaker graph b0000002 (out of band)", open_dev, AR_OP_GRAPH_OPEN, close_dev, AR_OP_GRAPH_CLOSE);
    r |= CMD(p, "speaker TDM sink: interface, format, lane 1; MFC 4 x 32-bit", dev_endpoint, AR_OP_SET_CFG);
    for (unsigned k = 0; k < p->chunks; ++k)
        if (!p->chunk[k].vi) r |= add(p, kChunkNames[0][p->chunk[k].oob], chunk_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0, (int)k);
    r |= CMD(p, "Re25 to SP_RX", re25_cmd, AR_OP_SET_CFG);
    r |= CMD_UNDO(p, "open playback stream graph (to the MFC)", open_str, AR_OP_GRAPH_OPEN, close_str, AR_OP_GRAPH_CLOSE);
    r |= CMD(p, "stream shared memory format", str_shmem, AR_OP_SET_CFG);
    r |= CMD(p, "stream decoder and converter", str_pcm, AR_OP_SET_CFG);
    r |= CMD(p, "stream logger", str_log, AR_OP_SET_CFG);
    r |= CMD(p, "prepare speaker", prep_dev, AR_OP_GRAPH_PREPARE);
    r |= CMD(p, "prepare stream", prep_str, AR_OP_GRAPH_PREPARE);
    r |= CMD_UNDO(p, "start speaker (bit clock runs)", start_dev, AR_OP_GRAPH_START, stop_dev, AR_OP_GRAPH_STOP);
    r |= CMD_UNDO(p, "start stream", start_str, AR_OP_GRAPH_START, stop_str, AR_OP_GRAPH_STOP);
    r |= add(p, "amplifiers on", 0, 0, 0, amps_on, 0, 0, 0, amps_off, 0, -1);
    if (r) return fail(why, "too many steps");
    return 0;
}

/* --------------------------------------------------------------------------------------------------------- the monitor */
static const SpkMonitor *mon(const ArSeq *s) {return (const SpkMonitor *)s->context;}
static uint32_t vbat_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_vbat(o, c, s->port, t, mon(s)->vbat_uv);}
static uint32_t live_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    return spk_oob(o, c, s->port, t, AR_OP_GET_CFG, mon(s)->live_address, mon(s)->handle, SPK_LIVE_SLOT);
}
static const ArStep kMonitor[] = {
    {"battery voltage to SP_RX", vbat_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0},
    {"live data from SP_RX", live_cmd, AR_OP_GET_CFG, ArGetCfgDone, 0, 0, 0, 0, 0, 0},
};

const ArStep *spk_monitor_steps(int with_vbat, unsigned *count)
{
    *count = with_vbat ? 2u : 1u;
    return with_vbat ? kMonitor : kMonitor + 1;
}
