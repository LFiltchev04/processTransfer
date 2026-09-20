#include "dumpFile.hpp"


dumpFile::dumpFile(std::string containerHash, std::string dumpPath){
    dumpfilePath = dumpPath;
    targetContainerHash = containerHash;
}


std::filesystem::path dumpFile::getDumpFilePath(){
    return dumpfilePath;
}


std::string dumpFile::getTargetContainerID(){
    return targetContainerHash;
}


std::string dumpFile::getPath(){
    return dumpfilePath.string();
}