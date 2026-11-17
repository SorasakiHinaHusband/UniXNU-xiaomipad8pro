// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "ChargeContract.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static uint32_t fixed(unsigned mv, unsigned ma) { return (mv / 50) << 10 | ma / 10; }
static uint32_t pps(unsigned lo, unsigned hi, unsigned ma) { return 3u << 30 | (hi / 100) << 17 | (lo / 100) << 8 | ma / 50; }
static uint32_t fixedRdo(unsigned object, unsigned ma) { return object << 28 | (ma / 10) << 10 | ma / 10; }
static uint32_t ppsRdo(unsigned object, unsigned mv, unsigned ma) { return object << 28 | (mv / 20) << 9 | ma / 50; }
static void decoding() {
    uint32_t list[] = {fixed(5000,3000),fixed(9000,2000),pps(3300,11000,3000)};
    PianoContractDecoded d;
    assert(piano_contract_decode(list,3,fixedRdo(2,1600),&d));
    assert(d.object==2 && d.protocol==PIANO_CONTRACT_FIXED && d.mv==9000 && d.ma==1600 && d.source_ma==2000);
    assert(piano_contract_decode(list,3,fixedRdo(2,1990),&d) && d.ma==1990);
    assert(!piano_contract_decode(list,3,fixedRdo(2,2010),&d));
    assert(!d.protocol && !d.mv && !d.ma);
    assert(!piano_contract_decode(list,3,fixedRdo(0,500),&d));
    assert(!piano_contract_decode(list,3,fixedRdo(4,500),&d));
    assert(!piano_contract_decode(list,8,fixedRdo(1,500),&d));
    assert(!piano_contract_decode(list,3,2u<<28 | 160u<<10 | 159u,&d));
    assert(piano_contract_decode(list,3,(2u<<28)|(1u<<27)|(160u<<10)|150u,&d));
    assert(!piano_contract_decode(list,3,(2u<<28)|(1u<<27)|(160u<<10)|161u,&d));
    assert(piano_contract_decode(list,3,ppsRdo(3,9020,1550),&d));
    assert(d.protocol==PIANO_CONTRACT_PPS && d.mv==9020 && d.ma==1550 && d.source_ma==3000);
    assert(piano_contract_decode(list,3,ppsRdo(3,3300,50),&d));
    assert(piano_contract_decode(list,3,ppsRdo(3,11000,3000),&d));
    assert(!piano_contract_decode(list,3,ppsRdo(3,3280,500),&d));
    assert(!piano_contract_decode(list,3,ppsRdo(3,11020,500),&d));
    assert(!piano_contract_decode(list,3,ppsRdo(3,9000,3050),&d));
    assert(!piano_contract_decode(list,3,ppsRdo(3,9000,0),&d));
    assert(!piano_contract_decode(list,3,ppsRdo(3,9000,500)|(1u<<8),&d));
    list[2] |= 1u<<28; // EPR APDO is not PPS
    assert(!piano_contract_decode(list,3,ppsRdo(3,9000,500),&d));
}

