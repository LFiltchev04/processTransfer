#include <unordered_map>
#include <cstdint>
#include <mutex>

//dentry scoped competion tracker for io_uring resubmissions in case of short reads
//can support multithreaded access, no thread pinning needed 
struct pHolder{
    int targetWrite;

    pHolder(){targetWrite = 0;}
    
    pHolder(int tgtWrt){
        targetWrite = tgtWrt;
    }
};

//tracks active completion using fifo 
class completionTracker{
    uint16_t numCompletions; //absolute number of completions to look for
    pHolder *holders;
    pHolder* lastElem;

    std::mutex mtx;

    public:
    completionTracker(uint16_t num){
        numCompletions = num;
        lastElem = holders;
        holders = new pHolder[numCompletions];
    }

    pHolder* selfRegister(uint16_t targetWrite){
        std::lock_guard<std::mutex> lck(mtx);
        
        lastElem->targetWrite = targetWrite;
        this->lastElem = lastElem + 1;
        return lastElem - 1;
    }

    uint16_t remainingBytes(){
        std::lock_guard<std::mutex> lck(mtx);

        
        return numCompletions; 

        


    }
    ~completionTracker(){
        delete[] holders;
    }



};

