#pragma once

#include <vector>
#include <cstdio>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

#include "pidPreDump.hpp"
//will add more stuff if needed here
struct timerEntry {
    int timestamp;
    pidPreDump* preDump;
};


//also i am not rebalancing this thing, i wont cram 100k criu migratable processes in a single node anytime soon and if i do timer insert latency will not be the scary latency source anyway
//basically the only reason i even did this is to be capable of supporting inerting multilength timers since i need to change the top one and interrupt sleeps, if they were all the same length i could have gotten away with a queue
//cancellation wise there ought to just be a set of dead timers that i set when i want to prevent callbacks to dead objects, also this thing will require many locks one way or another

// Theoretically its kinda good because its a consumer-producer, popping takes no locks and is carried out in-line. Yeah this can delay callback execution but
//i dont care that much over it, otherwise i gotta bother with locking the removeTop() too


/*
timerEntry* getRight(int pos){
    int rightPos = 2 * pos + 2;
    if(rightPos < timerHeap.size()){
        return &timerHeap[rightPos];
    }
    return nullptr;
}

timerEntry* getLeft(int pos){
    int leftPos = 2 * pos + 1;
    if(leftPos < timerHeap.size()){
        return &timerHeap[leftPos];
    }
    return nullptr;
}

timerEntry* getParent(int pos){
    if(pos == 0) return nullptr;
    int parentPos = (pos - 1) / 2;
    return &timerHeap[parentPos];
}

//takes a given position and moves it up with its parrent, does it unconditionally
int bubbleUp(int pos){
    timerEntry* parent = getParent(pos);
    if(parent == nullptr){
        printf("No parent for position %d\n", pos);
        return -1;
    }
    std::swap(timerHeap[pos], *parent);
    return (pos - 1) / 2;
}


void insertTimer(timerEntry entry){
    timerHeap.push_back(entry);

    timerEntry* parent = getParent(timerHeap.size() -1);
    if(parent == nullptr){
        return;
    }
    int currentPos = timerHeap.size() - 1;

    while(entry.timestamp < parent->timestamp){
        int retCode = bubbleUp(currentPos);
        if(retCode == -1) {
            break;
        }


        currentPos = retCode;
        parent = getParent(currentPos);
        if(parent == nullptr){
            break;
        }
    }

    heapStateEmpty = false;

}



timerEntry removeTop(){
    if(timerHeap.empty()){
        printf("Heap is empty, cannot remove top\n");
        return timerEntry{-1};
    }

    timerEntry top = timerHeap[0];
    timerHeap[0] = timerHeap.back();
    timerHeap.pop_back();

    int currentPos = 0;
    while(true){
        timerEntry* left = getLeft(currentPos);
        timerEntry* right = getRight(currentPos);
        int smallestPos = currentPos;

        if(left != nullptr && left->timestamp < timerHeap[smallestPos].timestamp){
            smallestPos = 2 * currentPos + 1;
        }
        if(right != nullptr && right->timestamp < timerHeap[smallestPos].timestamp){
            smallestPos = 2 * currentPos + 2;
        }

        if(smallestPos == currentPos){

            break;
        }

        std::swap(timerHeap[currentPos], timerHeap[smallestPos]);
        currentPos = smallestPos;
    }

    if(timerHeap.empty()){
        heapStateEmpty = true;
    }
    return top;
}






*/







//i aint fiddling with coroutines or boost imports to make this a green thread, its whatever


class waiterHeap{
    std::mutex workerRevive;
    std::mutex heapMutex;

    std::stop_token stopTkn;

    bool heapStateEmpty;
    bool activeWriterFlag = false;
    std::vector<timerEntry> timerHeap;
    std::jthread worker;

    timerEntry* getRight(int pos){
        int rightPos = 2 * pos + 2;
        if(rightPos < this->timerHeap.size()){
            return &timerHeap[rightPos];
        }
        return nullptr;
    }

    timerEntry* getLeft(int pos){
        int leftPos = 2 * pos + 1;
        if(leftPos < this->timerHeap.size()){
            return &timerHeap[leftPos];
        }
        return nullptr;
    }

    timerEntry* getParent(int pos){
        if(pos == 0) return nullptr;
        int parentPos = (pos - 1) / 2;
        return &this->timerHeap[parentPos];
    }

    int bubbleUp(int pos){
        timerEntry* parent = getParent(pos);
        if(parent == nullptr){
            printf("No parent for position %d\n", pos);
            return -1;
        }
        std::swap(this->timerHeap[pos], *parent);
        return (pos - 1) / 2;
    }
    
    

