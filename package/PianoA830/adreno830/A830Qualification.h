/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * R3a is an operator qualification, not a daily frequency policy. One heartbeat owns the sensor state;
 * requests, the ceiling and the fault latch cross threads atomically. Never waits for the power/HFI mutex.
 */
#ifndef A830_QUALIFICATION_H
#define A830_QUALIFICATION_H
#include "A830Recovery.h"
#include "Sm8750Tsens.h"
#include "../../include/PianoBoardTemps.h"

enum A830QualReason : uint32_t {A830QualNone,A830QualVote,A830QualGpuHot,A830QualBoardHot,A830QualSensor,A830QualExpired};
// Current pre-arm blockers, distinct from the first armed-run fault (reason). Diagnostic only.
enum A830QualInhibit : uint32_t {A830QualGpuInvalid=1,A830QualBoardInvalid=2,A830QualGpuOver=4,
    A830QualBoardOver=8,A830QualNeedRounds=16};
class A830Qualification {
public:
    void init(A830PowerState *power,A830Recovery *recovery) {power_=power;recovery_=recovery;power_->setQualificationCeiling(3,3);}
    bool ready() const {return __atomic_load_n(&ready_,__ATOMIC_ACQUIRE);}
    uint32_t inhibit() const {return __atomic_load_n(&inhibit_,__ATOMIC_ACQUIRE);}
    bool high() const {return __atomic_load_n(&high_,__ATOMIC_ACQUIRE) && !power_->qualificationLatched();}
    bool arm() {
        if(!ready() || power_->qualificationLatched() || __atomic_exchange_n(&armed_,true,__ATOMIC_ACQ_REL))return false;
        return true;
    }
    uint32_t reason() const {return __atomic_load_n(&reason_,__ATOMIC_ACQUIRE);}
    uint64_t failedAt() const {return __atomic_load_n(&failedAt_,__ATOMIC_ACQUIRE);}
    uint64_t openedAt() const {return __atomic_load_n(&openedAt_,__ATOMIC_ACQUIRE);}
    void fault(uint32_t reason,uint64_t now) {
        uint32_t expected=0;
        (void)__atomic_compare_exchange_n(&reason_,&expected,reason,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        // Preserve the first boot reason, but never suppress a later hardware-unknown vote failure.
        const bool underway=recovery_->busy() || recovery_->pending();
        if(!underway || !failedAt())__atomic_store_n(&failedAt_,now,__ATOMIC_RELEASE);
        __atomic_store_n(&high_,false,__ATOMIC_RELEASE);
        recovery_->closeQualification();
        if(reason==A830QualSensor || reason==A830QualGpuHot)power_->setQualificationCeiling(1,2);
        if(underway)return;
        recovery_->report(A830FaultHfi,reason);
    }
    // Called every second by a separate kernel thread. 600 s soak + at most 2 s for the last, dropped ACK.
    // A missing callback/service, partial VALID mask, implausible reading or stalled sample is never a permit.
    void tick(uint64_t now,const Sm8750TsensSample &gpu,const piano_gpu_temps &board,bool boardOk) {
        __atomic_store_n(&heartbeatAt_,now,__ATOMIC_RELEASE);
        if(power_->qualificationLatched() && !reason())fault(A830QualVote,now);
        bool gpuOk=gpu.valid==255;
        for(unsigned i=0;i<8;++i)if(gpu.deci[i]<-400 || gpu.deci[i]>1250)gpuOk=false;
        const bool boardGood=boardOk && board.version==PIANO_GPU_TEMPS_VERSION && board.state==PIANO_BOARD_TEMPS_RUNNING &&
            board.valid==3 && !board.disabled && board.period_ms==5000 && board.age_ms[0]<=10000 && board.age_ms[1]<=10000 &&
            board.sample_us[0] && board.sample_us[1] && board.milli_c[0]>=-40000 && board.milli_c[1]>=-40000 &&
            board.milli_c[0]<=125000 && board.milli_c[1]<=125000;
        const bool gpuHot=gpu.valid && gpu.max_deci>=700;
        const bool boardHot=boardGood && (board.milli_c[0]>=65000 || board.milli_c[1]>=40000);
        if(boardGood && gpuOk && !gpuHot && !boardHot) {
            if(board.rounds!=lastRound_ && board.sample_us[0]>lastSamples_[0] && board.sample_us[1]>lastSamples_[1]) {
                if(rounds_<2)++rounds_;
                lastRound_=board.rounds;lastSamples_[0]=board.sample_us[0];lastSamples_[1]=board.sample_us[1];
            }
        } else rounds_=0;
        __atomic_store_n(&inhibit_,(!gpuOk?A830QualGpuInvalid:0u)|(!boardGood?A830QualBoardInvalid:0u)|
            (gpuHot?A830QualGpuOver:0u)|(boardHot?A830QualBoardOver:0u)|(rounds_<2?A830QualNeedRounds:0u),__ATOMIC_RELEASE);
        __atomic_store_n(&ready_,gpuOk && boardGood && !gpuHot && !boardHot && rounds_>=2,__ATOMIC_RELEASE);
        if(high()) {
            if(gpuHot)fault(A830QualGpuHot,now);
            else if(boardHot)fault(A830QualBoardHot,now);
            else if(!gpuOk || !boardGood)fault(A830QualSensor,now);
            else if(now-openedAt()>=602000000000ull)fault(A830QualExpired,now);
        }
        if(!openedAt() && __atomic_load_n(&armed_,__ATOMIC_ACQUIRE) && ready() && !power_->qualificationLatched()) {
            __atomic_store_n(&openedAt_,now,__ATOMIC_RELEASE);
            __atomic_store_n(&high_,true,__ATOMIC_RELEASE);
        }
        const uint64_t end=openedAt()+602000000000ull;
        power_->setQualificationLease(now+2500000000ull<end?now+2500000000ull:end);
        // After the boot latch, cool readings may still reach 342; unknown/hot must always be able to reach 160.
        power_->setQualificationCeiling(!gpuOk || gpuHot?1:high()?7:3,!gpuOk || gpuHot?2:high()?6:3);
    }
    void checkLease(uint64_t now) {
        const uint64_t at=__atomic_load_n(&heartbeatAt_,__ATOMIC_ACQUIRE);
        if(__atomic_load_n(&high_,__ATOMIC_ACQUIRE) && now>=at && now-at>=2500000000ull)
            fault(A830QualSensor,now);
        else if(power_->qualificationLatched() && !reason())fault(A830QualVote,now);
    }
    void sample(uint64_t now,const Sm8750TsensSample &gpu) {
        if(!openedAt())return;
        if(gpu.valid && gpu.max_deci>=700 && !power_->qualificationLatched())fault(A830QualGpuHot,now);
        else if(gpu.valid!=255 && !power_->qualificationLatched())fault(A830QualSensor,now);
    }
    bool deadlineMissed(uint64_t now) const {
        const uint64_t at=failedAt();
        return at && now>=at && now-at>=5000000000ull && !recovery_->safeOff() && !recovery_->qualificationSafeLow();
    }
private:
    A830PowerState *power_=nullptr;
    A830Recovery *recovery_=nullptr;
    bool ready_=false,high_=false,armed_=false;
    uint32_t reason_=0,rounds_=0,lastRound_=0,inhibit_=A830QualNeedRounds;
    uint64_t openedAt_=0,failedAt_=0,lastSamples_[2]={};
    uint64_t heartbeatAt_=0;
};
#endif
