#include "transferPushSvc.hpp"

#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <string_view>

http2PushService::http2PushService(dumpPresenceTable* table, int port): pushService(table) {
    epfd = epoll_create1(0);
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    bind(serverSocket, (struct sockaddr*)&address, sizeof(address));
    //wont have more than like 4 nodes anyway so
    listen(serverSocket, 32);

    if (io_uring_queue_init(4096, &ring, 0) < 0) {
        throw std::runtime_error("io_uring_queue_init failed");
    }

    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, http2PushService::onHeaderRecv);

    nghttp2_data_provider dataProvider;
    dataProvider.read_callback = http2PushService::dataSrcRead;

    nghttp2_session_server_new(&session, callbacks, nullptr);
    
    this->ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    this->ev.data.fd = serverSocket;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serverSocket, &this->ev);
}




void http2PushService::listenL() {
    // i dont feel like putting it in its user data right now
    std::unordered_map<int, tcpCtx> activeFds;

    int sixtnKB = 16 * 1024;
    //its a little big for header manipulation
    uint8_t staticBuffer[sixtnKB];
    while (true){
        int nfds = epoll_wait(epfd, &ev, 1, -1);



        if(ev.data.fd == ring.ring_fd){
            cqeHandler(reinterpret_cast<io_uring_cqe*>(ev.data.ptr));
        }

        if(activeFds.find(ev.data.fd) == activeFds.end()) {
            nghttp2_session_callbacks* callbacks;
            nghttp2_session_callbacks_new(&callbacks);
            nghttp2_session_callbacks_set_on_header_callback(callbacks, http2PushService::onHeaderRecv);
            nghttp2_session_callbacks_set_send_data_callback(callbacks, http2PushService::dataWrite);

            tcpCtx ctx;
            ctx.outgoingFd = ev.data.fd;
            
            activeFds.insert({ev.data.fd, ctx});
            auto safePointer = &activeFds[ev.data.fd];
            safePointer->outgoingFd = ev.data.fd;
            nghttp2_session_server_new(&safePointer->session, callbacks, safePointer);
            //will just repeat, next connection will loop back to mem_recv to get drained
            //if i change trigger mechanism i need to change it to explicitly drain

        }else{
            if(ev.events & EPOLLIN){
            //straight dump into async writers, should do exclusivley metadata operations, nothing too blocking
            for(int i = 0; i < nfds; ++i) {
                nghttp2_session_mem_recv(activeFds[ev.data.fd].session, staticBuffer, sixtnKB);
            }
        }
        }




        if(ev.events & (EPOLLHUP | EPOLLERR)) {
            close(ev.data.fd);
            tcpCtx* ctx = &activeFds[ev.data.fd]; 
            nghttp2_session_del(ctx->session);
            delete ctx;

            activeFds.erase(ev.data.fd);
            continue;
        }


        
        

    }
}





int http2PushService::onHeaderRecv(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len, uint8_t flags, void *user_data) {
    
    std::string_view headerName(reinterpret_cast<const char*>(name), name_len);
    std::string_view headerValue(reinterpret_cast<const char*>(value), value_len);

    if(headerName == ":status" and headerValue != "200") {
        //closes stream, keeps tcp open
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id, NGHTTP2_INTERNAL_ERROR);
            
        return 0;
    }



    if(headerName == ":path"){
        if(presenceTable->has(headerValue.data())) {

            //just sets up the data provider
            basicCtx* ctx = new basicCtx();
            //have to add the base path

            if(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id) == nullptr){
                nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, ctx);
            }

            ctx->openDir = opendir(headerValue.data());
            

            //this basically locks the class into it being a singleton per process
            //that wont be that big a deal, given that i could effectivley move the slow parts to asynchronous io_uring calls
            epoll_ctl(epfd, EPOLL_CTL_MOD, serverSocket, &ev);
            //if i got here it must mean its safe to write, just mem send?

            nghttp2_nv pathHeader[1];
            //apperently theese need flags set to not dangle after this goes out of scope
            ctx->src.read_callback = http2PushService::dataSrcReadZcp;

            nghttp2_submit_request(session, nullptr, pathHeader, 1, &ctx->src, ctx);
            allowNetworkFlush();
        }
    }



    return 0;
}



