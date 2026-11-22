/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * Bounded engineering qualification only. Board NTCs are not a calibrated skin bound.
 * One 100 ms guard owns this state; consumers receive only the atomic packed snapshot.
 */
#ifndef A830_R3B_THERMAL_H
#define A830_R3B_THERMAL_H
#include "Sm8750Tsens.h"
#include "../../include/PianoBoardTemps.h"
struct A830R3bThermal {
    static constexpr uint64_t Lease=250000000,Period=100000000;
    /* gpu/daily4: the ladder's top GMU level, the qualified ceiling (7 = 607 MHz; 8 = 660, 9 = 734 on the 660/734 step). */
    uint8_t top=7;
    uint8_t step=3,entryRounds=0,releaseRounds=0,minValid=255;
    bool boardCap=false,ready=false,healthy=false,halt=false;
    int32_t raw=0,filter16=0,trendRef=0,peaks[8]={};
    int8_t trend=0;
    uint32_t lastRound=0,reads=0,downs=0,ups=0,events=0;
    uint64_t sequence=0;
    uint64_t samples[2]={},last=0,freshAt=0,downAt=0,coolSince=0,trendAt=0,maxGap=0;
    bool filterValid=false;
    // Published once after each complete guard round: cap, raw, filtered, validity and board hold.
    uint64_t snapshot=0;
    uint64_t published() const {return __atomic_load_n(&snapshot,__ATOMIC_ACQUIRE);}
    void tick(uint64_t now,const Sm8750TsensSample &g,const piano_gpu_temps &b,bool ok,bool running) {
        events=0;halt=false;
        const uint64_t gap=last && now>=last?now-last:0;
        if(gap>maxGap)maxGap=gap;
        bool gpu=g.valid==255;raw=-400;
        for(unsigned i=0;i<8;++i) {
            if(g.deci[i]<-400 || g.deci[i]>1250)gpu=false;
            if(g.valid&(1u<<i)) {
                if(g.deci[i]>raw)raw=g.deci[i];
                if(g.deci[i]>=1050)halt=true; // independent of filter, board API and recovery/HFI
            }
            if(!reads || g.deci[i]>peaks[i])peaks[i]=g.deci[i];
        }
        minValid&=g.valid;++reads;++sequence;
        const bool board=ok && b.version==PIANO_GPU_TEMPS_VERSION && b.state==PIANO_BOARD_TEMPS_RUNNING &&
            b.valid==3 && !b.disabled && b.period_ms==5000 && b.age_ms[0]<=10000 && b.age_ms[1]<=10000 &&
            b.sample_us[0] && b.sample_us[1] && b.milli_c[0]>=-40000 && b.milli_c[0]<=125000 &&
            b.milli_c[1]>=-40000 && b.milli_c[1]<=125000;
        const bool fresh=board && b.rounds!=lastRound && b.sample_us[0]>samples[0] && b.sample_us[1]>samples[1];
        if(fresh) {lastRound=b.rounds;samples[0]=b.sample_us[0];samples[1]=b.sample_us[1];freshAt=now;}
        const bool boardLive=board && freshAt && now>=freshAt && now-freshAt<=10000000000ull;
        healthy=gpu && boardLive && (!last || (now>=last && gap<=Lease));
        const bool entry=healthy && raw<800 && b.milli_c[0]<62000 && b.milli_c[1]<37000;
        if(!entry)entryRounds=0;
        else if(fresh && entryRounds<2)++entryRounds;
        ready=entry && entryRounds>=2;
        if(boardLive && (b.milli_c[0]>=62000 || b.milli_c[1]>=37000)) {
            if(!boardCap)events|=4;
            boardCap=true;releaseRounds=0;
        }
        if(boardCap) {
            if(!healthy || b.milli_c[0]>60000 || b.milli_c[1]>35000)releaseRounds=0;
            else if(fresh && ++releaseRounds>=2) {boardCap=false;releaseRounds=0;events|=8;}
        }
        if(gpu && (!last || now-last>=Period)) {
            if(!filterValid) {filter16=raw*16;filterValid=true;trendRef=filter16;trendAt=now;}
            else filter16+=(raw*16-filter16)/4;
            if(now-trendAt>=500000000) {
                const int32_t delta=filter16-trendRef;
                trend=delta>32?1:delta<-32?-1:0;trendAt=now;trendRef=filter16;
            }
        }
        if(!running || !healthy || boardCap || raw>=800 || filter16>750*16)coolSince=0;
        else if(!coolSince)coolSince=now;
        else if(now-coolSince>=2000000000ull && step<top) {++step;++ups;events|=1;coolSince=now;}
        if(running && healthy && (raw>=800 || filter16>=800*16) && (!downAt || now-downAt>=500000000)) {
            if(step>3) {--step;++downs;events|=2;}
            downAt=now; // a late window never catches up by dropping several levels
        }
        const uint8_t cap=!healthy || raw>=850 || (boardLive && (b.milli_c[0]>=65000 || b.milli_c[1]>=40000))?1:boardCap?3:step;
        __atomic_store_n(&snapshot,uint64_t(cap)|uint64_t(uint16_t(raw))<<8|
            uint64_t(uint16_t(filter16/16))<<24|uint64_t(healthy)<<40|uint64_t(boardCap)<<41,__ATOMIC_RELEASE);
        last=now;
    }
    void resetPeaks() {reads=0;minValid=255;maxGap=0;}
};
#endif
