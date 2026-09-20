#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>


#include "transferService.hpp"
#include "extImport/httplib.h"
#include "dumpPresenceTable.hpp"

connInfo httpTransferService::connect(std::string endpoint){
    //tcp connects to the remote socket, maintains a single connection alive indefinitley
    connInfo newConn;

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9999);

    if(inet_pton(AF_INET, endpoint.c_str(), &serverAddr.sin_addr) != 1){
        throw std::invalid_argument("invalid IPv4 endpoint: " + endpoint);
    }

    newConn.socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if(newConn.socketFd < 0){
        throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
    }

    if(::connect(newConn.socketFd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0){
        const std::string error = std::strerror(errno);
        close(newConn.socketFd);
        throw std::runtime_error("connect: " + error);
    }

    newConn.sockStr = serverAddr;
    newConn.service = this;
    return newConn;

}

httpTransferService::httpTransferService(std::string remoteEndpoint){

    if(remoteEndpoint == "DUMMY"){
        return;
    }

    connInfo newEntry;
    epoll_event event{EPOLLIN};

    if(this->epfd < 0){
        this->epfd = epoll_create1(0);
        
        if(epoll_ctl(this->epfd, EPOLL_CTL_ADD, newEntry.socketFd, &event) < 0){
            throw std::runtime_error("epoll_ctl: " + std::string(std::strerror(errno)));
        }

    }

    if(this->channelFdArr[0] == 0){
        this->channelFdArr[0] = -1;
        this->channelFdArr[1] = -1;

        if(pipe2(this->channelFdArr, O_NONBLOCK) < 0){
            throw std::runtime_error("pipe2: " + std::string(std::strerror(errno)));
        }

        event.data.fd = this->channelFdArr[0];
        if(epoll_ctl(this->epfd, EPOLL_CTL_ADD, this->channelFdArr[0], &event) < 0){
            throw std::runtime_error("epoll_ctl: " + std::string(std::strerror(errno)));
        }
    }


    //just gotta make sure that another thread didnt insert it
    //initially i wanted to spawn a bunch of services and just have them dedup internally but i gave it up, the code remains and a single
    //further check on the control path isnt exactly that big a deal
    this->remoteEndpoint;
    auto aConn = this->activeEndpoints.find(this->remoteEndpoint);
    if(aConn == this->activeEndpoints.end()){
        try{
            newEntry = this->connect(this->remoteEndpoint);
            this->activeEndpoints[this->remoteEndpoint] = newEntry;
        } catch(const std::exception &e){
            throw std::runtime_error("Failed to connect to remote endpoint: " + std::string(e.what()));
        }

        if(epoll_ctl(this->epfd, EPOLL_CTL_ADD, newEntry.socketFd, &event) < 0){
            throw std::runtime_error("epoll_ctl: " + std::string(std::strerror(errno)));
        }
    }

    this->remoteEndpoint = remoteEndpoint;
    this->remoteFd = newEntry.socketFd;

    this->pullThread = std::jthread(&httpTransferService::pullWorker, this);
    pullThread.detach();
    
    // basically exists already, do nothing
}


void httpTransferService::pullWorker(){
    this->pullWorkerUp = true;
    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    
    std::unordered_map<int32_t, basicCtx> streamCtx;

    //declare callbacks here
    nghttp2_session_callbacks_set_on_header_callback(callbacks, this->headerRecvCback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, dataChunkRecvCback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, endStreamCback);

    //starts the thing 
    nghttp2_session_client_new(&session, callbacks, &streamCtx);

   

    char staticBuffer[1024];

    while(true){
        epoll_event events[1];
        int nfds = epoll_wait(this->epfd, events, 1, -1);
        if(nfds < 0){
            throw std::runtime_error("epoll_wait: " + std::string(std::strerror(errno)));
        }

        // alt path for initing pulls, notified over the channel
        if(events->data.fd == this->channelFdArr[0]){
            uint64_t buf;
            read(this->channelFdArr[0], &buf, sizeof(buf));
            pullNotify* notify = reinterpret_cast<pullNotify*>(buf);
            std::string path = "/perfPull/" + notify->dumpID;
            int Sz = path.length();
            

            nghttp2_nv hdrs[] = {
                { (uint8_t *)":method",    (uint8_t *)"GET",         7,  3,  NGHTTP2_NV_FLAG_NONE },
                { (uint8_t *)":path",      (uint8_t *)path.c_str(),  5,  Sz, NGHTTP2_NV_FLAG_NONE },
                { (uint8_t *)":scheme",    (uint8_t *)"http",        7,  4,  NGHTTP2_NV_FLAG_NONE },
                { (uint8_t *)":authority", (uint8_t *)"127.0.0.1",   10, 9,  NGHTTP2_NV_FLAG_NONE }
            };

            basicCtx* bctx = new basicCtx{};
            int32_t stream_id = nghttp2_submit_request(session, NULL, hdrs, 4, NULL, bctx);

            if (stream_id > 0) {
                //this thing is blocking, outgoing frames ought to be small anyway and the connection is guaranteed to be clear due to IO blocking for the writes on this end? No prob under load?
                const uint8_t *out;
                while(nghttp2_session_want_write(session)){
                    //this can technically block forever but i dont seriously expect it to actually happen
                    //will add some sort of debounce and yield to epoll
                    auto wrLen = nghttp2_session_mem_send(session, &out);
                    write(this->remoteFd, out, wrLen);
                }
                continue;
                
            }

            //its all heap allocated
            delete notify;
            continue;
        }

        for(int i = 0; i < nfds; ++i){
            // Handle the events here
            // For now, just print the file descriptor that is ready
            int bytesRead = read(events[i].data.fd, staticBuffer, sizeof(staticBuffer));
            if(bytesRead < 0){
                //dont plan on moving to edge triggered, will just keep it like that
                throw std::runtime_error("read: " + std::string(std::strerror(errno)));
            }

            nghttp2_session_mem_recv(session, reinterpret_cast<const uint8_t*>(staticBuffer), bytesRead);
        }
    }
}


