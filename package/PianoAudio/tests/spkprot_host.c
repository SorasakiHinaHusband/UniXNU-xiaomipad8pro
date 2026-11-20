// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of speaker protection's packets and plan (AudioSpkProt.c) with SYNTHETIC private files (no ACDB byte, no calibration):
//   - the two private files: accepted as documented, refused for every malformation (magic, version, count, sizes, truncation,
//     trailing bytes, a module outside the two graphs, a parameter twice, a TDM interface, an endpoint format, lane or channel map
//     that is not the driver's, no SP_RX or SP_TX entry; Re25 of another size or outside 2..16 ohm);
//   - both GRAPH_OPEN payloads decoded block by block (subgraph, containers and their seven properties, module lists per
//     container, port counts, the data links, the control link and its two properties), each block's exact size, 8-byte aligned;
//     both too big for one packet (the reason they go out of band); the out-of-band command header; the endpoints' and MFC's
//     configuration; in-band parameter packing; Re25, battery and live-data packets byte for byte;
//   - the plan against a DSP model that reads every out-of-band payload from the parameter area: the order (V/I opened and started
//     before the speaker graph, Re25 after the speaker parameters, the stream linked to the MFC), every parameter sent exactly once
//     (in band or out of band), the stop in exact reverse; a refusal and a missing answer at every step undo exactly what was done;
//   - the monitor's two commands and the live slot.
//   cc -std=c11 -Wall -Wextra -Werror spkprot_host.c ../AudioSpkProt.c ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioSpkProt.h"
#include "../AudioSession.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
#define N(a) (sizeof a / sizeof a[0])

// ------------------------------------------------------------------------------------------- synthetic private files
struct E {uint32_t iid, pid, size; const uint8_t *fixed;};
static const uint8_t kMf[12] = {0x80, 0xbb, 0, 0, 32, 0, 4, 0, 1, 0, 0, 0};
static const uint8_t kLaneVi[4] = {1, 0, 0, 0}, kLaneRx[4] = {2, 0, 0, 0}, kOne[4] = {1, 0, 0, 0};
static const uint8_t kChmap[10] = {4, 0, 48, 0, 49, 0, 50, 0, 51, 0};
static const uint8_t kMfc[16] = {0x80, 0xbb, 0, 0, 32, 0, 4, 0, 1, 0, 2, 0, 4, 0, 5, 0};
// The stock file's shape (ids and sizes as the decode lists them), with synthetic values but where the driver checks its own.
static const struct E kStock[] = {
    {0x40ee, 0x10001fc2, 8, 0}, {0x40ee, 0x11111601, 4, kOne}, {0x4587, 0x08001026, 4, kOne}, {0x4587, 0x08001031, 12, 0},
    {0x4587, 0x08001313, 4, 0}, {0x45c2, 0x08001017, 12, kMf}, {0x45c2, 0x08001018, 4, kOne}, {0x45c2, 0x0800106a, 4, kLaneVi},
    {0x45c2, 0x0800108a, 10, kChmap}, {0x45c2, 0x0800113c, 16, 0}, {0x4763, 0x0800103c, 4, 0}, {0x4763, 0x080014fc, 4, 0},
    {0x4007, 0x08001026, 4, kOne}, {0x4007, 0x08001031, 12, 0}, {0x4007, 0x08001313, 4, 0}, {0x4023, 0x08001017, 12, kMf},
    {0x4023, 0x08001018, 4, kOne}, {0x4023, 0x0800106a, 4, kLaneRx}, {0x4023, 0x0800113c, 16, 0}, {0x4032, 0x0800101f, 4, 0},
    {0x4032, 0x08001024, 16, kMfc}, {0x4032, 0x0800104d, 12, 0}, {0x4046, 0x10001fb0, 6180, 0}, {0x4046, 0x10001fc2, 8, 0},
    {0x4046, 0x11111611, 4, kOne}, {0x418e, 0x0800103c, 60, 0}, {0x418e, 0x080014fc, 12, 0},
};
static uint8_t g_file[65536];
static uint32_t build(uint8_t *out, const struct E *e, unsigned n, uint32_t magic, uint32_t version)
{
    uint32_t at = 16;
    put32(out, magic); put32(out + 4, version); put32(out + 8, n); put32(out + 12, 0);
    for (unsigned i = 0; i < n; ++i) {
        put32(out + at, e[i].iid); put32(out + at + 4, e[i].pid); put32(out + at + 8, e[i].size);
        at += 12;
        for (uint32_t b = 0; b < e[i].size; ++b) out[at + b] = e[i].fixed ? e[i].fixed[b] : (uint8_t)(0xa5 ^ (b * 7 + i));
        const uint32_t padded = (e[i].size + 3u) & ~3u;
        for (uint32_t b = e[i].size; b < padded; ++b) out[at + b] = 0;
        at += padded;
    }
    return at;
}
static uint8_t g_re25[16];
static const uint32_t kRe[4] = {27000, 27500, 28000, 28500};   // synthetic, about 6.7 ohm
static void build_re25(void) {for (int c = 0; c < 4; ++c) put32(g_re25 + 4 * c, kRe[c]);}