ssize_t http2PushService::dataSrcRead(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data) {
        basicCtx* ctx = reinterpret_cast<basicCtx*>(source->ptr);

        //if this thing does not overflow at least a dozen times and waste me at least a week of time to chase
        //down later i wont be pleased
    
        dirent* dentry = ctx->activeDentry;
        if(ctx->activeDentry == nullptr){   
            dentry = readdir(ctx->openDir);
            ctx->activeDentry = dentry;
        }

        int deferCount = 0;
    
        while(dentry != nullptr) {
        //assuming its all a flat structure with nothing weird, no nested dirs no nothing

        
            const size_t nameLength = strnlen(dentry->d_name, sizeof(dentry->d_name));
            std::string uniqFilePull(reinterpret_cast<const char*>(&stream_id), sizeof(stream_id));
            uniqFilePull.append(dentry->d_name, nameLength);

            partialWritesCtx *partialWriteRef;
            if(partialWritesMap.find(uniqFilePull) == partialWritesMap.end()) {
                partialWritesCtx partialWrite;
                partialWrite.lastWriteEnd = 0u;
                partialWrite.openFd = -1;

                partialWritesMap[uniqFilePull] = partialWrite;
                partialWriteRef = &partialWritesMap[uniqFilePull];


            }
            else {
                partialWriteRef = &partialWritesMap[uniqFilePull];
            }


            if(partialWriteRef->openFd == -1){
                partialWriteRef->openFd = open(dentry->d_name, O_RDONLY);
            }

            //kind of a rough saftey margin, this wil most definitley overfow and waste a whole lot of padding bytes in the rare event it works OK
            if(dentry->d_reclen > length-64){

            
                partialWriteRef->lastWriteEnd = 0u;
                packData pck;
                pck.fileName = dentry->d_name;
                pck.size = length-64;

                if(sizeof(pck) > 64){
                    throw std::runtime_error("the metadata string in the frame packer blew the buffer");
                }

                //write for bigger than files
                memcpy(buf, &pck, sizeof(pck));
                lseek(partialWriteRef->openFd, partialWriteRef->lastWriteEnd, SEEK_SET);

                ssize_t bytesRead = read(partialWriteRef->openFd, buf + sizeof(pck), pck.size);
                if(bytesRead > 0) {
                    partialWriteRef->lastWriteEnd += bytesRead;
                }

                deferCount++;
                
            }else{

                int bufPosPtr = length-64;
                while(dentry->d_reclen <= bufPosPtr) {

    
                    partialWritesMap[uniqFilePull] = *partialWriteRef;

                    packData pck;
                    pck.fileName = dentry->d_name;
                    pck.size = dentry->d_reclen;

                    if(sizeof(pck) > 64){
                        throw std::runtime_error("the metadata string in the frame packer blew the buffer");
                    }

                    memcpy(buf + sizeof(pck), &pck, sizeof(pck));   
                    lseek(partialWriteRef->openFd, partialWriteRef->lastWriteEnd, SEEK_SET);

                    ssize_t bytesRead = read(partialWriteRef->openFd, buf + sizeof(pck), pck.size);
                    //will just let it blow up if it fails
                
                    close(partialWriteRef->openFd);
                    partialWritesMap.erase(uniqFilePull);
                
                    deferCount++;
                }

            dentry = readdir(ctx->openDir);
            if(deferCount > 50){
                return NGHTTP2_ERR_DEFERRED;
            }
        }

    
    }

    allowNetworkFlush();

    return NGHTTP2_RST_STREAM;

}







//this thing apperently can just hand me a header that i send from user space and then set up a zero copy io_uring submittion with strict ordering, should yield the event loop fast
//as well as solving the DEFERRED and reader writer split nonsense in the epoll_wait loop, its not like the other thing where i have to track copmletions or anything so a fire-and-forget approach works
ssize_t http2PushService::dataSrcReadZcp(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data) {
    auto tmp = nghttp2_session_get_stream_user_data(session, stream_id);
    basicCtx* ctx = static_cast<basicCtx*>(tmp);

    *data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;

    
    dirent* dentry = ctx->activeDentry;
        if(ctx->activeDentry == nullptr){   
            dentry = readdir(ctx->openDir);
            ctx->activeDentry = dentry;
        }



    
        return 0;
}





