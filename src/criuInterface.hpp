#include <string>
#include <unistd.h>
#include <fcntl.h>

#include "criu/lib/c/criu.h"



class criuInterface {
    std::string socketPath;
    std::string dumpsDirBase;
    
    public:
    criuInterface(std::string socketPath);

    void restoreDump(criu_opts* optPtr, std::string dumpDir);
    void performDump(int pid, criu_opts* optPtr, std::string dumpDir);
    void flushDirtyPages(criu_opts* optPtr, std::string dumpDir);
    void finalBackup(criu_opts* optPtr, std::string dumpDir);

    std::string getDumpsDirBase();
};