static void testFiles(void)
{
    SpkProt p;
    const char *why = 0;
    uint32_t n = build(g_file, kStock, N(kStock), SPK_PARAMS_MAGIC, 1);
    CHECK(spk_params_parse(&p, g_file, n, &why) == 0 && p.have_params && p.count == N(kStock));
    CHECK(p.e[22].iid == 0x4046 && p.e[22].pid == 0x10001fb0 && p.e[22].size == 6180 && p.e[22].data[0] == (uint8_t)(0xa5 ^ 22));
    build_re25();
    CHECK(spk_re25_parse(&p, g_re25, 16, &why) == 0 && p.have_re25 && p.re25[0] == 27000 && p.re25[3] == 28500);
    // the header and the walk
    CHECK(spk_params_parse(&p, 0, 0, &why) && strstr(why, "missing"));
    CHECK(spk_params_parse(&p, g_file, 15, &why) && !p.have_params);
    n = build(g_file, kStock, N(kStock), 0x4b505351u, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "PSPK"));
    n = build(g_file, kStock, N(kStock), SPK_PARAMS_MAGIC, 2); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "version"));
    n = build(g_file, kStock, 0, SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "count"));
    n = build(g_file, kStock, N(kStock), SPK_PARAMS_MAGIC, 1);
    CHECK(spk_params_parse(&p, g_file, n - 4, &why) && strstr(why, "size"));
    CHECK(spk_params_parse(&p, g_file, n + 4, &why) && strstr(why, "after the last"));
    put32(g_file + 8, N(kStock) + 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "truncated"));
    put32(g_file + 8, 65); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "count"));
    n = build(g_file, kStock, N(kStock), SPK_PARAMS_MAGIC, 1); put32(g_file + 12, 1);
    CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "count"));
    // the entries
    struct E e[N(kStock) + 1];
    memcpy(e, kStock, sizeof kStock);
    e[3].iid = 0x4012;   // SAL: not in our graphs
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "not in the protection"));
    memcpy(e, kStock, sizeof kStock); e[4] = e[3];
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "twice"));
    memcpy(e, kStock, sizeof kStock); e[9].pid = 0x0800101b;
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "TDM interface"));
    memcpy(e, kStock, sizeof kStock); e[7].fixed = kLaneRx;   // the V/I source on lane 1
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "lane"));
    memcpy(e, kStock, sizeof kStock); e[17].fixed = kLaneVi;  // the sink on lane 0
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "lane"));
    memcpy(e, kStock, sizeof kStock); e[15].fixed = 0;        // the sink's format
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "format"));
    memcpy(e, kStock, sizeof kStock); e[20].size = 12;        // the MFC's map cut
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "size"));
    memcpy(e, kStock, sizeof kStock); e[8].fixed = 0;         // the V/I channel map
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why));
    memcpy(e, kStock, sizeof kStock); e[22].size = SPK_MAX_PARAM + 1;
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "size"));
    memcpy(e, kStock, sizeof kStock); e[22].size = 0;
    n = build(g_file, e, N(kStock), SPK_PARAMS_MAGIC, 1); CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "size"));
    n = build(g_file, kStock + 2, N(kStock) - 2, SPK_PARAMS_MAGIC, 1);   // no SP_TX
    CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "SP_TX"));
    n = build(g_file, kStock, 12, SPK_PARAMS_MAGIC, 1);                  // the V/I graph's alone: no SP_RX
    CHECK(spk_params_parse(&p, g_file, n, &why) && strstr(why, "SP_RX"));
    // Re25
    CHECK(spk_re25_parse(&p, g_re25, 15, &why) && !p.have_re25);
    CHECK(spk_re25_parse(&p, 0, 16, &why) && strstr(why, "16 bytes"));
    put32(g_re25 + 8, 2 * 4096 - 1); CHECK(spk_re25_parse(&p, g_re25, 16, &why) && strstr(why, "2..16"));
    put32(g_re25 + 8, 16 * 4096 + 1); CHECK(spk_re25_parse(&p, g_re25, 16, &why) && strstr(why, "2..16"));
    put32(g_re25 + 8, 2 * 4096); CHECK(!spk_re25_parse(&p, g_re25, 16, &why));
    build_re25();
    CHECK(spk_iid_graph(0x40ee) == 1 && spk_iid_graph(0x45c2) == 1 && spk_iid_graph(0x4046) == 2 && spk_iid_graph(0x4023) == 2);
    CHECK(!spk_iid_graph(0x4012) && !spk_iid_graph(0x6050) && !spk_iid_graph(0));
}

// ----------------------------------------------------------------------------------------------------- APM payload walk
// The blocks of a payload: {iid, pid, size, error} + size bytes + zeros to 8. Returns the count; fills ids/offsets.
static unsigned blocks(const uint8_t *b, uint32_t n, uint32_t *pid, uint32_t *at, uint32_t *size)
{
    unsigned k = 0;
    uint32_t o = 0;
    while (o < n) {
        CHECK(n - o >= 16 && o % 8 == 0 && k < 64);
        pid[k] = le32(b + o + 4); size[k] = le32(b + o + 8); at[k] = o + 16;
        CHECK(le32(b + o + 12) == 0);
        const uint32_t next = o + ((16 + size[k] + 7u) & ~7u);
        CHECK(next <= n);
        for (uint32_t z = o + 16 + size[k]; z < next; ++z) CHECK(b[z] == 0);   // the padding is zeros
        o = next; ++k;
    }
    CHECK(o == n);
    return k;
}

