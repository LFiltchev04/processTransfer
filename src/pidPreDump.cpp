#include "pidPreDump.hpp"

pidPreDump::pidPreDump(int pid, dumpFile location) {
    pidToTrack = pid;
    predumpLocation = location;
}

void pidPreDump::watchPid() {
    auto now = std::chrono::steady_clock::now();
    int tStamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    timerEntry entry{
        tStamp,
        this
    };

    timerHeap.insertTimer(entry);
}

void pidPreDump::doBackup(){
    std::string fullPath = criuCmd->getDumpsDirBase() + "/" + predumpLocation.getPath();
    criuCmd->flushDirtyPages(criuOptions, fullPath);
}


void pidPreDump::finalBackup(){
    std::string fullPath = criuCmd->getDumpsDirBase() + "/" + predumpLocation.getPath();
    
}


void doNothingPredump::timerheapCallback() {
    
}