struct Ppm {
    PianoChargeContract c{};
    uint64_t now=10;
    uint64_t control=0;
    uint32_t status=(1u<<19)|(3u<<16);
    uint32_t rdo=fixedRdo(2,1600);
    uint32_t pdos[7]={fixed(5000,3000),fixed(9000,2000),pps(3300,11000,3000),fixed(12000,1500),fixed(15000,1500),0,0};
    unsigned count=5;
    uint16_t version=0x110;
    unsigned stage=0, reads=0, writes=0, acks=0, connectorAcks=0, snapshots=0;
    bool busy=false, shortStatus=false, changeOnVerify=false;
    bool transportError=false, ucsiError=false, wrongEcho=false, shortPayload=false, missingPdoFeature=false;
    unsigned cciConnector=0, connectorCount=1;
    Ppm() { piano_contract_init(&c); piano_contract_link(&c,1); }
    int step() {
        uint8_t request[PIANO_CONTRACT_REQUEST_MAX];
        auto n=piano_contract_next(&c,now,request); ++now;
        if (!n) return 0;
        assert(piano_contract_u32(request)==PIANO_CONTRACT_OWNER && piano_contract_u32(request+4)==1);
        unsigned op=piano_contract_u32(request+8);
        if(op==PIANO_CONTRACT_WRITE) {
            ++writes;
            assert(n==(version<0x200 ? 64u:544u));
            control=uint64_t(piano_contract_u32(request+20)) | uint64_t(piano_contract_u32(request+24))<<32;
            unsigned cmd=control&255;
            assert(cmd==4 || cmd==6 || cmd==0x10 || cmd==0x12);
            if(cmd==4) { ++acks; if(control & (uint64_t(1)<<16)) {++connectorAcks; cciConnector=0;} }
            else if(cmd==0x12) ++stage;
            uint8_t reply[16]={};
            piano_contract_put32(reply,PIANO_CONTRACT_OWNER); piano_contract_put32(reply+4,1); piano_contract_put32(reply+8,op);
            if(transportError) piano_contract_put32(reply+12,9);
            return piano_contract_receive(&c,now,reply,sizeof(reply));
        }
        assert(op==PIANO_CONTRACT_READ && n==12); ++reads;
        uint8_t response[544]={}; unsigned size=version<0x200 ? 64:544;
        piano_contract_put32(response,PIANO_CONTRACT_OWNER); piano_contract_put32(response+4,1); piano_contract_put32(response+8,op);
        response[12]=uint8_t(version); response[13]=uint8_t(version>>8);
        piano_contract_put32(response+20,uint32_t(control)+(wrongEcho?1u:0u)); piano_contract_put32(response+24,uint32_t(control>>32));
        uint8_t *msg=response+28;
        uint32_t cci=0;
        unsigned cmd=control&255;
        if(c.stage!=PianoContractProbe) {
            if(busy) cci=1u<<28;
            else if(cmd==4) cci=1u<<29;
            else {
                cci=1u<<31;
                if(cmd==6) { cci|=16u<<8; msg[0]=4; msg[4]=uint8_t(connectorCount); msg[5]=missingPdoFeature?0:16; }
                else if(cmd==0x12) {
                    cci|=(shortStatus?8u:9u)<<8;
                    piano_contract_put32(msg,status);
                    piano_contract_put32(msg+4,changeOnVerify&&c.stage==PianoContractVerify ? fixedRdo(1,500) : rdo);
                } else if(cmd==0x10) {
                    unsigned offset=(control>>24)&255;
                    unsigned want=unsigned((control>>32)&3)+1;
                    unsigned available=count>offset ? count-offset:0;
                    if(available>want) available=want;
                    cci|=available*4<<8;
                    for(unsigned i=0;i<available;++i) piano_contract_put32(msg+4*i,pdos[offset+i]);
                }
            }
        }
        if(ucsiError && c.stage!=PianoContractProbe) cci|=1u<<30;
        if(shortPayload && c.stage!=PianoContractProbe) cci=(cci&~0xff00u)|(17u<<8);
        cci|=cciConnector<<1;
        piano_contract_put32(response+16,cci);
        auto result=piano_contract_receive(&c,now,response,size);
        if(result==2) ++snapshots;
        return result;
    }
    void complete() {
        unsigned max=100;
        while(max-- && !snapshots && c.last_error!=PIANO_CONTRACT_TIMEOUT) {
            auto result=step(); if(result<0) return;
        }
        assert(max>0);
    }
};
static void protocol() {
    Ppm p; p.complete();
    assert(p.snapshots==1 && p.c.valid && p.c.status_valid && p.c.connected && p.c.sink && p.c.pd);
    assert(p.c.protocol==PIANO_CONTRACT_FIXED && p.c.mv==9000 && p.c.ma==1600 && p.c.pdos==5);
    assert(p.reads==11 && p.writes==10 && p.acks==5); // includes both PDO pages and status recheck
    assert(piano_contract_fresh(&p.c,p.now+14999));
    assert(!piano_contract_fresh(&p.c,p.now+15001));
    uint8_t request[544]; assert(!piano_contract_next(&p.c,p.now,request));
    auto gen=p.c.generation; auto seq=p.c.seq;
    piano_contract_refresh(&p.c); p.snapshots=0; p.complete();
    assert(p.c.generation==gen && p.c.seq==seq+1 && p.c.valid);
    p.rdo=ppsRdo(3,9020,1550); piano_contract_refresh(&p.c); p.snapshots=0; p.complete();
    assert(p.c.generation==gen+1 && p.c.seq==seq+2 && p.c.protocol==PIANO_CONTRACT_PPS && p.c.mv==9020 && p.c.ma==1550);
    gen=p.c.generation;
    p.status=1u<<19 | 1u<<16; p.rdo=0; piano_contract_refresh(&p.c); p.snapshots=0; p.complete();
    assert(p.c.status_valid && !p.c.valid && !p.c.pd && p.c.protocol==0 && p.c.mv==0 && p.c.pdos==0 && p.c.generation==gen+1);
    p.status=0; piano_contract_refresh(&p.c); p.snapshots=0; p.complete();
    assert(p.c.status_valid && !p.c.connected && !p.c.valid);
    Ppm v2; v2.version=0x200; v2.complete(); assert(v2.c.valid && v2.c.version==0x200);
    Ppm v21; v21.version=0x210; v21.complete(); assert(v21.c.valid);
}
static void invalidation() {
    Ppm p; p.complete(); auto gen=p.c.generation;
    uint8_t notify[24]={}; piano_contract_put32(notify,PIANO_CONTRACT_OWNER); piano_contract_put32(notify+4,2); piano_contract_put32(notify+8,PIANO_CONTRACT_NOTIFY);
    assert(piano_contract_receive(&p.c,p.now,notify,24)==1);
    auto previousSeq=p.c.seq; auto previousUpdated=p.c.updated_ms; auto previousWrites=p.writes;
    assert(p.c.valid && p.c.status_valid && p.c.notify_pending && p.c.generation==gen && p.c.pdos && p.c.ma);
    assert(!piano_contract_fresh(&p.c,p.now));
    assert(p.step()==2); // delayed final ACK_COMPLETE notification: CCI only, no new GET/ACK
    assert(p.c.valid && !p.c.notify_pending && p.c.stage==PianoContractIdle);
    assert(p.c.generation==gen && p.c.seq==previousSeq && p.c.updated_ms==previousUpdated && p.writes==previousWrites);
    assert(piano_contract_fresh(&p.c,p.now));
    assert(piano_contract_receive(&p.c,p.now,notify,24)==1);
    p.cciConnector=1;
    assert(p.step()==2);
    assert(!p.c.valid && !p.c.status_valid && !p.c.notify_pending && p.c.generation==gen+1 && !p.c.pdos && !p.c.ma);
    assert(p.c.stage==PianoContractStatus);
    p.snapshots=0; p.complete(); assert(p.c.valid && p.connectorAcks==1);
    piano_contract_link(&p.c,0); assert(!p.c.valid && !p.c.status_valid && !p.c.pdos);
    uint8_t request[544]; assert(!piano_contract_next(&p.c,p.now,request));
    Ppm malformed; malformed.shortStatus=true; malformed.complete();
    assert(!malformed.c.valid && !malformed.c.status_valid && malformed.c.last_error==PIANO_CONTRACT_PAYLOAD);
    Ppm changed; changed.changeOnVerify=true; changed.complete();
    assert(!changed.c.valid && !changed.c.status_valid && changed.c.last_error==PIANO_CONTRACT_CHANGED);
    Ppm shortPdo; shortPdo.pdos[4]=0; shortPdo.complete();
    assert(!shortPdo.c.valid && shortPdo.c.last_error==PIANO_CONTRACT_PAYLOAD);
    Ppm badObject; badObject.rdo=fixedRdo(6,500); badObject.complete();
    assert(!badObject.c.valid && badObject.c.status_valid && badObject.c.last_error==PIANO_CONTRACT_RDO);
    Ppm source; source.status|=1u<<20; source.complete();
    assert(source.c.status_valid && !source.c.valid && !source.c.sink);
    Ppm timeout; assert(piano_contract_next(&timeout.c,10,request)==12);
    assert(!piano_contract_next(&timeout.c,1510,request));
    assert(timeout.c.quarantined && timeout.c.last_error==PIANO_CONTRACT_TIMEOUT);
    auto seq=timeout.c.seq;
    uint8_t late[64]={}; piano_contract_put32(late,PIANO_CONTRACT_OWNER); piano_contract_put32(late+4,1); piano_contract_put32(late+8,PIANO_CONTRACT_READ); late[12]=0x10; late[13]=1;
    assert(piano_contract_receive(&timeout.c,1511,late,64)==0 && timeout.c.seq==seq);
    piano_contract_refresh(&timeout.c); assert(!piano_contract_next(&timeout.c,1600,request));
    piano_contract_link(&timeout.c,1); timeout.now=2000; timeout.complete(); assert(timeout.c.valid);
    Ppm truncated; assert(piano_contract_next(&truncated.c,10,request)==12);
    assert(piano_contract_receive(&truncated.c,11,late,63)==-1);
    assert(!truncated.c.valid && truncated.c.quarantined);
    Ppm wrongVersion; assert(piano_contract_next(&wrongVersion.c,10,request)==12); late[12]=0;late[13]=3;
    assert(piano_contract_receive(&wrongVersion.c,11,late,64)==-1 && wrongVersion.c.last_error==PIANO_CONTRACT_VERSION);
    Ppm reject; reject.step(); reject.transportError=true; assert(reject.step()==-1);
    assert(reject.c.last_error==PIANO_CONTRACT_RETURN && !reject.c.valid);
    Ppm ppmError; ppmError.step(); ppmError.step(); ppmError.ucsiError=true; assert(ppmError.step()==-1);
    assert(ppmError.c.last_error==PIANO_CONTRACT_CCI);
    Ppm echo; echo.step(); echo.step(); echo.wrongEcho=true; assert(echo.step()==-1);
    assert(echo.c.last_error==PIANO_CONTRACT_CCI && echo.c.quarantined);
    Ppm payload; payload.step(); payload.step(); payload.shortPayload=true; assert(payload.step()==-1);
    assert(payload.c.last_error==PIANO_CONTRACT_PAYLOAD);
    Ppm feature; feature.missingPdoFeature=true; feature.complete();
    assert(feature.c.last_error==PIANO_CONTRACT_CAPABILITY && !feature.c.status_valid);
    Ppm count; count.connectorCount=2; count.complete();
    assert(count.c.last_error==PIANO_CONTRACT_CAPABILITY && !count.c.valid);
    Ppm connector; connector.cciConnector=2; assert(connector.step()==-1);
    assert(connector.c.last_error==PIANO_CONTRACT_CCI && !connector.c.valid && !connector.acks);
    Ppm connectorChange; connectorChange.step(); connectorChange.step(); connectorChange.cciConnector=2;
    assert(connectorChange.step()==-1);
    assert(connectorChange.c.last_error==PIANO_CONTRACT_CCI && !connectorChange.c.valid && !connectorChange.acks);
    Ppm notifyCommand; notifyCommand.step(); notifyCommand.step();
    auto initialGeneration=notifyCommand.c.generation;
    assert(piano_contract_receive(&notifyCommand.c,notifyCommand.now,notify,24)==1);
    assert(notifyCommand.c.generation==initialGeneration); // command completion notify alone is not a detach
    auto notifyTargetSeq=notifyCommand.c.seq+1;
    for(unsigned i=0; i<100 && notifyCommand.c.seq<notifyTargetSeq; ++i) assert(notifyCommand.step()>=0);
    assert(notifyCommand.c.valid && notifyCommand.c.generation==initialGeneration && notifyCommand.c.seq==notifyTargetSeq);
    Ppm stale; stale.complete(); auto previousGeneration=stale.c.generation;
    piano_contract_refresh(&stale.c); stale.snapshots=0; stale.shortStatus=true; stale.complete();
    assert(!stale.c.valid && !stale.c.status_valid && !stale.c.pdos && !stale.c.ma && stale.c.generation==previousGeneration);
    Ppm sourceChange; sourceChange.complete(); previousGeneration=sourceChange.c.generation;
    sourceChange.pdos[1]=fixed(9000,1800); piano_contract_refresh(&sourceChange.c); sourceChange.snapshots=0; sourceChange.complete();
    assert(sourceChange.c.valid && sourceChange.c.source_ma==1800 && sourceChange.c.generation==previousGeneration+1);
    Ppm active; active.complete();
    auto activeSeq=active.c.seq, activeGen=active.c.generation; auto activeUpdated=active.c.updated_ms;
    piano_contract_refresh(&active.c); assert(active.step()==1); // periodic status GET written, no CCI yet
    auto activeCommand=active.c.command; auto activeWrites=active.writes;
    assert(active.c.stage==PianoContractStatus && active.c.wire==2);
    assert(piano_contract_receive(&active.c,active.now,notify,24)==1);
    auto activeNotifyMs=active.c.notify_ms;
    assert(active.c.notify_pending && active.c.stage==PianoContractStatus && active.c.command==activeCommand);
    assert(active.c.valid && !piano_contract_fresh(&active.c,active.now));
    assert(active.c.seq==activeSeq && active.c.generation==activeGen && active.c.updated_ms==activeUpdated);
    assert(active.step()==2); // next non-BUSY CCI resolves notification mid-GET
    assert(!active.c.notify_pending && active.c.stage==PianoContractAck && active.c.valid);
    assert(active.writes==activeWrites && active.c.seq==activeSeq && active.c.updated_ms==activeUpdated);
    assert(piano_contract_fresh(&active.c,active.now));
    for(unsigned i=0; i<100 && active.c.seq==activeSeq; ++i) assert(active.step()>=0);
    assert(active.c.seq==activeSeq+1 && active.c.generation==activeGen);
    Ppm activeBusy; activeBusy.complete(); piano_contract_refresh(&activeBusy.c); assert(activeBusy.step()==1);
    assert(piano_contract_receive(&activeBusy.c,activeBusy.now,notify,24)==1);
    activeNotifyMs=activeBusy.c.notify_ms; activeCommand=activeBusy.c.command; activeUpdated=activeBusy.c.updated_ms;
    activeBusy.busy=true; activeBusy.cciConnector=127; activeBusy.shortPayload=true; activeBusy.ucsiError=true;
    assert(activeBusy.step()==1 && activeBusy.c.notify_pending && activeBusy.c.command==activeCommand);
    activeBusy.now+=200;
    assert(piano_contract_receive(&activeBusy.c,activeBusy.now,notify,24)==1);
    assert(activeBusy.c.notify_ms==activeNotifyMs && activeBusy.c.updated_ms==activeUpdated);
    assert(activeBusy.step()==1 && activeBusy.c.notify_pending);
    assert(!piano_contract_next(&activeBusy.c,activeNotifyMs+5000,request));
    assert(activeBusy.c.quarantined && !activeBusy.c.valid && activeBusy.c.last_error==PIANO_CONTRACT_TIMEOUT);
    Ppm activeConnector; activeConnector.complete(); activeGen=activeConnector.c.generation;
    piano_contract_refresh(&activeConnector.c); assert(activeConnector.step()==1);
    assert(piano_contract_receive(&activeConnector.c,activeConnector.now,notify,24)==1);
    activeConnector.cciConnector=1; assert(activeConnector.step()==2);
    assert(!activeConnector.c.valid && !activeConnector.c.notify_pending && activeConnector.c.generation==activeGen+1);
    Ppm notifyError; notifyError.complete();
    assert(piano_contract_receive(&notifyError.c,notifyError.now,notify,24)==1);
    notifyError.ucsiError=true; assert(notifyError.step()==-1);
    assert(!notifyError.c.valid && !notifyError.c.status_valid && !notifyError.c.notify_pending && notifyError.c.last_error==PIANO_CONTRACT_CCI);
    Ppm notifyTimeout; notifyTimeout.complete(); previousGeneration=notifyTimeout.c.generation;
    assert(piano_contract_receive(&notifyTimeout.c,notifyTimeout.now,notify,24)==1);
    auto firstNotify=notifyTimeout.c.notify_ms;
    notifyTimeout.now+=4000;
    assert(piano_contract_receive(&notifyTimeout.c,notifyTimeout.now,notify,24)==1);
    assert(notifyTimeout.c.notify_ms==firstNotify); // storms cannot extend unknown-state authorization suspension
    assert(!piano_contract_next(&notifyTimeout.c,firstNotify+5000,request));
    assert(!notifyTimeout.c.valid && !notifyTimeout.c.status_valid && notifyTimeout.c.quarantined &&
           !notifyTimeout.c.notify_pending && notifyTimeout.c.last_error==PIANO_CONTRACT_TIMEOUT && notifyTimeout.c.generation==previousGeneration);
    Ppm notifyBusy; notifyBusy.complete(); previousGeneration=notifyBusy.c.generation;
    assert(piano_contract_receive(&notifyBusy.c,notifyBusy.now,notify,24)==1);
    notifyBusy.busy=true; notifyBusy.cciConnector=127; notifyBusy.shortPayload=true; notifyBusy.ucsiError=true;
    assert(notifyBusy.step()==1 && notifyBusy.c.notify_pending && notifyBusy.c.generation==previousGeneration);
    assert(!piano_contract_fresh(&notifyBusy.c,notifyBusy.now)); // BUSY makes all remaining CCI fields non-authoritative
    assert(!piano_contract_next(&notifyBusy.c,notifyBusy.c.notify_ms+5000,request));
    assert(notifyBusy.c.last_error==PIANO_CONTRACT_TIMEOUT && !notifyBusy.c.valid);
    Ppm busy; busy.step(); busy.step(); busy.step(); busy.busy=true; busy.step();
    assert(busy.c.wire==2);
    assert(!piano_contract_next(&busy.c,busy.now+6000,request));
    assert(busy.c.quarantined && busy.c.last_error==PIANO_CONTRACT_TIMEOUT);
}
int main() { decoding(); protocol(); invalidation(); std::puts("charge contract: fixed/PPS units, UCSI framing/ACK, source/status coherence, generations, invalidation and late ACK tests PASS"); }