int http2PushService::dataWrite(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data) {
    
    int32_t stream_id = frame->hd.stream_id;
    auto tmp = nghttp2_session_get_stream_user_data(session, stream_id);
    basicCtx* ctx = static_cast<basicCtx*>(tmp);
    auto dentry = ctx->activeDentry;

    while(dentry != nullptr){
        //write path for oversized files
        if(dentry->d_reclen > length-64){


            std::string refKey = getPrtlRefKey(dentry->d_name, stream_id);
            partialWritesCtx *wrtCtxRef = getPwriteCtx(refKey);
            wrtCtxRef->refkey = refKey;

            if(wrtCtxRef != nullptr) {

                for(int x = length; x >= SIXTYFOUR_KB; x -= SIXTYFOUR_KB){
                    wrtCtxRef->openFd = open(dentry->d_name, O_RDONLY);
            
                    int* pipeFds = pipeMgr.getPipe();

                    io_uring_sqe* sqePipeRead = io_uring_get_sqe(&ring);
                    sqePipeRead->flags = IOSQE_IO_LINK;
                    sqePipeRead->user_data = reinterpret_cast<uint64_t>(wrtCtxRef);
                    io_uring_prep_splice(sqePipeRead, wrtCtxRef->openFd, -1, pipeFds[1], -1, length, SPLICE_F_MORE);
                
                    io_uring_sqe* sqePipeWrite = io_uring_get_sqe(&ring);
                    sqePipeWrite->flags = IOSQE_IO_LINK;
                    io_uring_prep_splice(sqePipeWrite, pipeFds[0], 0, ctx->outgoingFd, -1, length, SPLICE_F_MORE);

                    wrtCtxRef->pipes.push_back({pipeFds[0], pipeFds[1]});
                
                    if(x < SIXTYFOUR_KB){
                        //since the chaining model interface is so stupid you have to not set a chain flag for the last one otherwise it will pull in the next unrelated sqe of another operation in here
                        //absolute neanderthals
                        int* pipeFds = pipeMgr.getPipe();
                        io_uring_sqe* sqePipeRead = io_uring_get_sqe(&ring);
                        io_uring_prep_splice(sqePipeRead, wrtCtxRef->openFd, -1, pipeFds[1], -1, length, 0);
                
                        io_uring_sqe* sqePipeWrite = io_uring_get_sqe(&ring);
                        io_uring_prep_splice(sqePipeWrite, pipeFds[0], 0, ctx->outgoingFd, -1, length, 0);
                    }

                }
            }
        }
    }    



    return 0;
}


void http2PushService::allowNetworkFlush() {
    if(ev.events & (EPOLLIN | EPOLLOUT)){
        ev.events = EPOLLIN | EPOLLOUT;
        epoll_ctl(epfd, EPOLL_CTL_MOD, serverSocket, &ev);
    }
}

int http2PushService::getRadomStream() {
    int randPos = rand() % 100; 
}



std::string http2PushService::getPrtlRefKey(const std::string& uniqFilePull, ssize_t streamID) {
    std::string key;
    key.reserve(sizeof(streamID) + uniqFilePull.size() + 1);
    key.append(reinterpret_cast<const char*>(&streamID), sizeof(streamID));
    key.append(uniqFilePull);
    return key;
}




http2PushService::partialWritesCtx *http2PushService::getPwriteCtx(const std::string& uniqFilePull) {
    auto it = partialWritesMap.find(uniqFilePull);
    if(it != partialWritesMap.end()) {
        return &(it->second);
    }
    return nullptr;
}


void http2PushService::cqeHandler(io_uring_cqe* cqe) {
    partialWritesCtx* wrtCtxRef = reinterpret_cast<partialWritesCtx*>(cqe->user_data);
    // there is an ordering guarantee, the first completion entry should be that of the first submission entry so i should aways
    //remote the first element of the vector, should align but if i change the thing its good to know it can wipe the pipes of active writers
    
    
    if(!wrtCtxRef->pipes.empty()) {
        int* pipeFds = wrtCtxRef->pipes.front();
        wrtCtxRef->pipes.erase(wrtCtxRef->pipes.begin());
        pipeMgr.returnPipe(pipeFds);

        if(wrtCtxRef->pipes.empty()) {
            close(wrtCtxRef->openFd);
            partialWritesMap.erase(wrtCtxRef->refkey);
        }
    }

    wrtCtxRef->pipes.shrink_to_fit();
}