#include <unistd.h>
#include <cstdint>
#include <stack>

#define UPLOAD_STATIC_BUFFER_SIZE (64 * 1024)

struct bufEntry {
    uint8_t buffer[UPLOAD_STATIC_BUFFER_SIZE];
};

//64 kb static bufffers
class uploadStaticBuffer {
    static constexpr size_t bufferSize = UPLOAD_STATIC_BUFFER_SIZE;
    uint8_t buffer[bufferSize];
    std::stack<bufEntry*> freeStack;

    public:
    uploadStaticBuffer() {

    }

    ~uploadStaticBuffer() {
        for (size_t i = 0; i < bufferSize; ++i) {
            
        }
    }


};