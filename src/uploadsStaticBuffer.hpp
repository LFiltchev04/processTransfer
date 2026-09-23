#include <unistd.h>
#include <cstdint>
#include <stack>

#define UPLOAD_STATIC_BUFFER_SIZE (64 * 1024)
#define POOL_SIZE 24

struct bufEntry {
    uint8_t buffer[UPLOAD_STATIC_BUFFER_SIZE];
};

//64 kb static bufffers
//theese are likely getting the backspace relativley soon, no need to have them around
class uploadStaticBuffer {
    uint8_t pool[POOL_SIZE];
    std::stack<bufEntry*> freeStack;

    public:
    uploadStaticBuffer() {

        //i dont need this
        uint64_t sz = sizeof(bufEntry);
        uint64_t baseOffset = reinterpret_cast<uint64_t>(pool);
        for (int i = 0; i < POOL_SIZE; ++i) {
            // that wil definitley turn out to be a mistake
            freeStack.push(reinterpret_cast<bufEntry*>(baseOffset + i * sz));
        }
    }

    bufEntry* getBuffer() {
        if (freeStack.empty()) {
            return nullptr;
        }
        bufEntry* entry = freeStack.top();
        freeStack.pop();
        return entry;
    }

    void returnBuffer(bufEntry* entry) {
        freeStack.push(entry);
    }
};