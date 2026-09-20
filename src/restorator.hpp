

#include "dumpFile.hpp"
#include "reflinkCopy.hpp"
#include "cgroupInjector.hpp"
#include "./containerdInterface/containerdWrapper.h"
#include "criuInterface.hpp"

class restorator{
    static std::filesystem::path cgroupBasePath;
    static std::filesystem::path socketPath;
    static std::string namespaceName;
    static criuInterface* criuCommands;

    dumpFile* toRestore;
    criu_opts* opts;
    int targetPid;
    const char* liveContainerRoot;

    
    public:
    restorator(dumpFile* dump);
    ~restorator();
    
    char* queryMergedFsPath();
    void beginRestore();
    std::string getCgroupPath();

};