/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Speaker protection on the audio DSP (piano-audio-spk=1, docs/drivers/audio.md section 12): the stock FourSemi FSADSP pair that
 * Android opens for this tablet's speakers, instead of the passthrough device graph (PianoAudioReach.h's 0x4005):
 *   V/I subgraph 0xb00000d5, opened and started first:
 *     [TDM source 0x45c2 (secondary TDM TX, lane 0: the amplifiers' V/I slots)] -> [LOG 0x4587] -> [SPLITTER 0x4763] -> [SP_TX 0x40ee]
 *   speaker subgraph 0xb0000002 without its foreign inputs (no SAL), fed by our stream subgraph 0x4001:
 *     [MFC 0x4032] -> [SP_RX 0x4046, own container] -> [SPLITTER 0x418e] -> [LOG 0x4007] -> [TDM sink 0x4023 (secondary TDM RX, lane 1)]
 *   the control link SP_TX 0x40ee port 0x80000000 <-> SP_RX 0x4046 port 0x80000000, in both opens (as the ACDB holds it in both
 *   subgraphs' data: the first open names a peer that is not there yet, the second connects them).
 * Subgraph, container and module ids, ports and properties are the stock ACDB's graph data for graph keys {DEVICERX speaker,
 * stream low latency, instance 1, device PP MIPLAY} and {DEVICETX VI feedback} (private/host-checks/20261121-audio-spk/ANALYSIS.md).
 * The tuning is not here: every ACDB parameter of the two subgraphs' default calibration comes at run time from the owner's own
 * ACDB through tooling/audio/spk-prot-extract.py (the private file PianoAudio/spk-prot.bin, below), and the per-unit Re25 from the
 * owner's own persist copy (PianoAudio/spk-re25.bin); neither is ever committed.
 *
 * Packet sizes: both GRAPH_OPENs (about 530 and 790 bytes) and the 6180-byte SP_RX configuration are larger than one GLINK intent
 * (GPR_MAX_PACKET), so they go out of band as the stock graph services send everything: the payload in the mapped region (the
 * speaker periods at its start, this module's parameter area at AUDIO_DMA_PARAM), the packet only the APM command header naming its
 * address, the map handle and its size. Parameters that fit go in band, packed several to a SET_CFG.
 *
 * Private file formats (little endian; tooling/audio/spk-prot-extract.py writes both):
 *   spk-prot.bin   u32 magic 'PSPK' (0x4b505350), u32 version 1, u32 count (1..SPK_MAX_ENTRIES), u32 0; then count entries:
 *                  u32 module instance id, u32 parameter id, u32 size (1..SPK_MAX_PARAM), size bytes zero-padded to a multiple of 4.
 *                  Nothing after the last entry. Only the nine instance ids of the two graphs above are accepted, and the endpoints'
 *                  and the MFC's formats and lanes, when present, must be the values this driver sends itself.
 *   spk-re25.bin   16 bytes: four u32 Re25 in Q12 (ohm x 4096), CH0..CH3 = TDM slots 0..3 (the persist file fsalgo_rdc.bin as it is);
 *                  each 2..16 ohm.
 * Portable C, new code for PianoAudio (layouts: the AudioReach APM API, BSD-3-Clause-Clear, read as a specification). No I/O. */
#ifndef PIANO_AUDIO_SPK_PROT_H
#define PIANO_AUDIO_SPK_PROT_H
#include <stdint.h>
#include "AudioSequence.h"
#include "PianoGpr.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SPK_SG_VI 0xb00000d5u
#define SPK_SG_DEV 0xb0000002u
#define SPK_CONT_VI 0xe0000122u
#define SPK_CONT_MFC 0xe0000098u
#define SPK_CONT_RX 0xe0000250u
#define SPK_CONT_TAIL 0xe0000256u
#define SPK_MOD_TDM_SOURCE 0x0700100Fu
#define SPK_MOD_SPLITTER 0x07001011u
#define SPK_MOD_SP_TX 0x10001110u
#define SPK_MOD_SP_RX 0x10001111u
#define SPK_IID_VI_TDM 0x45c2u
#define SPK_IID_VI_LOG 0x4587u
#define SPK_IID_VI_SPLIT 0x4763u
#define SPK_IID_SP_TX 0x40eeu
#define SPK_IID_MFC 0x4032u
#define SPK_IID_SP_RX 0x4046u
#define SPK_IID_SPLIT 0x418eu
#define SPK_IID_LOG 0x4007u
#define SPK_IID_TDM 0x4023u
#define SPK_LANE_RX 2u                  /* PARAM_ID_TDM_LANE_CFG: lane 1 (gpio124 i2s1_data1) */
#define SPK_LANE_VI 1u                  /* lane 0 (gpio122 i2s1_data0, the amplifiers' feedback) */
/* SP_RX's run-time parameters (not in the ACDB: libar-pal and libfsalgocal build them) */
#define SPK_PID_RE25 0x10001fa7u        /* u16 version 0, u16 count 4, 4 x {u32 trim 0, u32 Re25 Q12}: 36 bytes */
#define SPK_PID_LIVE 0x10001fabu        /* get: 4 x 24 bytes {u32 Re Q12, u32 temperature, u32 ?, u32 f0 Q8, u32 Q, u32 ?} */
#define SPK_PID_VBAT 0x10001fb5u        /* set: {u32 1 (voltage), u32 battery uV, u32 0, u32 0}: PAL's battery monitor, V mode only */
#define SPK_RE25_BYTES 36u
#define SPK_LIVE_BYTES 96u
#define SPK_VBAT_BYTES 16u
#define SPK_MONITOR_MS 2000u            /* PAL's interval for the battery voltage; live data read with it */

#define SPK_PARAMS_MAGIC 0x4b505350u    /* "PSPK" */
#define SPK_PARAMS_VERSION 1u
#define SPK_MAX_ENTRIES 64u
#define SPK_MAX_PARAM 16384u
#define SPK_RE25_FILE_BYTES 16u
#define SPK_RE25_MIN (2u * 4096u)       /* Q12 */
#define SPK_RE25_MAX (16u * 4096u)
/* One SET_CFG's parameter blocks in band: a packet less the GPR header and the APM command header. */
#define SPK_INBAND_MAX (GPR_MAX_PACKET - 24u - 16u)

typedef struct {
    uint32_t iid, pid, size;
    const uint8_t *data;                /* into the caller's file, kept for the boot */
} SpkParam;

typedef struct {
    SpkParam e[SPK_MAX_ENTRIES];
    unsigned count;
    uint32_t bytes;                     /* the parameters' data */
    uint32_t re25[4];                   /* Q12, TDM slot order */
    int have_params, have_re25;
} SpkProt;

/* 0, or -1 with *why (a static string): the file as above. */
int spk_params_parse(SpkProt *p, const uint8_t *file, uint32_t bytes, const char **why);
int spk_re25_parse(SpkProt *p, const uint8_t *file, uint32_t bytes, const char **why);
/* 1 when the instance belongs to the V/I subgraph, 2 to the speaker subgraph, 0 otherwise. */
int spk_iid_graph(uint32_t iid);

/* ---- packets (each builder returns the packet's size, or 0 when it does not fit or its input is invalid) */
/* GRAPH_OPEN's payload (the parameter blocks only, as it goes out of band) into buf: the V/I subgraph (vi 1) or the speaker one. */
uint32_t spk_open_payload(uint8_t *buf, uint32_t cap, int vi);
/* An APM command whose payload is in the mapped region: the command header only (address, handle, bytes). */
uint32_t spk_oob(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t opcode, uint64_t address, uint32_t handle,
                 uint32_t bytes);
/* The endpoint's interface (t, as the passthrough sink: AR's ar_piano_tdm), media format, frame size factor and lane; the V/I source
 * also its channel map (4 channels, the V/I types 48..51); the speaker side also the MFC's output (f, FL FR LS RS). */
uint32_t spk_endpoint_config(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, int vi, const ArTdm *t, const ArFormat *f);
/* One SET_CFG of the file's entries order[first .. first + count) in band (exact sizes, each block 8-aligned). */
uint32_t spk_set_params(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const SpkProt *p, const uint8_t *order,
                        unsigned first, unsigned count);
/* One parameter block (header and data, padded to 8) into buf: an out-of-band payload. */
uint32_t spk_param_block(uint8_t *buf, uint32_t cap, uint32_t iid, uint32_t pid, const uint8_t *data, uint32_t size);
uint32_t spk_re25(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const uint32_t re25[4]);
uint32_t spk_vbat(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t microvolts);
/* The live-data GET's out-of-band slot: the parameter header asking for SPK_LIVE_BYTES, the data zeroed (SPK_LIVE_SLOT bytes). */
#define SPK_LIVE_SLOT (16u + SPK_LIVE_BYTES)
void spk_live_stage(uint8_t slot[SPK_LIVE_SLOT]);
typedef struct {
    uint32_t re[4], temp[4], w2[4], f0[4], q[4], w5[4];
} SpkLive;
/* After the GET's answer: 0 and the four channels, or -1 (the DSP's header names another parameter, another size or an error). */
int spk_live_parse(const uint8_t slot[SPK_LIVE_SLOT], SpkLive *out);

/* ---- the plan: the passthrough speaker plan's shape with the protection pair, as one ArSeq (Android's order, round 2):
 *   bit clock (not with +2); map [periods | parameter area]; open speaker (oob); speaker endpoint and MFC; speaker parameters (in
 *   band, oob for the big ones); Re25; open the stream (to the MFC); its shared memory format, decoder/converter, logger; open V/I
 *   (oob); V/I endpoint; V/I parameters; prepare V/I; start V/I; prepare speaker, prepare stream; start speaker, start stream;
 *   amplifiers on.
 * Undone in reverse: amplifiers off, stop stream, stop speaker, stop V/I, close V/I, close stream, close speaker, unmap, clock.
 * +16 (the round-1 control): bit clock; map; open V/I .. start V/I; open speaker .. Re25; the stream; prepare, start; amplifiers. */
#define SPK_MAX_CHUNKS 24u
#define SPK_MAX_STEPS 56u
/* Round 2 (2026-11-21, spk-prot1: the DSP refused START of the V/I graph, status 0x1, after accepting its open, endpoint,
 * parameters and prepare; private/.../20261121-audio-spk ANALYSIS.md section 7). The default order is now Android's: the speaker
 * graph (SP_RX, the IMCL link's peer) is opened and configured before the V/I graph is opened; V/I is prepared and started before
 * the speaker graph is prepared and started. The request (piano-audio-spk, machdep.piano_audio_spk) is a bit mask, odd = protection:
 *   +2 no CPU bit-clock request: the endpoints own the clock through the ACDB's HW_INTF_CLK_CFG (attribute 4, inverted), as Android;
 *   +4 the CPU request with attribute 4 (CLOCK_ATTRIBUTE_INVERT_COUPLE_NO), agreeing with HW_INTF_CLK_CFG;
 *   +8 HW_INTF_CLK_CFG is not sent: the CPU owns a non-inverted clock (attribute 1), as the passthrough graph;
 *   +16 the round-1 order (V/I opened and started before the speaker graph is opened), the control.
 * +2 and +4 exclude each other. */
#define SPK_VAR_ENDPOINT_CLOCK 2u
#define SPK_VAR_INVERT_CLOCK 4u
#define SPK_VAR_NO_CLK_CFG 8u
#define SPK_VAR_VI_FIRST 16u
#define SPK_VAR_ALL 31u
#define SPK_PID_HW_INTF_CLK 0x0800113Cu
/* 1 for a valid request: 0 (passthrough) or an odd mask within SPK_VAR_ALL without both +2 and +4. */
int spk_request_valid(int request);
/* The variant as words for the log ("Android order, endpoint clock", ...): a static string per mask bit set, written into buf. */
const char *spk_variant_text(uint32_t variant, char *buf, unsigned cap);
typedef struct {
    uint8_t vi, oob;
    uint16_t first, count;              /* in band: order[first .. first + count) */
    uint32_t offset, bytes;             /* out of band: in the parameter area */
} SpkChunk;
typedef struct {
    ArSpeakerPlan base;                 /* tdm, formats, the period buffer (the region's start), stream id, owner, amplifiers */
    const SpkProt *prot;
    uint32_t region_bytes;              /* mapped from base.buffer_phys: AUDIO_DMA_PARAM + AUDIO_DMA_PARAM_BYTES */
    uint32_t param_offset;              /* the parameter area in the region */
    uint32_t vi_open, vi_open_bytes, dev_open, dev_open_bytes, live_slot, staged;   /* offsets in the parameter area */
    uint8_t order[SPK_MAX_ENTRIES];
    SpkChunk chunk[SPK_MAX_CHUNKS];
    unsigned chunks, vi_chunks, oob_chunks;
    ArStep steps[SPK_MAX_STEPS];
    int8_t step_chunk[SPK_MAX_STEPS];
    unsigned count;
    uint32_t variant;                   /* the request's mask (SPK_VAR_*); set by the caller with base, before spk_plan_init */
    unsigned skipped;                   /* the file's parameters this variant does not send (HW_INTF_CLK_CFG with +8) */
} SpkPlan;
/* base and variant must be filled first (ar_speaker_defaults and the period buffer, as the passthrough plan). Lays the steps out and writes every
 * out-of-band payload into area (the parameter area as the CPU sees it, area_bytes long; the owner publishes it to the DSP before
 * the plan runs). 0, or -1 with *why. */
int spk_plan_init(SpkPlan *p, const SpkProt *prot, uint8_t *area, uint32_t area_bytes, const char **why);
/* The DSP address of the parameter area's byte at offset (the region's DSP address plus param_offset). */
uint64_t spk_plan_address(const SpkPlan *p, uint32_t offset);

/* ---- the monitor every SPK_MONITOR_MS while the protected stream runs: the battery voltage set (when known), the live data read */
typedef struct {
    uint32_t vbat_uv;                   /* 0: not sent */
    uint64_t live_address;              /* the live slot's DSP address and the region's map handle */
    uint32_t handle;
} SpkMonitor;
const ArStep *spk_monitor_steps(int with_vbat, unsigned *count);

#ifdef __cplusplus
}
#endif
#endif