static void check_open(const uint8_t *b, uint32_t n, int vi)
{
    uint32_t pid[64], at[64], size[64];
    CHECK(blocks(b, n, pid, at, size) == 6);
    for (unsigned k = 0; k < 6; ++k) CHECK(le32(b + at[k] - 16) == AR_APM_IID);
    static const uint32_t kOrder[6] = {0x08001001, 0x08001000, 0x08001002, 0x08001003, 0x08001004, 0x08001061};
    for (unsigned k = 0; k < 6; ++k) CHECK(pid[k] == kOrder[k]);
    const uint32_t sg = vi ? SPK_SG_VI : SPK_SG_DEV;
    // the subgraph: perf 2, direction (TX for V/I, RX), scenario any
    const uint8_t *s = b + at[0];
    CHECK(size[0] == 48 && le32(s) == 1 && le32(s + 4) == sg && le32(s + 8) == 3);
    CHECK(le32(s + 12) == 0x0800100E && le32(s + 16) == 4 && le32(s + 20) == 2);
    CHECK(le32(s + 24) == 0x0800100F && le32(s + 32) == (vi ? 1u : 2u));
    CHECK(le32(s + 36) == 0x08001010 && le32(s + 44) == 0xffffffffu);
    // the containers: seven properties each
    const uint8_t *c = b + at[1];
    const unsigned nc = vi ? 1 : 3;
    static const uint32_t kViC[] = {SPK_CONT_VI}, kDevC[] = {SPK_CONT_MFC, SPK_CONT_RX, SPK_CONT_TAIL};
    CHECK(size[1] == 4 + nc * 96 && le32(c) == nc);
    for (unsigned i = 0; i < nc; ++i) {
        const uint8_t *x = c + 4 + 96 * i;
        CHECK(le32(x) == (vi ? kViC[i] : kDevC[i]) && le32(x + 4) == 7);
        CHECK(le32(x + 8) == 0x08001011 && le32(x + 12) == 8 && le32(x + 16) == 1 && le32(x + 20) == 0x0B001001);
        CHECK(le32(x + 24) == 0x08001012 && le32(x + 32) == 4);       // global device position
        CHECK(le32(x + 36) == 0x08001013 && le32(x + 44) == 4096);
        CHECK(le32(x + 48) == 0x08001014 && le32(x + 56) == 2);       // ADSP
        CHECK(le32(x + 60) == 0x08001174 && le32(x + 68) == 1);       // heap
        CHECK(le32(x + 72) == 0x080010CB && le32(x + 80) == 0xffffffffu);
        CHECK(le32(x + 84) == 0x0800124D && le32(x + 92) == (vi ? 1u : 2u));
    }
    // the module lists, per container
    const uint8_t *m = b + at[2];
    CHECK(le32(m) == nc);
    if (vi) {
        CHECK(size[2] == 4 + 12 + 32 && le32(m + 4) == sg && le32(m + 8) == SPK_CONT_VI && le32(m + 12) == 4);
        CHECK(le32(m + 16) == 0x0700101A && le32(m + 20) == 0x4587 && le32(m + 24) == 0x07001011 && le32(m + 28) == 0x4763);
        CHECK(le32(m + 32) == 0x0700100F && le32(m + 36) == 0x45c2 && le32(m + 40) == 0x10001110 && le32(m + 44) == 0x40ee);
    } else {
        CHECK(size[2] == 4 + 20 + 20 + 36);
        CHECK(le32(m + 8) == SPK_CONT_MFC && le32(m + 12) == 1 && le32(m + 16) == 0x07001015 && le32(m + 20) == 0x4032);
        CHECK(le32(m + 28) == SPK_CONT_RX && le32(m + 32) == 1 && le32(m + 36) == 0x10001111 && le32(m + 40) == 0x4046);
        CHECK(le32(m + 48) == SPK_CONT_TAIL && le32(m + 52) == 3 && le32(m + 56) == 0x07001011 && le32(m + 60) == 0x418e);
        CHECK(le32(m + 64) == 0x0700101A && le32(m + 68) == 0x4007 && le32(m + 72) == 0x0700100E && le32(m + 76) == 0x4023);
    }
    // the port counts
    const uint8_t *pp = b + at[3];
    const unsigned nm = vi ? 4 : 5;
    static const uint32_t kViP[][3] = {{0x4587, 1, 1}, {0x4763, 1, 2}, {0x45c2, 0, 1}, {0x40ee, 1, 1}};
    static const uint32_t kDevP[][3] = {{0x4032, 1, 1}, {0x4046, 1, 1}, {0x418e, 1, 24}, {0x4007, 1, 1}, {0x4023, 1, 0}};
    CHECK(size[3] == 4 + nm * 24 && le32(pp) == nm);
    for (unsigned i = 0; i < nm; ++i) {
        const uint8_t *x = pp + 4 + 24 * i;
        const uint32_t *want = vi ? kViP[i] : kDevP[i];
        CHECK(le32(x) == want[0] && le32(x + 4) == 1 && le32(x + 8) == 0x08001015 && le32(x + 12) == 8);
        CHECK(le32(x + 16) == want[1] && le32(x + 20) == want[2]);
    }
    // the data links: no foreign input, the chain end to end
    const uint8_t *l = b + at[4];
    static const uint32_t kViL[][2] = {{0x45c2, 0x4587}, {0x4587, 0x4763}, {0x4763, 0x40ee}};
    static const uint32_t kDevL[][2] = {{0x4032, 0x4046}, {0x4046, 0x418e}, {0x418e, 0x4007}, {0x4007, 0x4023}};
    const unsigned nl = vi ? 3 : 4;
    CHECK(size[4] == 4 + nl * 16 && le32(l) == nl);
    for (unsigned i = 0; i < nl; ++i) {
        const uint8_t *x = l + 4 + 16 * i;
        CHECK(le32(x) == (vi ? kViL[i][0] : kDevL[i][0]) && le32(x + 4) == 1 && le32(x + 8) == (vi ? kViL[i][1] : kDevL[i][1]) && le32(x + 12) == 2);
    }
    // the control link SP_TX <-> SP_RX with its intent list {1, 0x08001231} and property 0x0800136f = 1
    const uint8_t *k = b + at[5];
    CHECK(size[5] == 52 && le32(k) == 1 && le32(k + 4) == 0x40ee && le32(k + 8) == 0x80000000u && le32(k + 12) == 0x4046);
    CHECK(le32(k + 16) == 0x80000000u && le32(k + 20) == 2);
    CHECK(le32(k + 24) == 0x08001062 && le32(k + 28) == 8 && le32(k + 32) == 1 && le32(k + 36) == 0x08001231);
    CHECK(le32(k + 40) == 0x0800136F && le32(k + 44) == 4 && le32(k + 48) == 1);
}