    void threadWorker(std::stop_token stopTkn){
        std::mutex cvm;
        std::condition_variable_any cv;

        std::unique_lock<std::mutex> armLock(cvm);

        while(true){
            while(!timerHeap.empty()){
                
                //its ultra janky
                heapMutex.lock();
                timerEntry topElem = timerHeap[0];

                auto wakeTime = std::chrono::steady_clock::time_point(std::chrono::seconds(topElem.timestamp));
                cv.wait_until(armLock, stopTkn, wakeTime, [&]{
                    std::lock_guard<std::mutex> lock(heapMutex);
                    //this is the normal case of a timer expiring uninterrupted
                    try{
                    if(!stopTkn.stop_requested()){
                        auto ct = std::chrono::steady_clock::now();
                        auto crrTime = std::chrono::duration_cast<std::chrono::seconds>(ct.time_since_epoch()).count();

                        timerEntry old = removeTop();
                        //to dump out any duplicates if they exist or timers that had their time pass while execution was running
                        while(!timerHeap.empty() && (old.timestamp == timerHeap.at(0).timestamp || crrTime > old.timestamp)){
                            timerEntry *crrTop = &timerHeap[0];
                            crrTop->preDump->doBackup();

                            old = removeTop();

                        }

                    }else{

                        if(activeWriterFlag){
                            //its ultra jankk but a writer has to request_stop to unlock the thing and let it carry out its own locking phase
                            heapMutex.unlock();
                        }
                        
                        //this is the normal update path to swap out the top timer for a smaller one if present, basically just re-run get top after writer has completed the insertion perculation
                        //if it just returns the same thing, no big deal
                    }

                    heapMutex.unlock();

                }catch(const std::out_of_range& outOfRng){
                    printf("Out of range exception caught in timerheapWorker thread %s\n", outOfRng.what());
                }catch(const std::exception& excp){
                    printf("Unanticipated exception caught in timerheapWorker thread %s\n", excp.what());
                }

                });
        
            }

            //have to add the maintenance regular routines here on some fixed time period
        }
    }


    //since this thing is always called in the worker thread i dont need to lock it at all, it is guaranteed to not be doing anything else
    //never call it outside of the worker
    timerEntry removeTop(){
        if(timerHeap.empty()){
            printf("Heap is empty, cannot remove top\n");
            throw std::out_of_range("Heap is empty, cannot remove top");
        }

        timerEntry top = timerHeap[0];
        timerHeap[0] = timerHeap.back();
        timerHeap.pop_back();

        int currentPos = 0;
        while(true){
            timerEntry* left = getLeft(currentPos);
            timerEntry* right = getRight(currentPos);
            int smallestPos = currentPos;

            if(left != nullptr && left->timestamp < this->timerHeap[smallestPos].timestamp){
                smallestPos = 2 * currentPos + 1;
            }
            
            if(right != nullptr && right->timestamp < this->timerHeap[smallestPos].timestamp){
                smallestPos = 2 * currentPos + 2;
            }

            if(smallestPos == currentPos){
                break;
            }

            std::swap(this->timerHeap[currentPos], this->timerHeap[smallestPos]);
            currentPos = smallestPos;
        }

            if(timerHeap.empty()){
                this->heapStateEmpty = true;
            }
            return top;
        }


    public:
    waiterHeap(){
        this->heapStateEmpty = true;
        this->worker = std::jthread(&waiterHeap::threadWorker, this);
        this->stopTkn = worker.get_stop_token();
    } 


    

    void insertTimer(timerEntry entry){

        //this is ultra janky but it was somehow the cleanest solution, the call to stop is made here to ensure that the worker thread releases its own lock on the heap mutex
        //it is very ugly but its the only way to avoid consitiency problems or a rewirite
        this->activeWriterFlag = true;
        this->worker.request_stop();
        this->activeWriterFlag = false;

        //now this is the lock for the mutex
        std::lock_guard<std::mutex> lock(heapMutex);
        this->timerHeap.push_back(entry);

        timerEntry* parent = getParent(this->timerHeap.size() -1);
        if(parent == nullptr){
            return;
        }
        int currentPos = this->timerHeap.size() - 1;

        while(entry.timestamp < parent->timestamp){
            int retCode = bubbleUp(currentPos);
            if(retCode == -1) {
                break;
            }


            currentPos = retCode;
            parent = getParent(currentPos);
            if(parent == nullptr){
                break;
            }
        }

        heapStateEmpty = false;

        //this just makes sure that the waiter ran to set the timer to a new earliest tirgger time had it been the case
        this->worker.request_stop();
    }


    

};