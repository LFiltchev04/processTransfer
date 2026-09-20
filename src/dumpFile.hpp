#include <filesystem>
#include <string>

#include "cgroupInjector.hpp"

class dumpFile{
    std::filesystem::path dumpfilePath;
    std::string targetContainerHash;
    int openFd;

    public:
    dumpFile();
    dumpFile(std::string containerHash, std::string dumpPath);
    std::filesystem::path getDumpFilePath();
    std::string getTargetContainerID();

    std::string getPath();

};