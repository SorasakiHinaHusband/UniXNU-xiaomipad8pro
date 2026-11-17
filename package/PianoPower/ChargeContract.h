/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Read-only UCSI observation over the existing PMIC GLINK owner. This is a
 * command codec, not another GLINK client or a PD policy manager. The owner
 * serializes next/receive/timeout on its worker and must exclude other UCSI
 * users. Facts: USB PD fixed/PPS units; UCSI 1.x/2.x register layouts and
 * command fields; third-party Linux ucsi/ucsi_glink at 1c21cb9895d6 was studied
 * for wire behaviour only. No source text is incorporated.
 *
 * GLINK READ is 12 bytes, its response is header + 48 (1.x) or 528 (2.x)
 * buffer bytes + u32 result. WRITE has the same full buffer and trailing
 * reserved word, and receives a 16-byte transport ACK. Each GET is followed
 * by polling CCI and ACK_CC_CI, including polling ACK_COMPLETE. Neither the
 * transport ACK nor advertised PDOs alone authorize a contract.
 *
 * 1.x MessageIn is 16 bytes: fetch four PDOs, then at most three. A final
 * connector-status read must agree with the initial RDO before publication.
 * A timeout quarantines the unsequenced GLINK transaction until link reset;
 * a late response can never satisfy a subsequent request. Other failures
 * invalidate the snapshot; a later refresh starts with a drain/probe.
 */
#ifndef PIANO_CHARGE_CONTRACT_H
#define PIANO_CHARGE_CONTRACT_H
#include <stdint.h>

#define PIANO_CONTRACT_REQUEST_MAX 544u
#define PIANO_CONTRACT_OWNER 0x800bu
#define PIANO_CONTRACT_READ 0x11u
#define PIANO_CONTRACT_WRITE 0x12u
#define PIANO_CONTRACT_NOTIFY 0x13u
#define PIANO_CONTRACT_FRESH_MS 15000u

enum {PIANO_CONTRACT_NONE = 0, PIANO_CONTRACT_FIXED = 1, PIANO_CONTRACT_PPS = 2};
enum {
    PIANO_CONTRACT_OK = 0, PIANO_CONTRACT_FRAME, PIANO_CONTRACT_RETURN,
    PIANO_CONTRACT_VERSION, PIANO_CONTRACT_CCI, PIANO_CONTRACT_PAYLOAD,
    PIANO_CONTRACT_RDO, PIANO_CONTRACT_CHANGED, PIANO_CONTRACT_TIMEOUT,
    PIANO_CONTRACT_CAPABILITY
};
typedef struct {
    uint8_t protocol, object;
    uint32_t mv, ma, source_ma, source_min_mv, source_max_mv;
} PianoContractDecoded;

