#include "transferPushSvc.hpp"

#include <cstring>

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


    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, http2PushService::onHeaderRecv);

    nghttp2_data_provider dataProvider;
    dataProvider.read_callback = http2PushService::dataSrcRead;
    nghttp2_session_server_new(&session, callbacks, nullptr);
    
    this->ev.events = EPOLLIN;
    this->ev.data.fd = serverSocket;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serverSocket, &this->ev);
}

void http2PushService::listenL() {
    int sixtnKB = 16 * 1024;
    uint8_t staticBuffer[sixtnKB];
    while (true){
        int nfds = epoll_wait(epfd, &ev, 1, -1);

        if(ev.events & EPOLLIN){
            //theese inline processing steps could get quite slow if i dont thread pool this thing
            for(int i = 0; i < nfds; ++i) {
                nghttp2_session_mem_recv(session, staticBuffer, sixtnKB);
            }
        }
        

        if(ev.events & (EPOLLOUT | EPOLLIN)){
            //the path for reading code is lighter, i dont know whether a certain event came from a read or write
            //so i just double check, the EPOLLINs are not that hard to process and if i decide to multithread this
            //a simple solution like an intendWrite flag might just be enough? 

            for(int i = 0; i < nfds; ++i) {
                nghttp2_session_mem_recv(session, staticBuffer, sixtnKB);
            }

            const uint8_t* sendData;
            ssize_t sendLen;
            
            do{
                sendLen = nghttp2_session_mem_send(session, &sendData);
                send(ev.data.fd, sendData, sendLen, 0);
            }while(sendLen > 0);
            
            
        }
    }
}

int http2PushService::onHeaderRecv(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len, uint8_t flags, void *user_data) {
    
    std::string_view headerName(reinterpret_cast<const char*>(name), name_len);
    std::string_view headerValue(reinterpret_cast<const char*>(value), value_len);

    if(headerName == ":status" and headerValue != "200") {
        //closes stream, keeps tcp open
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id, NGHTTP2_INTERNAL_ERROR);
        //have to remember to clear theese
        if(ev.events != EPOLLIN | EPOLLOUT){
            ev.events = EPOLLIN | EPOLLOUT;
            epoll_ctl(epfd, EPOLL_CTL_MOD, serverSocket, &ev);
        }
            
        return 0;
    }



    if(headerName == ":path"){
        if(presenceTable->has(headerValue.data())) {
            //is this too wasteful?
            ev.events = EPOLLOUT;

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

            nvRow path{":path", headerValue.data()};
            
            nghttp2_nv* nvArr = makeNvHelper(new nvRow[1]{{":path", headerValue.data()}});
            nghttp2_submit_request(session, nullptr, nvArr, sizeof(nvArr)/sizeof(nghttp2_nv), &ctx->src, &ctx);
            delete[] nvArr;
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

}