// i dont wanna do lifecycle right now, maybe in the evening

int httpTransferService::headerRecvCback(nghttp2_session *session, 
    const nghttp2_frame *frame, 
    const uint8_t *name, 
    size_t name_len,
    const uint8_t *value, 
    size_t value_len, 
    uint8_t flags,
    void *user_data){
    //cant be arsed to deal with 404/200 parsing right now
    
    //i have not set up a root dir system so i guess ill just have this thing around, ill eventually fix it or hardcode it for testing
    std::string tempRoot;

    std::string_view headerName(reinterpret_cast<const char*>(name), name_len);
    std::string_view headerValue(reinterpret_cast<const char*>(value), value_len);

    if(frame->hd.type != NGHTTP2_HEADERS){
        return 0;
    }

    //404 jumps off the hot path instantly
    if(headerName == ":status" and headerValue == "404"){
        return -1;
    }

    auto temp = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    basicCtx* bctx = static_cast<basicCtx*>(temp);
    
    //pretty sure it wont ever trigger, how bad can it be to have a check
    if(bctx != nullptr){
        if(headerName == ":path"){
            tempRoot += std::string(headerValue);
        }

        int resOp = open(tempRoot.c_str(), O_CREAT | O_RDWR, 0644);
        if(resOp < 0){
            throw std::runtime_error("open failed in nghttp2 header callback: " + std::string(std::strerror(errno)));
        }

        bctx->openFd = resOp;
    }
    

    return 0;
}

void httpTransferService::frameRecvCback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data){
    //might get rid of this thing
}

int httpTransferService::dataChunkRecvCback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data){
    //relies on blocking writes, no uring annoyance, good enough for testing i guess

    auto ctxMap = static_cast<std::unordered_map<int32_t, httpTransferService::basicCtx>*>(user_data);

    //theese neanderthals saw fit to give me a stream ID but no pointer to my actual struct inside tha callback signatures for god knows what reason
    //apperently you just HAVE to call this thing to get your stuff and then cast it
    auto temp = nghttp2_session_get_stream_user_data(session, stream_id);
    basicCtx* bctx = static_cast<basicCtx*>(temp);

    httpTransferService::basicCtx &bctx = (*ctxMap)[stream_id];
    ssize_t written = write(bctx->openFd, data, len);
    if(written < 0){
        throw std::runtime_error("write failed in data chunk callback: " + std::string(std::strerror(errno)));
    }

    return 0;
}

int httpTransferService::endStreamCback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data){
    auto tmp = nghttp2_session_get_stream_user_data(session, stream_id);
    httpTransferService::basicCtx* bctx = static_cast<httpTransferService::basicCtx*>(tmp);

    fsync(bctx->openFd);
    close(bctx->openFd);

    filePresence.add(bctx->id, dumpFile(bctx->id, ""));

    delete bctx;
    return 0;
}




void httpTransferService::performPull(std::string dumpId){
    if(this->pullWorkerUp == false){
        //i cant imagine this happening at all
        throw std::exception();
    }

    
    pullNotify* notify = new pullNotify();
    notify->dumpID = dumpId;
    notify->remoteEndpoint = this->remoteEndpoint;

    write(channelFdArr[1], notify, sizeof(pullNotify));
    //there is basically 0 state reporting, given this ought to be driven by an operatorSDK binary i think ill just write some emitter system to dump out state in etcd but for now idk

    return;
}


bool httpTransferService::shouldOpen(std::string remoteAddr){
    if(this->activeEndpoints.find(remoteAddr) != this->activeEndpoints.end()){
        return true;
    }
    return false;
}

void httpTransferService::preInitialize(){
    this->channelFdArr[0] = -1;
    this->channelFdArr[1] = -1;
    this->epfd = -1;
}



