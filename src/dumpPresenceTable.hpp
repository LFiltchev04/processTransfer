#include <unordered_map>
#include <mutex>

#include "dumpFile.hpp" 


dumpPresenceTable filePresence;

class dumpPresenceTable{
    static std::unordered_map<std::string, dumpFile> table;
    static std::mutex tableMutex;

    public:
    bool has(const std::string &key) const {
        std::lock_guard<std::mutex> lock(tableMutex);
        return table.find(key) != table.end();
    }

    dumpFile* get(const std::string &key) const {
        std::lock_guard<std::mutex> lock(tableMutex);
        auto it = table.find(key);
        if(it != table.end()){
            return &it->second;
        }
        return nullptr;
    }

    void add(const std::string &key, const dumpFile &file) {
        std::lock_guard<std::mutex> lock(tableMutex);
        table[key] = file;
    }

    void remove(const std::string &key) {
        std::lock_guard<std::mutex> lock(tableMutex);
        table.erase(key);
    }
};