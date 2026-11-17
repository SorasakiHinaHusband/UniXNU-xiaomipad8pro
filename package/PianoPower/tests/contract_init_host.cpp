// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// A wire-level cold PPM, independent of the observer's stages. In notifications-
// disabled state only RESET/ENABLE are processed (UCSI 1.2 section 4.3).
#include "ChargeContract.h"
#include <cstdio>
#include <vector>
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
struct ColdPpm {
    PianoChargeContract c{};
    uint64_t now = 1, enabled = 0, pending = 0;
    uint32_t cci = 0;
    unsigned version = 0x110, resetDelay = 2, delay = 0, commandDelay = 1;
    unsigned droppedGets = 0, resets = 0, ackErrors = 0, snapshots = 0;
    bool stuckReset = false, stuckEnable = false, stuckAck = false, dropRead = false, rejectEnable = false;
    bool dirtyReset = false, alwaysDirtyReset = false, stuckClear = false, notify = false;
    uint8_t payload[16] = {};
    std::vector<uint64_t> commands;
    ColdPpm() { piano_contract_init(&c); piano_contract_link(&c, 1); }
    void indication(unsigned receiver = 0) {
        uint8_t m[24] = {};
        piano_contract_put32(m, PIANO_CONTRACT_OWNER); piano_contract_put32(m+4, 2);
        piano_contract_put32(m+8, PIANO_CONTRACT_NOTIFY); piano_contract_put32(m+12, cci);
        piano_contract_put32(m+16, receiver);
        piano_contract_receive(&c, now, m, 24);
    }
    void completeCommand() {
        const unsigned cmd = pending & 255u;
        for (auto &b : payload) b = 0;
        if (cmd == 1) {
            if (stuckReset) return;
            cci = alwaysDirtyReset || (dirtyReset && resets == 1) ? 1u<<31 : 1u<<27;
        } else if (cmd == 4) {
            if (stuckAck) return;
            if (!(cci & (1u<<31))) ++ackErrors;
            cci = 1u<<29;
        } else if (cmd == 5) {
            if (stuckEnable || (stuckClear && pending == 5)) return;
            enabled = pending & ~uint64_t(0xffffu);
            cci = 1u<<31;
            if (rejectEnable && enabled) cci |= 1u<<30;
        } else if (cmd == 6) {
            cci = 1u<<31 | 16u<<8; payload[0]=4; payload[4]=1; payload[5]=16;
        } else if (cmd == 0x12) {
            cci = 1u<<31 | 9u<<8;
            piano_contract_put32(payload, 1u<<19 | 1u<<16); // attached non-PD sink
        } else ++ackErrors;
        pending = 0;
        if (notify && enabled && cmd != 1) indication();
    }
    void step() {
        uint8_t req[544] = {}, out[544] = {};
        auto n = piano_contract_next(&c, now, req);
        if (!n) { now += 20; return; }
        const unsigned op = piano_contract_u32(req+8);
        CHECK(piano_contract_u32(req)==0x800b && piano_contract_u32(req+4)==1);
        piano_contract_put32(out, 0x800b); piano_contract_put32(out+4, 1); piano_contract_put32(out+8, op);
        if (op == 0x12) {
            CHECK(n == (version < 0x200 ? 64u : 544u));
            uint64_t ctrl=piano_contract_u32(req+20) | uint64_t(piano_contract_u32(req+24))<<32;
            commands.push_back(ctrl);
            const unsigned cmd=ctrl&255u;
            if (!enabled && cmd!=1 && cmd!=5) ++droppedGets; // transport still ACKs the WRITE
            else {
                if (cmd==1) { enabled=0; ++resets; cci=0; }
                else if (cmd!=4 && (cci&(1u<<31)) && (commands.size()<2 || (commands[commands.size()-2]&255u)!=5 || (cci&(1u<<27)))) ++ackErrors;
                cci &= ~(1u<<27); // RESET_COMPLETE clears on the next accepted command
                pending=ctrl; delay=cmd==1 ? resetDelay : commandDelay;
            }
            piano_contract_receive(&c, now, out, 16);
        } else {
            CHECK(op==0x11 && n==12);
            if (dropRead) { now+=20; return; }
            if (pending) { if (delay) --delay; else completeCommand(); }
            out[12]=uint8_t(version); out[13]=uint8_t(version>>8);
            piano_contract_put32(out+16, cci); // deliberately no CONTROL echo
            for (unsigned i=0;i<16;++i) out[28+i]=payload[i];
            const auto result=piano_contract_receive(&c, now, out, version<0x200 ? 64 : 544);
            CHECK(result>=0 || c.last_error);
            snapshots=c.seq;
        }
        now+=20;
    }
    void run(unsigned bound=12000) {
        const auto end=now+bound;
        while (now<end && !c.quarantined && !c.status_valid && !c.last_error) step();
    }
};
int main() {
    for (unsigned version : {0x100u,0x110u,0x120u,0x200u,0x210u}) {
        ColdPpm p; p.version=version; p.run();
        if (!p.c.status_valid) std::printf("cold v%x: cmd=%llx cci=%x error=%d timeout=%u dropped-GETs=%u reads-replied; no FIFO pressure\n",
            version,(unsigned long long)p.c.command,p.c.cci,p.c.last_error,p.c.timeouts,p.droppedGets);
        CHECK(p.c.status_valid && p.c.seq==1 && !p.c.valid && !p.c.pd);
        CHECK(!p.droppedGets && !p.ackErrors && p.resets==1 && !p.c.quarantined);
        CHECK(p.commands.size()>=8);
        if (p.commands.size()>=8) {
            CHECK((p.commands[0]&255u)==1 && p.commands[1]==0x80010005ull);
            CHECK(p.commands[2]==0x20004 && p.commands[3]==6 && p.commands[4]==0x20004);
            CHECK((p.commands[5]&255u)==5 && (p.commands[5]&(1ull<<30)));
            CHECK(!(p.commands[5]&((1ull<<17)|(1ull<<23)|(1ull<<24)))); // unsupported supply/reset/altmode events
            CHECK(p.commands[6]==0x20004 && p.commands[7]==0x10012);
        }
        CHECK(piano_contract_status_fresh(&p.c,p.now));
        auto seq=p.c.seq; auto resets=p.resets; auto written=p.commands.size();
        piano_contract_refresh(&p.c); p.c.status_valid=0; p.run();
        CHECK(p.c.seq==seq+1 && p.resets==resets && p.commands.size()==written+2);
        piano_contract_link(&p.c,0); piano_contract_link(&p.c,1);
        p.enabled=0; p.cci=0; p.run();
        CHECK(p.c.status_valid && p.resets==resets+1 && !p.c.quarantined);
    }
    ColdPpm latched; latched.cci=1u<<27; latched.run();
    CHECK(latched.c.status_valid && latched.commands.size()>=2);
    if (latched.commands.size()>=2) CHECK(latched.commands[0]==5 && latched.commands[1]==1);
    CHECK(!latched.ackErrors && !latched.droppedGets);
    ColdPpm clear; clear.cci=1u<<27; clear.stuckClear=true; clear.run();
    CHECK(clear.c.status_valid && clear.now>=5000 && !clear.c.quarantined);
    ColdPpm dirty; dirty.dirtyReset=true; dirty.run();
    CHECK(dirty.c.status_valid && dirty.resets==2 && !dirty.ackErrors);
    ColdPpm repeated; repeated.alwaysDirtyReset=true; repeated.run();
    CHECK(repeated.c.quarantined && repeated.c.last_error==PIANO_CONTRACT_TIMEOUT && repeated.resets>2 && repeated.now<11000);
    ColdPpm slow; slow.resetDelay=12; slow.run(); CHECK(slow.c.status_valid);
    ColdPpm stuck; stuck.stuckReset=true; stuck.run();
    CHECK(stuck.c.quarantined && stuck.c.last_error==PIANO_CONTRACT_TIMEOUT && !stuck.c.status_valid);
    ColdPpm enable; enable.stuckEnable=true; enable.run();
    CHECK(enable.c.quarantined && enable.c.last_error==PIANO_CONTRACT_TIMEOUT && !enable.c.status_valid);
    ColdPpm reject; reject.rejectEnable=true; reject.run();
    CHECK(reject.c.last_error==PIANO_CONTRACT_CCI && !reject.c.status_valid);
    ColdPpm ack; ack.stuckAck=true; ack.run();
    CHECK(ack.c.quarantined && ack.c.last_error==PIANO_CONTRACT_TIMEOUT && !ack.c.status_valid);
    CHECK(ack.commands.size()==3 && ack.commands.back()==0x20004); // no GET before ACK_COMPLETE
    ColdPpm wire; wire.dropRead=true; wire.run();
    CHECK(wire.c.quarantined && wire.c.timeouts==1 && wire.commands.empty());
    ColdPpm notified; notified.notify=true; notified.run();
    CHECK(notified.c.status_valid && notified.c.notifies && !notified.c.quarantined && !notified.ackErrors);
    std::printf("cold PPM: %u checks / %u failures\n",checks,failures);
    return failures?1:0;
}
