#pragma once
#include <chrono>

#include "dumpFile.hpp"
#include "criuInterface.hpp"

class waiterHeap;


class pidPreDump{
    static waiterHeap timerHeap;
    static criuInterface *criuCmd;

    criu_opts *criuOptions;

    dumpFile predumpLocation;
    int pidToTrack;

    protected:
    virtual void timerheapCallback();

    public:
    pidPreDump(int pid, dumpFile location);

    virtual void watchPid();
    virtual void doBackup();
    virtual void finalBackup();
    virtual bool isReaper() { return false; }
};


//exists entirely as a reaper subroutine that wont throw exceptions like crazy in the sleeper-worker of of the timer heap
class doNothingPredump : public pidPreDump{
    protected:
    void timerheapCallback() override;

    public:
    doNothingPredump(int pid, dumpFile location) : pidPreDump(pid, location) {}

    void watchPid() override {}
    void doBackup() override {}
    void finalBackup() override {}
    bool isReaper() override { return true; }
};