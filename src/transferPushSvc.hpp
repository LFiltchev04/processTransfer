#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>

#include "dumpPresenceTable.hpp"


class pushService {

    protected:
    static dumpPresenceTable* presenceTable;

    public:
    pushService(dumpPresenceTable* table){ presenceTable = table; };

    virtual void listenL() = 0;
    
};



class http2PushService: public pushService {
    int serverSocket;
    int epfd;
    static epoll_event ev;

    nghttp2_session* session;

    struct basicCtx{
        int dumpFd;
        int outgoingFd;
    };

    static int onHeaderRecv(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len, uint8_t flags, void *user_data);
    static int dataSrcRead(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);


    public:
    http2PushService(dumpPresenceTable* table, int port);

    

    void listenL() override;
};