static void testPackets(void)
{
    uint8_t buf[2048], pkt[GPR_MAX_PACKET];
    const uint32_t vi = spk_open_payload(buf, sizeof buf, 1);
    CHECK(vi == 512 && vi > SPK_INBAND_MAX);   // out of band: no in-band packet holds it
    check_open(buf, vi, 1);
    const uint32_t dev = spk_open_payload(buf, sizeof buf, 0);
    CHECK(dev == 776 && dev > SPK_INBAND_MAX);
    check_open(buf, dev, 0);
    CHECK(!spk_open_payload(buf, dev - 1, 0) && !spk_open_payload(buf, 100, 1));
    // the out-of-band header: 40 bytes, the address 32-byte aligned, a handle, a size in words
    GprPacket p;
    uint32_t n = spk_oob(pkt, sizeof pkt, AR_PORT_APM, 0x1234, AR_OP_GRAPH_OPEN, 0x19da08000ull, 0x55, 512);
    CHECK(n == 40 && gpr_decode(pkt, n, &p) == GprOk && p.opcode == AR_OP_GRAPH_OPEN && p.dst_port == AR_APM_IID && p.src_port == AR_PORT_APM);
    CHECK(p.token == 0x1234 && p.src_domain == GPR_DOMAIN_APPS && p.dst_domain == GPR_DOMAIN_ADSP && p.payload_bytes == 16);
    CHECK(le32(p.payload) == 0x9da08000u && le32(p.payload + 4) == 1 && le32(p.payload + 8) == 0x55 && le32(p.payload + 12) == 512);
    CHECK(!spk_oob(pkt, sizeof pkt, 1, 1, AR_OP_SET_CFG, 0x19da08010ull, 0x55, 512));   // not 32-byte aligned
    CHECK(!spk_oob(pkt, sizeof pkt, 1, 1, AR_OP_SET_CFG, 0x19da08000ull, 0, 512));      // no handle
    CHECK(!spk_oob(pkt, sizeof pkt, 1, 1, AR_OP_SET_CFG, 0x19da08000ull, 0x55, 6198));  // not words
    CHECK(!spk_oob(pkt, 39, 1, 1, AR_OP_SET_CFG, 0x19da08000ull, 0x55, 64));
    // the endpoints: our TDM interface (as the passthrough sink), the format, frame size factor, lanes; the V/I channel map; the MFC
    ArTdm t;
    ar_piano_tdm(&t);
    const ArFormat f = {48000, 32, 4};
    uint32_t pid[64], at[64], size[64];
    for (int v = 1; v >= 0; --v) {
        n = spk_endpoint_config(pkt, sizeof pkt, AR_PORT_APM, 7, v, &t, &f);
        CHECK(n && n <= GPR_MAX_PACKET && gpr_decode(pkt, n, &p) == GprOk && p.opcode == AR_OP_SET_CFG);
        CHECK(le32(p.payload) == 0 && le32(p.payload + 8) == 0 && le32(p.payload + 12) == p.payload_bytes - 16);
        const unsigned k = blocks(p.payload + 16, p.payload_bytes - 16, pid, at, size);
        CHECK(k == 5);
        const uint8_t *b = p.payload + 16;
        const uint32_t iid = v ? 0x45c2 : 0x4023;
        for (unsigned i = 0; i < 4; ++i) CHECK(le32(b + at[i] - 16) == iid);
        CHECK(pid[0] == 0x0800101B && size[0] == 28 && le32(b + at[0]) == 0 && le32(b + at[0] + 4) == 1 && le16(b + at[0] + 8) == 1);
        CHECK(le16(b + at[0] + 10) == 1 && le32(b + at[0] + 12) == 0xf && le16(b + at[0] + 16) == 4 && le16(b + at[0] + 18) == 32);
        CHECK(le16(b + at[0] + 20) == t.sync_mode && le16(b + at[0] + 24) == 1);
        CHECK(pid[1] == 0x08001017 && size[1] == 12 && !memcmp(b + at[1], kMf, 12));
        CHECK(pid[2] == 0x08001018 && size[2] == 4 && le32(b + at[2]) == 1);
        CHECK(pid[3] == 0x0800106A && size[3] == 4 && le32(b + at[3]) == (v ? 1u : 2u));
        if (v) CHECK(pid[4] == 0x0800108A && size[4] == 10 && !memcmp(b + at[4], kChmap, 10) && le32(b + at[4] - 16) == 0x45c2);
        else CHECK(pid[4] == 0x08001024 && size[4] == 16 && !memcmp(b + at[4], kMfc, 16) && le32(b + at[4] - 16) == 0x4032);
    }
    const ArFormat stereo = {48000, 32, 2};
    CHECK(!spk_endpoint_config(pkt, sizeof pkt, 1, 1, 0, &t, &stereo) && !spk_endpoint_config(pkt, sizeof pkt, 1, 1, 1, 0, &f));
    // Re25: {0x4046, 0x10001fa7, 36} then u16 0, u16 4, four {0, Re}, padded to 8: 56 bytes of command payload
    const uint32_t re[4] = {27000, 27500, 28000, 28500};
    n = spk_re25(pkt, sizeof pkt, AR_PORT_APM, 9, re);
    CHECK(n == 24 + 16 + 56 && gpr_decode(pkt, n, &p) == GprOk && le32(p.payload + 12) == 56);
    const uint8_t *r = p.payload + 16;
    CHECK(le32(r) == 0x4046 && le32(r + 4) == 0x10001fa7 && le32(r + 8) == 36 && le32(r + 12) == 0);
    CHECK(le16(r + 16) == 0 && le16(r + 18) == 4);
    for (int c = 0; c < 4; ++c) CHECK(le32(r + 20 + 8 * c) == 0 && le32(r + 24 + 8 * c) == re[c]);
    CHECK(le32(r + 52) == 0);
    const uint32_t bad[4] = {27000, 27500, 8191, 28500};
    CHECK(!spk_re25(pkt, sizeof pkt, 1, 1, bad));
    // the battery: {0x4046, 0x10001fb5, 16} {1, uV, 0, 0}: 32 bytes as PAL sends it
    n = spk_vbat(pkt, sizeof pkt, AR_PORT_APM, 10, 4045000);
    CHECK(n == 24 + 16 + 32 && gpr_decode(pkt, n, &p) == GprOk && le32(p.payload + 12) == 32);
    r = p.payload + 16;
    CHECK(le32(r) == 0x4046 && le32(r + 4) == 0x10001fb5 && le32(r + 8) == 16 && le32(r + 16) == 1 && le32(r + 20) == 4045000);
    CHECK(le32(r + 24) == 0 && le32(r + 28) == 0);
    CHECK(!spk_vbat(pkt, sizeof pkt, 1, 1, 0) && !spk_vbat(pkt, sizeof pkt, 1, 1, 2499999) && !spk_vbat(pkt, sizeof pkt, 1, 1, 5000001));
    // the live slot: the header asking for 96 bytes; parsed back after the DSP filled it
    uint8_t slot[SPK_LIVE_SLOT];
    memset(slot, 0xee, sizeof slot);
    spk_live_stage(slot);
    CHECK(le32(slot) == 0x4046 && le32(slot + 4) == 0x10001fab && le32(slot + 8) == 96 && le32(slot + 12) == 0);
    for (unsigned i = 16; i < SPK_LIVE_SLOT; ++i) CHECK(!slot[i]);
    for (unsigned c = 0; c < 4; ++c) for (unsigned k = 0; k < 6; ++k) put32(slot + 16 + 24 * c + 4 * k, 100 * c + k + 1);
    SpkLive live;
    CHECK(spk_live_parse(slot, &live) == 0 && live.re[0] == 1 && live.temp[1] == 102 && live.w2[2] == 203 && live.f0[3] == 304);
    CHECK(live.q[0] == 5 && live.w5[3] == 306);
    put32(slot + 12, 0x12); CHECK(spk_live_parse(slot, &live) == -1);   // the DSP's per-parameter error
    put32(slot + 12, 0); put32(slot + 8, 48); CHECK(spk_live_parse(slot, &live) == -1);
    put32(slot + 8, 96); put32(slot + 4, 0x10001fb6); CHECK(spk_live_parse(slot, &live) == -1);
    // the stream's link to the MFC
    n = ar_open_playback_stream_to(pkt, sizeof pkt, AR_PORT_APM, 3, SPK_IID_MFC);
    CHECK(n && gpr_decode(pkt, n, &p) == GprOk && p.opcode == AR_OP_GRAPH_OPEN);
    // the last link, then the block's 4 bytes of padding
    CHECK(le32(pkt + n - 20) == AR_IID_PLAY_LOG && le32(pkt + n - 16) == 1 && le32(pkt + n - 12) == SPK_IID_MFC && le32(pkt + n - 8) == 2 && !le32(pkt + n - 4));
    uint8_t pkt2[GPR_MAX_PACKET];
    const uint32_t n2 = ar_open_playback_stream(pkt2, sizeof pkt2, AR_PORT_APM, 3);
    CHECK(n2 == n && le32(pkt2 + n - 12) == AR_IID_SPK_LOG && !memcmp(pkt, pkt2, n - 12) && !memcmp(pkt + n - 8, pkt2 + n - 8, 8));
    // ar_parse: APM_CMD_RSP_GET_CFG
    GprPacket q = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = 1, .dst_port = 1, .token = 5, .opcode = AR_OP_RSP_GET_CFG};
    const uint32_t st = 0;
    n = gpr_encode(pkt, sizeof pkt, &q, &st, 4);
    ArAnswer a;
    CHECK(ar_parse(pkt, n, &a) == ArGetCfgDone && a.status == 0 && a.token == 5);
    n = gpr_encode(pkt, sizeof pkt, &q, 0, 0);
    CHECK(ar_parse(pkt, n, &a) == ArBad);
}