static inline uint32_t piano_contract_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void piano_contract_put32(uint8_t *p, uint32_t n) {
    unsigned i; for (i = 0; i != 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
/* Decode only the selected source object. Object positions are never compacted.
 * Fixed: operating current in bits 19:10 (10 mA), max/min in 9:0. PPS:
 * voltage in 20:9 (20 mV), operating current in 6:0 (50 mA).
 * Battery/variable/EPR APDO selections are deliberately unsupported. */
static inline int piano_contract_decode(const uint32_t *pdo, unsigned count, uint32_t rdo, PianoContractDecoded *out) {
    PianoContractDecoded d = {0, 0, 0, 0, 0, 0, 0};
    uint32_t p, kind, max_request;
    unsigned object = (rdo >> 28) & 15u;
    if (!out) return 0;
    *out = d;
    if (!pdo || !count || count > 7 || !object || object > count) return 0;
    p = pdo[object - 1]; kind = p >> 30;
    d.object = (uint8_t)object;
    if (kind == 0) {
        d.protocol = PIANO_CONTRACT_FIXED;
        d.mv = ((p >> 10) & 1023u) * 50u;
        d.ma = ((rdo >> 10) & 1023u) * 10u;
        d.source_ma = (p & 1023u) * 10u;
        d.source_min_mv = d.source_max_mv = d.mv;
        max_request = (rdo & 1023u) * 10u;
        if (d.mv < 3000 || d.mv > 21000 || d.source_ma < 100 || d.source_ma > 5000 ||
            d.ma < 100 || d.ma > d.source_ma || max_request < 100 || max_request > d.source_ma) return 0;
        /* GiveBack changes the low field to a minimum rather than a maximum. */
        if ((rdo & (1u << 27)) ? max_request > d.ma : max_request < d.ma) return 0;
    } else if (kind == 3 && ((p >> 28) & 3u) == 0) {
        d.protocol = PIANO_CONTRACT_PPS;
        d.source_min_mv = ((p >> 8) & 255u) * 100u;
        d.source_max_mv = ((p >> 17) & 255u) * 100u;
        d.source_ma = (p & 127u) * 50u;
        d.mv = ((rdo >> 9) & 4095u) * 20u;
        d.ma = (rdo & 127u) * 50u;
        if ((rdo & ((1u << 27) | (3u << 7) | (3u << 21))) ||
            d.source_min_mv < 3000 || d.source_max_mv > 21000 || d.source_min_mv > d.source_max_mv ||
            d.source_ma < 500 || d.source_ma > 5000 || d.mv < d.source_min_mv || d.mv > d.source_max_mv ||
            d.ma < 50 || d.ma > d.source_ma) return 0;
    } else return 0;
    *out = d;
    return 1;
}

enum {
    PianoContractIdle = 0, PianoContractProbe, PianoContractCapability,
    PianoContractStatus, PianoContractPdos0, PianoContractPdos4,
    PianoContractVerify, PianoContractAck, PianoContractCommit, PianoContractNotifyRead
};
typedef struct {
    /* Published snapshot. valid means PD sink + complete source list + decoded
     * RDO, status_valid also covers an observed non-PD/disconnected source. */
    uint32_t generation, seq;
    uint8_t valid, status_valid, connector, connected, sink, pd, protocol, object, pdos;
    uint16_t version, status_change;
    uint32_t cci, rdo, source_pdo[7], mv, ma, source_ma;
    uint32_t status_flags, capabilities;
    uint64_t updated_ms;
    int32_t last_error;
    /* Diagnostic transport state. in_flight is 0, READ=0x11, or WRITE=0x12. */
    uint8_t in_flight, linked, quarantined, stage, next_stage, wire, wanted, polls;
    uint8_t notify_pending;          /* unknown notification; last snapshot cannot authorize more current */
    uint8_t connector_event, stage_connected, stage_sink, stage_pd, stage_pdos;
    uint16_t stage_change;
    uint32_t stage_flags, stage_rdo, stage_pdo[7];
    uint32_t notifies, failures, timeouts;
    uint64_t command, sent_ms, command_ms, poll_ms, notify_ms;
} PianoChargeContract;

static inline void piano_contract_zero(void *v, unsigned n) {
    uint8_t *p = (uint8_t *)v; unsigned i; for (i = 0; i != n; ++i) p[i] = 0;
}
static inline void piano_contract_init(PianoChargeContract *c) {
    piano_contract_zero(c, (unsigned)sizeof(*c)); c->connector = 1;
}
static inline void piano_contract_invalidate(PianoChargeContract *c, int error, int new_generation) {
    c->valid = c->status_valid = 0;
    c->mv = c->ma = c->source_ma = c->rdo = 0;
    c->pdos = c->protocol = c->object = 0;
    piano_contract_zero(c->source_pdo, (unsigned)sizeof(c->source_pdo));
    c->last_error = error;
    if (new_generation) ++c->generation;
}
static inline void piano_contract_link(PianoChargeContract *c, int up) {
    uint32_t generation = c->generation + 1, seq = c->seq;
    piano_contract_init(c); c->generation = generation; c->seq = seq;
    c->linked = (uint8_t)(up != 0); c->wanted = 0;
    c->stage = up ? PianoContractProbe : PianoContractIdle;
}
static inline void piano_contract_refresh(PianoChargeContract *c) { c->wanted = 1; }
static inline int piano_contract_fresh(const PianoChargeContract *c, uint64_t now_ms) {
    return c->valid && !c->notify_pending && now_ms >= c->updated_ms && now_ms - c->updated_ms <= PIANO_CONTRACT_FRESH_MS;
}
static inline void piano_contract_fail(PianoChargeContract *c, int error, int quarantine) {
    piano_contract_invalidate(c, error, 0); ++c->failures;
    c->in_flight = c->wire = c->wanted = c->notify_pending = 0; c->stage = PianoContractIdle;
    if (quarantine) c->quarantined = 1;
}
static inline void piano_contract_timeout(PianoChargeContract *c, uint64_t now_ms) {
    (void)now_ms; ++c->timeouts; piano_contract_fail(c, PIANO_CONTRACT_TIMEOUT, 1);
}
static inline unsigned piano_contract_buffer_size(uint16_t version) {
    if (version >= 0x0100 && version <= 0x0120 && !(version & 15u)) return 48;
    if (version == 0x0200 || version == 0x0210) return 528;
    return 0;
}
static inline uint64_t piano_contract_stage_command(const PianoChargeContract *c) {
    uint64_t con = (uint64_t)c->connector << 16;
    switch (c->stage) {
        case PianoContractCapability: return 0x06u;
        case PianoContractStatus: case PianoContractVerify: return 0x12u | con;
        case PianoContractPdos0: return 0x10u | con | ((uint64_t)1 << 23) | ((uint64_t)3 << 32) | ((uint64_t)1 << 34);
        case PianoContractPdos4: return 0x10u | con | ((uint64_t)1 << 23) | ((uint64_t)4 << 24) | ((uint64_t)2 << 32) | ((uint64_t)1 << 34);
        case PianoContractAck: return c->command;
        default: return 0;
    }
}
/* Call frequently (worker timer); polls are at most 5/s, with a 5 s command
 * bound. A returned frame becomes the only outstanding UCSI transport request.
 * The caller sends exactly that length, then feeds its reply to receive. */
static inline uint32_t piano_contract_next(PianoChargeContract *c, uint64_t now_ms, uint8_t out[PIANO_CONTRACT_REQUEST_MAX]) {
    unsigned bytes, i; uint32_t opcode, length;
    if (!c->linked || c->quarantined) return 0;
    if (c->notify_pending && now_ms >= c->notify_ms && now_ms - c->notify_ms >= 5000) {
        piano_contract_timeout(c, now_ms); return 0;
    }
    if (c->in_flight) {
        if (now_ms >= c->sent_ms && now_ms - c->sent_ms >= 1500) piano_contract_timeout(c, now_ms);
        return 0;
    }
    if (c->wire == 2 && now_ms >= c->command_ms && now_ms - c->command_ms >= 5000) {
        piano_contract_timeout(c, now_ms); return 0;
    }
    if (c->stage == PianoContractIdle) {
        if (!c->wanted) return 0;
        c->wanted = 0; c->stage = c->last_error ? PianoContractProbe : PianoContractStatus;
        if (!c->version) c->stage = PianoContractProbe;
        c->stage_pdos = 0; c->wire = 0;
    }
    if (c->wire == 2 && now_ms < c->poll_ms) return 0;
    opcode = c->stage == PianoContractProbe || c->stage == PianoContractNotifyRead || c->wire == 2 ? PIANO_CONTRACT_READ : PIANO_CONTRACT_WRITE;
    bytes = opcode == PIANO_CONTRACT_READ ? 0 : piano_contract_buffer_size(c->version);
    if (opcode == PIANO_CONTRACT_WRITE && !bytes) { piano_contract_fail(c, PIANO_CONTRACT_VERSION, 0); return 0; }
    length = 12 + (bytes ? bytes + 4 : 0);
    for (i = 0; i != length; ++i) out[i] = 0;
    piano_contract_put32(out, PIANO_CONTRACT_OWNER); piano_contract_put32(out + 4, 1);
    piano_contract_put32(out + 8, opcode);
    if (opcode == PIANO_CONTRACT_WRITE) {
        c->command = piano_contract_stage_command(c);
        piano_contract_put32(out + 20, (uint32_t)c->command);
        piano_contract_put32(out + 24, (uint32_t)(c->command >> 32));
        c->command_ms = now_ms; c->polls = 0;
    }
    c->in_flight = (uint8_t)opcode; c->sent_ms = now_ms;
    return length;
}
static inline void piano_contract_ack_stage(PianoChargeContract *c, unsigned next, int connector_ack) {
    c->stage = PianoContractAck; c->next_stage = (uint8_t)next;
    c->command = 0x04u | ((uint64_t)1 << 17) | (connector_ack ? (uint64_t)1 << 16 : 0);
    c->wire = 0;
}
static inline void piano_contract_publish(PianoChargeContract *c, uint64_t now_ms) {
    PianoContractDecoded d; unsigned i; int changed;
    int valid = c->stage_connected && c->stage_sink && c->stage_pd &&
                piano_contract_decode(c->stage_pdo, c->stage_pdos, c->stage_rdo, &d);
    changed = c->status_valid && (c->connected != c->stage_connected || c->sink != c->stage_sink ||
              c->pd != c->stage_pd || c->rdo != c->stage_rdo || c->pdos != c->stage_pdos);
    for (i = 0; i != c->stage_pdos; ++i) if (c->status_valid && c->source_pdo[i] != c->stage_pdo[i]) changed = 1;
    if (changed) ++c->generation;
    c->status_valid = 1; c->valid = (uint8_t)valid;
    c->connected = c->stage_connected; c->sink = c->stage_sink; c->pd = c->stage_pd;
    c->status_flags = c->stage_flags; c->status_change = c->stage_change;
    c->rdo = c->stage_rdo; c->pdos = c->stage_pdos;
    for (i = 0; i != 7; ++i) c->source_pdo[i] = c->stage_pdo[i];
    c->protocol = valid ? d.protocol : 0; c->object = valid ? d.object : 0;
    c->mv = valid ? d.mv : 0; c->ma = valid ? d.ma : 0; c->source_ma = valid ? d.source_ma : 0;
    c->last_error = valid || !c->stage_pd ? PIANO_CONTRACT_OK : PIANO_CONTRACT_RDO;
    c->updated_ms = now_ms; ++c->seq; c->stage = PianoContractIdle; c->wire = 0;
}
/* Return 0 not this owner/unexpected late transport ACK; 1 consumed; 2 new
 * complete snapshot or resolved notification; -1 invalidated. Notify only schedules CCI reading: the
 * notification also denotes command completion and is NOT itself a detach. */
static inline int piano_contract_receive(PianoChargeContract *c, uint64_t now_ms, const uint8_t *data, uint32_t length) {
    uint32_t opcode, cci, result, payload, flags, rdo; uint16_t version;
    uint64_t echo; unsigned bytes, i, n, next; int notify_resolved;
    const uint8_t *message;
    if (!data || length < 12 || piano_contract_u32(data) != PIANO_CONTRACT_OWNER) return 0;
    opcode = piano_contract_u32(data + 8);
    if (opcode == PIANO_CONTRACT_NOTIFY) {
        if (length != 24 || piano_contract_u32(data + 16) != 0) return 0;
        ++c->notifies;
        if (c->linked && !c->quarantined) {
            /* The same notification reports command completion and connector
             * change. Suspend authorization until CCI distinguishes them, also
             * during a periodic GET. Preserve the active transaction and the
             * first notification's deadline across a notification storm. */
            if (!c->notify_pending) { c->notify_pending = 1; c->notify_ms = now_ms; }
            if (c->stage == PianoContractIdle) {
                c->command_ms = now_ms; c->stage = PianoContractNotifyRead; c->wire = 0;
            }
        }
        if (c->wire == 2 && !c->in_flight) c->poll_ms = now_ms;
        return 1;
    }
    if (!c->linked || c->quarantined || opcode != c->in_flight || piano_contract_u32(data + 4) != 1) return 0;
    c->in_flight = 0;
    if (opcode == PIANO_CONTRACT_WRITE) {
        if (length != 16) { piano_contract_fail(c, PIANO_CONTRACT_FRAME, 1); return -1; }
        result = piano_contract_u32(data + 12);
        if (result) { piano_contract_fail(c, PIANO_CONTRACT_RETURN, 0); return -1; }
        c->wire = 2; c->poll_ms = now_ms; return 1;
    }
    if (length != 64 && length != 544) { piano_contract_fail(c, PIANO_CONTRACT_FRAME, 1); return -1; }
    bytes = length - 16; version = (uint16_t)(data[12] | (uint16_t)data[13] << 8);
    if (!piano_contract_buffer_size(version) || piano_contract_buffer_size(version) != bytes || (c->version && c->version != version)) {
        piano_contract_fail(c, PIANO_CONTRACT_VERSION, 0); return -1;
    }
    result = piano_contract_u32(data + 12 + bytes);
    if (result) { piano_contract_fail(c, PIANO_CONTRACT_RETURN, 0); return -1; }
    c->version = version; cci = piano_contract_u32(data + 16); c->cci = cci;
    /* When BUSY, the other CCI fields are not authoritative (UCSI CCI
     * semantics); in particular they must not invent a connector event. */
    if (cci & (1u << 28)) {
        if (c->stage == PianoContractProbe) { piano_contract_fail(c, PIANO_CONTRACT_CCI, 0); return -1; }
        c->wire = 2; c->poll_ms = now_ms + 200; return 1;
    }
    payload = (cci >> 8) & 255u;
    if (payload > (bytes == 48 ? 16u : 256u)) { piano_contract_fail(c, PIANO_CONTRACT_PAYLOAD, 0); return -1; }
    /* This tablet has one connector. Never acknowledge an event for a
     * connector whose status/source PDOs we did not query. */
    if (((cci >> 1) & 127u) && ((cci >> 1) & 127u) != c->connector) {
        piano_contract_fail(c, PIANO_CONTRACT_CCI, 0); return -1;
    }
    if (((cci >> 1) & 127u) && !c->connector_event) {
        c->connector_event = 1; piano_contract_invalidate(c, PIANO_CONTRACT_CHANGED, 1);
    }
    notify_resolved = c->notify_pending;
    c->notify_pending = 0;
    if (c->stage == PianoContractNotifyRead) {
        if (cci & ((1u << 25) | (1u << 26) | (1u << 27) | (1u << 30))) {
            piano_contract_fail(c, PIANO_CONTRACT_CCI, 0); return -1;
        }
        if (cci & (1u << 31)) {
            /* A command-complete indication left to drain. Connector change
             * is acknowledged only after its own status has been queried. */
            piano_contract_ack_stage(c, c->connector_event ? PianoContractStatus : PianoContractIdle, 0);
        } else { c->stage = c->connector_event ? PianoContractStatus : PianoContractIdle; c->wire = 0; }
        return 2;   /* authorization changed; no new sample or freshness timestamp */
    }
    if (c->stage == PianoContractProbe) {
        if (cci & (1u << 31)) piano_contract_ack_stage(c, PianoContractCapability, c->connector_event);
        else { c->stage = PianoContractCapability; c->wire = 0; }
        return notify_resolved ? 2 : 1;
    }
    echo = (uint64_t)piano_contract_u32(data + 20) | (uint64_t)piano_contract_u32(data + 24) << 32;
    if (echo != c->command) { piano_contract_fail(c, PIANO_CONTRACT_CCI, 1); return -1; }
    if (cci & ((1u << 25) | (1u << 26) | (1u << 27) | (1u << 30))) {
        piano_contract_fail(c, PIANO_CONTRACT_CCI, 0); return -1;
    }
    if (c->stage == PianoContractAck) {
        if (!(cci & (1u << 29))) { c->wire = 2; c->poll_ms = now_ms + 200; return notify_resolved ? 2 : 1; }
        if (c->command & ((uint64_t)1 << 16)) c->connector_event = 0;
        c->stage = c->next_stage; c->wire = 0;
        if (c->stage == PianoContractCommit) { piano_contract_publish(c, now_ms); return 2; }
        return notify_resolved ? 2 : 1;
    }
    if (!(cci & (1u << 31))) { c->wire = 2; c->poll_ms = now_ms + 200; return notify_resolved ? 2 : 1; }
    message = data + 28; next = PianoContractIdle;
    if (c->stage == PianoContractCapability) {
        if (payload != 16 || message[4] != 1) { piano_contract_fail(c, PIANO_CONTRACT_CAPABILITY, 0); return -1; }
        c->capabilities = piano_contract_u32(message);
        if (!(c->capabilities & 4u) || !(message[5] & 16u)) { piano_contract_fail(c, PIANO_CONTRACT_CAPABILITY, 0); return -1; }
        next = PianoContractStatus;
    } else if (c->stage == PianoContractStatus || c->stage == PianoContractVerify) {
        if (payload < 9 || payload > (bytes == 48 ? 16u : 19u)) { piano_contract_fail(c, PIANO_CONTRACT_PAYLOAD, 0); return -1; }
        flags = piano_contract_u32(message); rdo = piano_contract_u32(message + 4);
        if (c->stage == PianoContractVerify) {
            if ((flags & 0xffff0000u) != (c->stage_flags & 0xffff0000u) || rdo != c->stage_rdo) {
                piano_contract_invalidate(c, PIANO_CONTRACT_CHANGED, 1);
                piano_contract_fail(c, PIANO_CONTRACT_CHANGED, 0); return -1;
            }
            next = PianoContractCommit;
        } else {
            if (c->status_valid && (c->connected != ((flags >> 19) & 1u) ||
                c->sink != !(flags & (1u << 20)) || c->pd != (((flags >> 16) & 7u) == 3u) || c->rdo != rdo))
                piano_contract_invalidate(c, PIANO_CONTRACT_CHANGED, 1);
            c->stage_flags = flags; c->stage_change = (uint16_t)flags; c->stage_rdo = rdo;
            c->stage_connected = (uint8_t)((flags >> 19) & 1u); c->stage_sink = (uint8_t)(!(flags & (1u << 20)));
            c->stage_pd = (uint8_t)(((flags >> 16) & 7u) == 3u);
            c->stage_pdos = 0; piano_contract_zero(c->stage_pdo, (unsigned)sizeof(c->stage_pdo));
            next = c->stage_connected && c->stage_sink && c->stage_pd ? PianoContractPdos0 : PianoContractCommit;
        }
    } else if (c->stage == PianoContractPdos0 || c->stage == PianoContractPdos4) {
        unsigned offset = c->stage == PianoContractPdos0 ? 0 : 4;
        if ((payload & 3u) || payload > (offset ? 12u : 16u) || (!offset && payload < 4)) {
            piano_contract_fail(c, PIANO_CONTRACT_PAYLOAD, 0); return -1;
        }
        n = payload / 4;
        for (i = 0; i != n; ++i) {
            uint32_t p = piano_contract_u32(message + 4 * i);
            if (!p) { piano_contract_fail(c, PIANO_CONTRACT_PAYLOAD, 0); return -1; }
            c->stage_pdo[offset + i] = p;
        }
        c->stage_pdos = (uint8_t)(offset + n);
        if (!offset && (c->stage_pdo[0] >> 30 || ((c->stage_pdo[0] >> 10) & 1023u) != 100u || !(c->stage_pdo[0] & 1023u))) {
            piano_contract_fail(c, PIANO_CONTRACT_PAYLOAD, 0); return -1;
        }
        next = !offset && n == 4 ? PianoContractPdos4 : PianoContractVerify;
    } else { piano_contract_fail(c, PIANO_CONTRACT_CCI, 0); return -1; }
    piano_contract_ack_stage(c, next, c->connector_event && (c->stage == PianoContractStatus || c->stage == PianoContractVerify));
    return notify_resolved ? 2 : 1;
}
#endif
