/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
#pragma once
#include "ChargeContract.h"
#include "../include/PianoUsbcContract.h"

/* Called only by the command owner's worker, after serviceMca. An ACK's
 * MessageIn is never exposed as connector status. No commands are generated. */
static inline PianoUsbcObservation piano_usbc_observe(const PianoChargeContract *c,
                                                     int owned, int ready, uint64_t now_ms)
{
    PianoUsbcObservation s = {};
    s.version = PIANO_USBC_OBSERVATION_VERSION;
    s.owned = owned != 0;
    s.ready = owned && ready && c->linked && c->initialized && !c->quarantined;
    s.valid = s.ready && piano_contract_status_fresh(c, now_ms);
    s.generation = c->generation; s.seq = c->seq;
    s.updated_ms = c->updated_ms; s.expires_ms = c->updated_ms + PIANO_CONTRACT_FRESH_MS;
    s.ucsi_version = c->version; s.connector = c->connector;
    if (s.valid) {
        s.status_bytes = c->status_bytes;
        for (unsigned i = 0; i != 16; ++i) s.status[i] = c->status_raw[i];
    }
    return s;
}
