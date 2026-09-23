#include <unordered_map>
#include <cstdint>
#include <mutex>

//dentry scoped competion tracker for io_uring resubmissions in case of short reads
//can support multithreaded access, no thread pinning needed 
struct pHolder{
    int targetWrite;
    void* resubmittableSqe;

    pHolder(){targetWrite = 0; resubmittableSqe = nullptr;}
    
    pHolder(int tgtWrt, void* sqer){
        targetWrite = tgtWrt;
        resubmittableSqe = sqer;
    }
};

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
        
        this->lastElem = lastElem + sizeof(pHolder);
            
    }

    ~completionTracker(){
        delete[] holders;
    }



};