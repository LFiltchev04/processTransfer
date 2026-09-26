#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <dirent.h>
#include <unordered_map>
#include <string>
#include <stack>
#include <liburing.h>
#include <vector>
#include <unordered_set>

#include "dumpPresenceTable.hpp"
#include "uploadsStaticBuffer.hpp"
#include "dataPack.hpp"

#define THIRTYTWO_KB (32 * 1024)
#define SIXTYFOUR_KB (64 * 1024)




class pipePool{
    int fdPool[64][2];
    std::stack<int*> pool;

    public:
    pipePool(){
        for(int i = 0; i < 64; ++i){
            pipe(fdPool[i]);
            pool.push(fdPool[i]);
        }
    }



    int* getPipe(){
        if(pool.empty()){
            return nullptr;
        }
     
        int* pipeFds = pool.top();
        pool.pop();
        return pipeFds;
    }

    void returnPipe(int* pipeFds){
        if(pipeFds == nullptr or pool.size() >= 64){
            return;
        }

        if(pipeFds < fdPool[0] or pipeFds > fdPool[63]){
            return;
        }
        pool.push(pipeFds);
    }
};

pipePool pipeMgr;


class pushService {

    protected:
    static dumpPresenceTable* presenceTable;

    public:
    pushService(dumpPresenceTable* table){ presenceTable = table; };

    virtual void listenL() = 0;
    
};

//should have used that from the start
#define MAKE_NV(NAME, VALUE) \
    { (uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE }


struct nvRow {
    const char* name;
    const char* value;
};
//have to call delete right after?
nghttp2_nv* makeNvHelper(nvRow rows[]) {
    int arrSz = sizeof(rows) / sizeof(nvRow);
    nghttp2_nv* nvArr = new nghttp2_nv[arrSz];
    for(int i = 0; i < arrSz; ++i) {
        nvArr[i] = MAKE_NV(rows[i].name, rows[i].value);
    }

    return nvArr;
}


class http2PushService: public pushService {
    static int serverSocket;
    static int epfd;
    static epoll_event ev;




    //basic repeating headers i dont want to allocate often
    nvRow scheme{":scheme", "http"};
    nvRow authority{":authority", "127.0.0.1"};
    nvRow method{":method", "GET"};
    nvRow type{":type", "application/octet-stream"};

    nvRow returnCodeOk{":status", "200"};
    nvRow returnCodeNotFnd{":status", "404"};
    

    nghttp2_session* session;

    static io_uring ring;


    struct tcpCtx{
        int outgoingFd;
        nghttp2_session* session;
    }; 
    //this one is weird, its semi-shared, completion trackers have to be allocated once and then only incremented
    //pipes are scoped to a single sqe-cqe pair
    struct partialWritesCtx{
        std::mutex *mtx;
        unsigned int completionTracker;
        unsigned int writeTarget = 0u;
        unsigned int startOffset = 0u;
        int* pipes[2];
        std::string refkey;
        uint8_t* peristHeader = nullptr;
        packData* packHeader = nullptr;
        

        partialWritesCtx(){completionTracker = 0u; mtx = new std::mutex();}
        ~partialWritesCtx(){
            delete mtx;
            delete peristHeader;
            delete packHeader;

            pipeMgr.returnPipe(pipes[0]);
        }
    };
    struct basicCtx{
        DIR *openDir;
        dirent* activeDentry = nullptr;
        int outgoingFd;
        unsigned int dentryOffset = 0u;
        partialWritesCtx *wrtCtx = nullptr;
        nghttp2_data_provider src{};
    };

    static std::unordered_map<std::string, partialWritesCtx> partialWritesMap;

    int getRadomStream();
    //sets epoll to run on return, need to allocate memory out of scope or it will dangle
    static void allowNetworkFlush();
    static void stopNetworkFlush();

    static int onHeaderRecv(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len, uint8_t flags, void *user_data);
    //this would have been great as a coroutine but i dont want to mess with the boilerplate, its way easier to just pause uploads and resume randomly
    static ssize_t dataSrcRead(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
    static ssize_t dataSrcReadZcp(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);

    //callback is used only for zero-copy writes
    static int dataWrite(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data);

    static std::string getPrtlRefKey(const std::string& uniqFilePull, ssize_t streamID);
    static partialWritesCtx *getPwriteCtx(const std::string& uniqFilePull);

    static partialWritesCtx* configurePwrite(basicCtx* ctx, size_t& len); 
    static void threePartSubmit(partialWritesCtx* wrtCtx);

    static void cqeHandler(io_uring_cqe* cqe);

    public:
    http2PushService(dumpPresenceTable* table, int port);

    

    void listenL() override;
};