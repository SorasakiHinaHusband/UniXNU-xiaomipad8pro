/* SPDX-License-Identifier: MIT */
/* host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c */
#define A830_R3B_ENTRY runOriginalR3b
#include "test_a830r3b.cpp"
#undef A830_R3B_ENTRY

int main()
{
    for(bool armed:{false,true}) {
        bootB();
        if(armed) {openB();driveB(90);}
        else power.noteVoted(3,3);
        auto b=board(24);b.milli_c[0]=63677;b.milli_c[1]=33532;
        tickB(784,100000000,b);
        b=board(25);b.milli_c[0]=65179;b.milli_c[1]=33549;b.age_ms[0]=b.age_ms[1]=37;
        tickB(780,100000000,b);
        const auto at=qual.failedAt();
        CHECK(qual.reason()==A830QualBoardHot && at==gEnv.now && recovery.pending() && !qual.high());
        CHECK(power.qualificationLatched() && power.qualificationBlocked() && power.qualificationEffectiveCap()==1);
        gOps.lastSubmitted=2;gOps.retired=1;
        gOps.during=[](const std::string &call) {
            if(call=="confirmLow") {CHECK(power.acquire()!=0 && power.inFlight());++gOps.retired;}
        };
        CHECK(recovery.runPending()==0 && recovery.qualificationSafeLow());
        CHECK(gOps.count("confirmLow")==1 && gOps.gates.empty() && !gOps.count("gmuForceOff") && !gOps.count("snapshot"));
        CHECK(unsafe==0 && !power.inFlight() && power.acquire()==0);
        power.release();power.setTarget(11,10);power.setWakeTargets(11,10,11,10);
        CHECK(power.votedLevel()==1 && power.votedBus()==2 && power.targetLevel()==1 && !qual.arm());
        tickB(740,100000000,board(26));power.setTarget(11,10);
        CHECK(power.targetLevel()==3 && qual.failedAt()==at && !qual.deadlineMissed(at+5000000000ull));
    }

    // Test-only reason=3 at cool readings, then a second transaction on the same already-latched boot.
    bootB();power.noteVoted(3,3);
    qual.tick(gEnv.now,gpu(),board(),true,true);
    CHECK(qual.reason()==A830QualBoardHot && recovery.runPending()==0);
    const auto at=qual.failedAt();
    gEnv.now+=60000000000ull;
    recovery.requestQualificationLow(A830QualBoardHot,gEnv.now,true);
    CHECK(!qual.deadlineMissed(gEnv.now) && qual.failedAt()==at);
    gOps.fail["dropLow"]=kIOReturnTimeout;
    CHECK(recovery.runPending()==0 && recovery.qualificationSafeLow());
    CHECK(gOps.index("dropLow")<gOps.index("gate:1") && gOps.index("strictQuiesce")<gOps.index("gmuForceOff"));
    CHECK(gOps.index("gmuForceOff")<gOps.index("gmuStart") && gOps.index("secureResume")<gOps.index("gate:0"));
    CHECK(gOps.count("snapshot")==0 && gOps.lastArgs.level==1 && gOps.lastArgs.bus==2 && !unsafe);
    CHECK(power.qualificationLatched() && power.votedLevel()==1 && !power.qualificationBlocked());

    // Concurrent genuine fault takes precedence, including one published from the HFI exchange.
    bootB();qual.fault(A830QualBoardHot,gEnv.now);
    gOps.during=[](const std::string &call) {if(call=="confirmLow")recovery.report(A830FaultGmu,0x12345678);};
    CHECK(recovery.runPending()==0 && recovery.snapshot().cause==A830FaultGmu && recovery.snapshot().detail==0x12345678);
    CHECK(gOps.count("gmuForceOff")==1 && gOps.count("snapshot")==0 && !unsafe);

    for(const char *fail:{"strictQuiesce","gmuForceOff","gmuStart","cpStart"}) {
        bootB();qual.fault(A830QualBoardHot,gEnv.now);
        gOps.fail["confirmLow"]=kIOReturnTimeout;gOps.fail[fail]=kIOReturnIOError;
        CHECK(recovery.runPending()!=0 && recovery.lost() && power.qualificationLatched());
        CHECK(gOps.count("gate:0")==0);
        if(std::string(fail)=="strictQuiesce")CHECK(unsafe==1 && gOps.count("gmuForceOff")==0);
    }
    bootB();CHECK(power.acquire()==0);qual.fault(A830QualBoardHot,gEnv.now);
    gOps.fail["confirmLow"]=kIOReturnTimeout;
    CHECK(recovery.runPending()!=0 && unsafe==1 && !gOps.count("strictQuiesce") && !gOps.count("gmuForceOff"));
    power.release();

    for(const auto mode:{A830SlumberFull,A830SlumberCxKeep}) {
        bootB();power.setTarget(3,3);power.setWakeTargets(3,3,3,3);
        CHECK(power.enterSlumber(mode)==0);qual.fault(A830QualBoardHot,gEnv.now);
        CHECK(recovery.runPending()==0 && !gOps.count("confirmLow") && !gOps.count("gmuStart"));
        CHECK(power.qualificationEffectiveCap()==1 && power.wake(A830WakeTouch)==0);
        CHECK(gOps.lastArgs.level==1 && gOps.lastArgs.bus==2 && power.qualificationLatched());
    }
    std::printf("PASS: recorded and injected board-hard exits, fresh-low routing, strict failure gates, fault race and deferred wake (%u checks)\n",gChecks);
}
