#include <sys/socket.h>
#include <unordered_map>
#include <nghttp2/nghttp2.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <thread>
#include <functional>
#include <stack>

#include "dumpFile.hpp"

//base for the transfer service
struct connInfo{
    sockaddr_in sockStr;
    int socketFd;
    dumpTransferService *service;
};


//exists to make sure the static entries are initialized at all times




class dumpTransferService{
  

    protected:
    static std::unordered_map<std::string, connInfo> activeEndpoints;
    std::string dumpLocation;

    virtual void pullWorker() =0;
    virtual connInfo connect(std::string endpoint) =0;
    
    public:
    virtual bool shouldOpen(std::string remoteAddr) =0;
    virtual void performPull(std::string dumpId) =0;
    dumpTransferService();
    dumpTransferService(std::string remoteEndpoint);
    virtual void preInitialize() = 0;

    dumpTransferService* getRef(std::string remoteEndpoint){
        auto it = activeEndpoints.find(remoteEndpoint);
        if(it != activeEndpoints.end()){
            return it->second.service;
        }
        return nullptr;
    }

};





//does http pulls, i decided i am driving the http module imperativley, you recieve a command to do it and establish connection then you keep it
//the reap cycle will decide what to do with it later
class httpTransferService : public dumpTransferService{
    std::jthread pullThread;
    std::string remoteEndpoint;
    //no locks on this but im pretty sure its not needed at all
    int epfd =-1;
    int channelFdArr[2];
    int remoteFd =-1;
    
    static bool pullWorkerUp;
    nghttp2_session *session;


    static void frameRecvCback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
    static int headerRecvCback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len, uint8_t flags, void *user_data);
    static int dataChunkRecvCback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
    static int endStreamCback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);




    //for managing DATA frames
    struct basicCtx{
        int openFd;
        std::string id;
        nghttp2_data_source src;
    };

    //for initiating a pull from a remote
    struct pullNotify{
        std::string dumpID;
        std::string remoteEndpoint;
    };


    protected:
    void pullWorker() override;
    connInfo connect(std::string endpoint) override;

    public:
    httpTransferService(std::string remoteEndpoint);
    void preInitialize() override;
    void performPull(std::string dumpId) override;
    bool shouldOpen(std::string remoteAddr) override;

};






class nvmeTcpTransferService : public dumpTransferService{
    protected:
    void pullWorker() override;
    connInfo connect(std::string endpoint) override;
    void preInitialize() override;

    public:
    void performPull(std::string dumpId) override;
    void preInitialize() override;
    bool shouldOpen(std::string remoteAddr) override;
  
    nvmeTcpTransferService(std::string remoteEndpoint);
};




//basically a dummy service to ensure the static fields exist
httpTransferService masterHttpSvc;

