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
            //should point straight right to the hashmap so no dangling risk? 
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

    //kinda think thats nonsense
    if((headerName == ":status" and headerValue != "200") 
                             or 
       (headerName == ":method" and headerValue != "GET")) {
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






//the final split decided for this ought to be just writing events, completion queues are needed to finish
ssize_t http2PushService::dataSrcReadZcp(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data) {
    auto tmp = nghttp2_session_get_stream_user_data(session, stream_id);
    basicCtx* ctx = static_cast<basicCtx*>(tmp);

    *data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;

    
    dirent* dentry = ctx->activeDentry;
    //this is cold open dentry transmission
    if(ctx->activeDentry == nullptr){
        partialWritesCtx *wrtCtx = new partialWritesCtx();
        ctx->wrtCtx = wrtCtx;

        dentry = readdir(ctx->openDir);
        ctx->activeDentry = dentry;
        
        auto wrCtx = configurePwrite(ctx, length);
        ctx->wrtCtx = wrCtx;

        ctx->dentryOffset = 0u;
        source->fd = open(dentry->d_name, O_RDONLY);
        
        return 0;
    }
        
    

    //this is directory advance if the current file was fully transmitted
    if(ctx->dentryOffset == dentry->d_reclen){
        ctx->activeDentry = readdir(ctx->openDir);
        ctx->dentryOffset = 0u;
        
        //the old one has to be collected entirely by the cqe callbacks, the reference is held there, the old one does not dangle
        partialWritesCtx *wrtCtx = new partialWritesCtx();
        ctx->wrtCtx = wrtCtx;

        source->fd = open(ctx->activeDentry->d_name, O_RDONLY);
        return 0;
    }

    //shouldnt happen
    throw std::runtime_error("unexpected state in dataSrcReadZcp");
    return 0;
}





int http2PushService::dataWrite(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data) {
    tcpCtx* tcpCtxRef = static_cast<tcpCtx*>(user_data);

    int32_t stream_id = frame->hd.stream_id;
    auto tmp = nghttp2_session_get_stream_user_data(session, stream_id);
    basicCtx* ctx = static_cast<basicCtx*>(tmp);
    auto dentry = ctx->activeDentry;
    

    if(dentry != nullptr){
        //write path for oversized files
        if(dentry->d_reclen > length-64){

            partialWritesCtx *wrtCtxRef = ctx->wrtCtx;

            //this is the header insert
            io_uring_sqe* sqeWriteFrame = io_uring_get_sqe(&ring);
            sqeWriteFrame->flags = IOSQE_IO_LINK;
            sqeWriteFrame->user_data = reinterpret_cast<uint64_t>(wrtCtxRef);
            io_uring_prep_write(sqeWriteFrame, ctx->outgoingFd, framehd, sizeof(framehd), SPLICE_F_MORE);

            //this is payload insert
            if(dentry->d_reclen >= length-64){
                int* pipeFds = pipeMgr.getPipe();

                io_uring_sqe* sqePipeRead = io_uring_get_sqe(&ring);
                sqePipeRead->flags = IOSQE_IO_LINK;
                sqePipeRead->user_data = reinterpret_cast<uint64_t>(wrtCtxRef);
                io_uring_prep_splice(sqePipeRead, tcpCtxRef->outgoingFd, -1, pipeFds[1], -1, SIXTYFOUR_KB, SPLICE_F_MORE);
                

                io_uring_sqe* sqePipeWrite = io_uring_get_sqe(&ring);
                sqePipeWrite->user_data = reinterpret_cast<uint64_t>(wrtCtxRef);
                io_uring_prep_splice(sqePipeWrite, pipeFds[0], 0, tcpCtxRef->outgoingFd, -1, SIXTYFOUR_KB, 0);


            }else{
                
            }


            //DELETE
            /*
            for(int x = length; x >= SIXTYFOUR_KB; x -= SIXTYFOUR_KB){
                
                int* pipeFds = pipeMgr.getPipe();

                io_uring_sqe* sqePipeRead = io_uring_get_sqe(&ring);
                sqePipeRead->flags = IOSQE_IO_LINK;
                sqePipeRead->user_data = reinterpret_cast<uint64_t>(wrtCtxRef);
                io_uring_prep_splice(sqePipeRead, `wrtCtxRef->openFd, -1, pipeFds[1], -1, SIXTYFOUR_KB, SPLICE_F_MORE);
                
                io_uring_sqe* sqePipeWrite = io_uring_get_sqe(&ring);
                sqePipeWrite->flags = IOSQE_IO_LINK;
                io_uring_prep_splice(sqePipeWrite, pipeFds[0], 0, ctx->outgoingFd, -1, SIXTYFOUR_KB, SPLICE_F_MORE);

                wrtCtxRef->pipes.push_back({pipeFds[0], pipeFds[1]});
                
                if(x < SIXTYFOUR_KB){
                    //since the chaining model interface is so stupid you have to not set a chain flag for the last one otherwise it will pull in the next unrelated sqe of another operation in here
                    //absolute neanderthals
                int* pipeFds = pipeMgr.getPipe();
                    io_uring_sqe* sqePipeRead = io_uring_get_sqe(&ring);
                    io_uring_prep_splice(sqePipeRead, wrtCtxRef->openFd, -1, pipeFds[1], -1, x, 0);
                
                    io_uring_sqe* sqePipeWrite = io_uring_get_sqe(&ring);
                    io_uring_prep_splice(sqePipeWrite, pipeFds[0], 0, ctx->outgoingFd, -1, x, 0);
                }

            }*/   //UP TO HERE
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



http2PushService::partialWritesCtx* http2PushService::configurePwrite(basicCtx* ctx, size_t& len) {
    if(ctx->activeDentry->d_reclen > len){
        //sets the needed amount of cqes to complete it, avoids any sort of screwups with increment/decrement race conditions
        unsigned int divsInto = ctx->activeDentry->d_reclen / len;
        //once for the header write and another one for packData append
        ctx->wrtCtx->completionTracker = (divsInto +2)*2; //the completions are doubled since the zero copy requires 2 sqe`s per chunk load baseline, both can do the short read bullshit btw

        if(ctx->activeDentry->d_reclen % len != 0){
            // once again for a remainder chunk
            ctx->wrtCtx->completionTracker += 2;
        }

    }


    if(ctx->activeDentry->d_ino < len){
        ctx->wrtCtx->completionTracker = 3; //once for header, once for packData, once for data
    }
}


//okay apperently this godsmaned event model is so utterly terrible you cannot actually count on it to trigger an event when all the data is
//actually sent over the wire but only when the kernel accepted it, so you gotta track whether a " COMPLETION " was AT ALL a completion
//then you gotta resubmit it, effectivley meaning you need to do your own chaining and context management for the whole entire thing
//its not implemented in the kernel, it should be, but clearly i cannot achieve such GODLY insight as the person who designed this WONDERFUL system