// ----------------------------------------------------------------------------------------------- the plan, against a DSP
#define REGION_PHYS 0x9da00000ull
#define REGION_DSP (REGION_PHYS | 1ull << 32)
#define HANDLE 0x55u
static uint8_t g_area[AUDIO_DMA_PARAM_BYTES];
static char lg[128][64];
static unsigned lines;
static void note(const char *s) {if (lines < 128) snprintf(lg[lines++], sizeof lg[0], "%s", s);}
struct Owner {int amps_on_fails, amps_off_calls, amps_on_calls;};
static int amps_on(void *o) {struct Owner *w = o; w->amps_on_calls++; note("AMPS ON"); return w->amps_on_fails;}
static int amps_off(void *o) {struct Owner *w = o; w->amps_off_calls++; note("AMPS OFF"); return 0;}

// Every parameter the DSP received (iid, pid, size, a checksum of the data), from in-band and out-of-band SET_CFGs.
struct Got {uint32_t iid, pid, size, sum; int oob;};
static struct Got g_got[128];
static unsigned g_ngot;
static uint32_t sum(const uint8_t *d, uint32_t n) {uint32_t s = 0; for (uint32_t i = 0; i < n; ++i) s = s * 31 + d[i]; return s;}
static void record(const uint8_t *b, uint32_t n, int oob)
{
    uint32_t pid[64], at[64], size[64];
    const unsigned k = blocks(b, n, pid, at, size);
    for (unsigned i = 0; i < k && g_ngot < 128; ++i)
        g_got[g_ngot++] = (struct Got){le32(b + at[i] - 16), pid[i], size[i], sum(b + at[i], size[i]), oob};
}

static const uint8_t *area_at(uint64_t address, uint32_t bytes)
{
    CHECK(address >= REGION_DSP + AUDIO_DMA_PARAM && address + bytes <= REGION_DSP + AUDIO_DMA_PARAM + AUDIO_DMA_PARAM_BYTES);
    CHECK(address % 32 == 0 && bytes % 4 == 0);
    return g_area + (address - REGION_DSP - AUDIO_DMA_PARAM);
}

