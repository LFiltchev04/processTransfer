#include "transferPushSvc.hpp"

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

        for(int i = 0; i < nfds; ++i) {
            nghttp2_session_mem_recv(session, staticBuffer, sixtnKB);
        }
    }
}

int http2PushService::onHeaderRecv(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len, uint8_t flags, void *user_data) {
    

    std::string_view headerName(reinterpret_cast<const char*>(name), name_len);
    std::string_view headerValue(reinterpret_cast<const char*>(value), value_len);

    if(headerName == ":path"){
        if(presenceTable->has(headerValue.data())) {
            ev.events = EPOLLOUT;
        }
    }


    return 0;
}