static void describe(const uint8_t *b, uint32_t n)
{
    GprPacket p;
    CHECK(gpr_decode(b, n, &p) == GprOk && p.size == n && n <= GPR_MAX_PACKET);
    const uint8_t *d = p.payload;
    const int oob = p.payload_bytes == 16 && p.opcode != AR_OP_MEM_UNMAP && p.opcode != AR_OP_MEM_MAP && le32(d + 8) != 0;
    char s[64];
    if (oob) CHECK(le32(d + 8) == HANDLE);
    switch (p.opcode) {
    case AR_OP_PRM_REQUEST: snprintf(s, sizeof s, "PRM REQ %#x %u", le32(d + 36), le32(d + 40)); break;
    case AR_OP_PRM_RELEASE: snprintf(s, sizeof s, "PRM REL %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_OPEN:
        if (oob) {
            const uint64_t a = le32(d) | (uint64_t)le32(d + 4) << 32;
            const uint8_t *o = area_at(a, le32(d + 12));
            check_open(o, le32(d + 12), le32(o + 20) == SPK_SG_VI);
            snprintf(s, sizeof s, "OPEN %#x oob", le32(o + 20));
        } else {
            snprintf(s, sizeof s, "OPEN %#x", le32(d + 36));
        }
        break;
    case AR_OP_GRAPH_PREPARE: snprintf(s, sizeof s, "PREPARE %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_START: snprintf(s, sizeof s, "START %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_STOP: snprintf(s, sizeof s, "STOP %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_CLOSE: snprintf(s, sizeof s, "CLOSE %#x", le32(d + 36)); break;
    case AR_OP_SET_CFG:
        if (oob) {
            const uint64_t a = le32(d) | (uint64_t)le32(d + 4) << 32;
            const uint8_t *o = area_at(a, le32(d + 12));
            record(o, le32(d + 12), 1);
            snprintf(s, sizeof s, "CFG %#x %#x oob", le32(o), le32(o + 4));
        } else {
            CHECK(le32(d + 8) == 0 && le32(d + 12) == p.payload_bytes - 16);
            if (le32(d + 20) != 0x0800101B && le32(d + 20) != SPK_PID_RE25 && le32(d + 20) != SPK_PID_VBAT && le32(d + 16) != AR_IID_PLAY_SHMEM &&
                le32(d + 16) != AR_IID_PLAY_DEC && le32(d + 16) != AR_IID_PLAY_LOG)
                record(d + 16, p.payload_bytes - 16, 0);
            snprintf(s, sizeof s, "CFG %#x", le32(d + 16));
        }
        break;
    case AR_OP_GET_CFG: {
        CHECK(oob);
        const uint64_t a = le32(d) | (uint64_t)le32(d + 4) << 32;
        const uint8_t *o = area_at(a, le32(d + 12));
        CHECK(le32(d + 12) == SPK_LIVE_SLOT);
        snprintf(s, sizeof s, "GET %#x %#x", le32(o), le32(o + 4));
        break;
    }
    case AR_OP_MEM_MAP: snprintf(s, sizeof s, "MAP %#x%08x %#x", le32(d + 12), le32(d + 8), le32(d + 16)); break;
    case AR_OP_MEM_UNMAP: snprintf(s, sizeof s, "UNMAP %#x", le32(d)); break;
    default: snprintf(s, sizeof s, "OP %#x", p.opcode);
    }
    note(s);
}

struct Dsp {int fail_at, timeout_at; uint32_t fail_status; unsigned commands;};
static uint32_t answer(const uint8_t *q, uint32_t n, uint8_t *out, uint32_t status)
{
    GprPacket p, r;
    CHECK(gpr_decode(q, n, &p) == GprOk);
    memset(&r, 0, sizeof r);
    r.dst_domain = p.src_domain; r.src_domain = p.dst_domain; r.src_port = p.dst_port; r.dst_port = p.src_port; r.token = p.token;
    uint32_t w[2] = {p.opcode, status};
    if (p.opcode == AR_OP_MEM_MAP && !status) {r.opcode = AR_OP_MEM_MAP_DONE; w[0] = HANDLE; return gpr_encode(out, GPR_MAX_PACKET, &r, w, 4);}
    if (p.opcode == AR_OP_GET_CFG && !status) {r.opcode = AR_OP_RSP_GET_CFG; w[0] = 0; return gpr_encode(out, GPR_MAX_PACKET, &r, w, 4);}
    if (p.opcode == AR_OP_PRM_REQUEST) r.opcode = AR_OP_PRM_REQUEST_DONE;
    else if (p.opcode == AR_OP_PRM_RELEASE) r.opcode = AR_OP_PRM_RELEASE_DONE;
    else r.opcode = GPR_OP_BASIC_RESULT;
    return gpr_encode(out, GPR_MAX_PACKET, &r, w, 8);
}
static void drive(ArSeq *s, struct Dsp *d)
{
    uint8_t pkt[GPR_MAX_PACKET], rsp[GPR_MAX_PACKET];
    for (unsigned guard = 0; guard < 200; ++guard) {
        const uint32_t n = ar_seq_next(s, pkt, sizeof pkt);
        if (!n) return;
        describe(pkt, n);
        const int k = (int)d->commands++;
        if (k == d->timeout_at) {ar_seq_timeout(s); continue;}
        const uint32_t m = answer(pkt, n, rsp, k == d->fail_at ? d->fail_status : 0);
        CHECK(ar_seq_receive(s, rsp, m) != ArSeqIgnored);
    }
    CHECK(0);
}

static SpkProt g_prot;
static SpkPlan g_plan;
static struct Owner owner;
static void make_plan(const struct E *e, unsigned n)
{
    const char *why = 0;
    const uint32_t bytes = build(g_file, e, n, SPK_PARAMS_MAGIC, 1);
    CHECK(spk_params_parse(&g_prot, g_file, bytes, &why) == 0);
    build_re25();
    CHECK(spk_re25_parse(&g_prot, g_re25, 16, &why) == 0);
    memset(&g_plan, 0, sizeof g_plan);
    ar_speaker_defaults(&g_plan.base);
    g_plan.base.buffer_phys = REGION_PHYS; g_plan.base.buffer_bytes = AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES;
    g_plan.base.owner = &owner; g_plan.base.amps_on = amps_on; g_plan.base.amps_off = amps_off;
    memset(g_area, 0xcc, sizeof g_area);
    CHECK(spk_plan_init(&g_plan, &g_prot, g_area, sizeof g_area, &why) == 0);
    memset(&owner, 0, sizeof owner);
    lines = 0; g_ngot = 0;
}
static void start(ArSeq *s) {ar_seq_init(s, g_plan.steps, g_plan.count, AR_PORT_APM, AR_PORT_PRM, 0x8000, &g_plan);}

static const char *kUp[] = {"PRM REQ 0x202 6144000", "MAP 0x19da00000 0x10000", "OPEN 0xb00000d5 oob", "CFG 0x45c2", "CFG 0x40ee",
                            "PREPARE 0xb00000d5", "START 0xb00000d5", "OPEN 0xb0000002 oob", "CFG 0x4023", "CFG 0x4007",
                            "CFG 0x4046 0x10001fb0 oob", "CFG 0x4046", "OPEN 0x4001", "CFG 0x6001", "CFG 0x6002", "CFG 0x6004",
                            "PREPARE 0xb0000002", "PREPARE 0x4001", "START 0xb0000002", "START 0x4001", "AMPS ON"};
static const char *kUndo[] = {"PRM REL 0x202", "UNMAP 0x55", "CLOSE 0xb00000d5", 0, 0, 0, "STOP 0xb00000d5", "CLOSE 0xb0000002", 0, 0, 0, 0,
                              "CLOSE 0x4001", 0, 0, 0, 0, 0, "STOP 0xb0000002", "STOP 0x4001", "AMPS OFF"};
_Static_assert(sizeof kUp / sizeof kUp[0] == sizeof kUndo / sizeof kUndo[0], "one undo entry per step");
enum {kSteps = sizeof kUp / sizeof kUp[0]};

static void expect_unwind(unsigned from, int upto, int skip_last_unmap)
{
    unsigned at = from;
    for (int i = upto - 1; i >= 0; --i) {
        if (!kUndo[i] || !strcmp(kUndo[i], "AMPS OFF")) {
            if (kUndo[i] && owner.amps_on_calls) {CHECK(at < lines && !strcmp(lg[at], "AMPS OFF")); ++at;}
            continue;
        }
        if (skip_last_unmap && i == upto - 1 && !strcmp(kUndo[i], "UNMAP 0x55")) continue;   // no handle came back
        if (at >= lines || strcmp(lg[at], kUndo[i])) fprintf(stderr, "undo %u: got %s want %s\n", at, at < lines ? lg[at] : "-", kUndo[i]);
        CHECK(at < lines && !strcmp(lg[at], kUndo[i]));
        ++at;
    }
    CHECK(at == lines);
}

static void testPlan(void)
{
    make_plan(kStock, N(kStock));
    CHECK(g_plan.count == kSteps && g_plan.chunks == 3 && g_plan.vi_chunks == 1 && g_plan.oob_chunks == 1);
    CHECK(g_plan.region_bytes == 0x10000 && g_plan.param_offset == AUDIO_DMA_PARAM && g_plan.staged <= AUDIO_DMA_PARAM_BYTES);
    CHECK(g_plan.vi_open == 0 && g_plan.vi_open_bytes == 512 && g_plan.dev_open == 512 && g_plan.dev_open_bytes == 776);
    CHECK(g_plan.live_slot % 32 == 0 && le32(g_area + g_plan.live_slot) == SPK_IID_SP_RX && le32(g_area + g_plan.live_slot + 4) == SPK_PID_LIVE);
    CHECK(spk_plan_address(&g_plan, 0) == REGION_DSP + AUDIO_DMA_PARAM);
    ArSeq s;
    start(&s);
    struct Dsp d = {-1, -1, 0, 0};
    drive(&s, &d);
    CHECK(s.state == ArSeqUp && s.handle == HANDLE && lines == kSteps);
    for (unsigned i = 0; i < kSteps; ++i) {
        if (strcmp(lg[i], kUp[i])) fprintf(stderr, "step %u: got %s want %s\n", i, lg[i], kUp[i]);
        CHECK(!strcmp(lg[i], kUp[i]));
    }
    // every parameter of the file reached the DSP exactly once, unchanged; the 6180-byte one out of band, the others in band
    CHECK(g_ngot == N(kStock));
    for (unsigned i = 0; i < N(kStock); ++i) {
        unsigned seen = 0;
        for (unsigned k = 0; k < g_ngot; ++k)
            if (g_got[k].iid == g_prot.e[i].iid && g_got[k].pid == g_prot.e[i].pid) {
                ++seen;
                CHECK(g_got[k].size == g_prot.e[i].size && g_got[k].sum == sum(g_prot.e[i].data, g_prot.e[i].size));
                CHECK(g_got[k].oob == (g_prot.e[i].size > 400));
            }
        CHECK(seen == 1);
    }
    CHECK(owner.amps_on_calls == 1);
    // the stop: the exact reverse, the amplifiers first, the V/I graph after the speaker's, the clock last
    ar_seq_stop(&s);
    drive(&s, &d);
    CHECK(s.state == ArSeqDown && s.undo_errors == 0 && owner.amps_off_calls == 1);
    static const char *kDown[] = {"AMPS OFF", "STOP 0x4001", "STOP 0xb0000002", "CLOSE 0x4001", "CLOSE 0xb0000002", "STOP 0xb00000d5",
                                  "CLOSE 0xb00000d5", "UNMAP 0x55", "PRM REL 0x202"};
    CHECK(lines == kSteps + N(kDown));
    for (unsigned i = 0; i < N(kDown); ++i) CHECK(!strcmp(lg[kSteps + i], kDown[i]));
}

static void testFailures(void)
{
    for (int timeout = 0; timeout < 2; ++timeout)
        for (int k = 0; k + 1 < kSteps; ++k) {   // the last step is the amplifiers' local action
            make_plan(kStock, N(kStock));
            ArSeq s;
            start(&s);
            struct Dsp d = {timeout ? -1 : k, timeout ? k : -1, 0x0b, 0};
            drive(&s, &d);
            CHECK(s.state == ArSeqFailed && s.failed_step == k && owner.amps_on_calls == 0 && owner.amps_off_calls == 0);
            CHECK(s.error == (timeout ? ArSeqErrTimeout : ArSeqErrStatus));
            for (int i = 0; i <= k; ++i) CHECK(!strcmp(lg[i], kUp[i]));
            // a refused step is not undone; a step without an answer may have run: undone too (a map without a handle cannot be)
            expect_unwind((unsigned)k + 1, timeout ? k + 1 : k, timeout);
        }
    // the amplifiers fail to start: shut down first, then all the rest
    make_plan(kStock, N(kStock));
    owner.amps_on_fails = -3;
    ArSeq s;
    start(&s);
    struct Dsp d = {-1, -1, 0, 0};
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrLocal && owner.amps_off_calls == 1);
    expect_unwind(kSteps, kSteps, 0);
}

// Many parameters: several in-band packets per graph, each within one packet; several out of band; all sent once.
static void testPacking(void)
{
    static struct E e[64];
    static uint8_t lane_vi[4] = {1, 0, 0, 0};
    unsigned n = 0;
    e[n++] = (struct E){0x40ee, 0x11111601, 4, kOne};
    e[n++] = (struct E){0x45c2, 0x0800106a, 4, lane_vi};
    e[n++] = (struct E){0x40ee, 0x10002000, 600, 0};                            // V/I out of band
    for (unsigned i = 0; i < 30; ++i) e[n++] = (struct E){0x418e, 0x10003000 + i, 40 + (i % 3), 0};   // speaker in band, 4 packets
    e[n++] = (struct E){0x4046, 0x10001fb0, 6180, 0};
    e[n++] = (struct E){0x4046, 0x10004000, 465, 0};                            // 16 + 472 > 472: out of band
    e[n++] = (struct E){0x4046, 0x10004001, 456, 0};                            // 16 + 456 = 472: the largest in band
    make_plan(e, n);
    CHECK(g_plan.vi_chunks == 2 && g_plan.oob_chunks == 3 && g_plan.chunks == 2 + 6 + 2);   // speaker: 424, 432, 432, 424, 128 and 472 bytes in band
    ArSeq s;
    start(&s);
    struct Dsp d = {-1, -1, 0, 0};
    drive(&s, &d);
    CHECK(s.state == ArSeqUp && g_ngot == n);
    for (unsigned i = 0; i < n; ++i) {
        unsigned seen = 0;
        for (unsigned k = 0; k < g_ngot; ++k) seen += g_got[k].iid == g_prot.e[i].iid && g_got[k].pid == g_prot.e[i].pid;
        CHECK(seen == 1);
    }
    // a parameter area too small for what goes out of band
    const char *why = 0;
    SpkPlan small;
    memset(&small, 0, sizeof small);
    small.base = g_plan.base;
    static uint8_t big[64 * 1024];
    struct E huge[3] = {{0x40ee, 0x11111601, 4, kOne}, {0x4046, 0x10005000, 16384, 0}, {0x4046, 0x10005001, 16384, 0}};
    const uint32_t bytes = build(big, huge, 3, SPK_PARAMS_MAGIC, 1);
    SpkProt hp;
    CHECK(spk_params_parse(&hp, big, bytes, &why) == 0 && spk_re25_parse(&hp, g_re25, 16, &why) == 0);
    CHECK(spk_plan_init(&small, &hp, g_area, sizeof g_area, &why) == -1 && strstr(why, "do not fit"));
    // no Re25: refused
    hp.have_re25 = 0;
    CHECK(spk_plan_init(&small, &hp, g_area, sizeof g_area, &why) == -1);
    // a buffer that is not DSP-addressable
    small.base.buffer_phys = 0x8c0000000ull;
    CHECK(spk_plan_init(&small, &g_prot, g_area, sizeof g_area, &why) == -1 && strstr(why, "DSP-addressable"));
}

static void testMonitor(void)
{
    make_plan(kStock, N(kStock));
    SpkMonitor m = {4012000, spk_plan_address(&g_plan, g_plan.live_slot), HANDLE};
    unsigned count;
    const ArStep *steps = spk_monitor_steps(1, &count);
    CHECK(count == 2);
    ArSeq s;
    ar_seq_init(&s, steps, count, AR_PORT_APM, AR_PORT_PRM, 0xa000, &m);
    struct Dsp d = {-1, -1, 0, 0};
    drive(&s, &d);
    CHECK(s.state == ArSeqUp && lines == 2 && !strcmp(lg[0], "CFG 0x4046") && !strcmp(lg[1], "GET 0x4046 0x10001fab"));
    steps = spk_monitor_steps(0, &count);
    CHECK(count == 1);
    lines = 0;
    ar_seq_init(&s, steps, count, AR_PORT_APM, AR_PORT_PRM, 0xa000, &m);
    drive(&s, &d);
    CHECK(s.state == ArSeqUp && lines == 1 && !strcmp(lg[0], "GET 0x4046 0x10001fab"));
    // a refused GET (a basic result with an error) fails the monitor
    lines = 0;
    ar_seq_init(&s, steps, count, AR_PORT_APM, AR_PORT_PRM, 0xa000, &m);
    d = (struct Dsp){0, -1, 0x12, 0};
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error_status == 0x12);
    // a battery voltage out of range cannot be built: the monitor fails at once (the owner sends it only when known)
    m.vbat_uv = 0;
    steps = spk_monitor_steps(1, &count);
    ar_seq_init(&s, steps, count, AR_PORT_APM, AR_PORT_PRM, 0xa000, &m);
    uint8_t pkt[GPR_MAX_PACKET];
    CHECK(ar_seq_next(&s, pkt, sizeof pkt) == 0 && s.state == ArSeqFailed && s.error == ArSeqErrBuild);
}

int main(void)
{
    testFiles();
    testPackets();
    testPlan();
    testFailures();
    testPacking();
    testMonitor();
    printf("PASS spkprot: %u checks\n", checks);
    return 0;
}
