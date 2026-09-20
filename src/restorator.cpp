#include "restorator.hpp"



restorator::restorator(dumpFile* dump){
    toRestore = dump;
    liveContainerRoot = queryMergedFsPath();

}



void restorator::beginRestore(){
    auto dumpfileLocation = toRestore->getDumpFilePath();
    
    //forks over the contents of the dump file, requires btrfs or anything capable of FICLONE, should be over very fast
    reflinkCopy(dumpfileLocation, liveContainerRoot);

    auto mutableLiveContainerRoot = const_cast<char*>(liveContainerRoot);
    auto mutableSocketPath = const_cast<char*>(socketPath.c_str());
    auto mutableNamespaceName = const_cast<char*>(namespaceName.c_str());
    auto mutableContainerHash = const_cast<char*>(toRestore->getTargetContainerID().c_str());

    char* temp = getRealPid(mutableSocketPath, mutableNamespaceName, mutableContainerHash);
    targetPid = reinterpret_cast<int>(temp);
    printf("Target PID, hopefully it does not blow up: %d\n", targetPid);

    //carries out the fetch of cgroup, needed to procceed with restoration
    
    //this switches over to the appropriate cgroup, the entire process is dragged along with it. 
    int newPid = injectCgroup(getCgroupPath());

    if(newPid != 0){
        printf("This comes from the process not tapped to the actual caller, fork might have worked \n");
        return;
    }



    //this happens in the new forked process, should call the resotre methods here as well as initiate the local criu structs
    //i dont want to deal with lifecycles of that nonsense globally

    criuCommands->restoreDump(opts, toRestore->getDumpFilePath());
}



std::string restorator::getCgroupPath(){
    //it might be a little overkill for buffer size 
    char cgroupPath[PATH_MAX];
    snprintf(cgroupPath, PATH_MAX, "/proc/%d/cgroup", targetPid);

    int fd = open(cgroupPath, O_RDONLY);
    if(fd < 0){
        printf("Failed to open cgroup file for target PID %d\n", targetPid);
        return std::string();
    }

    //should never overflow but still
    auto readNum = read(fd, cgroupPath, PATH_MAX-1);
    if(readNum < 0){
        printf("Failed to read cgroup file for target PID %d\n", targetPid);
        close(fd);
        return std::string();
    }

    //am i double wrapping the null terminator? Does it matter?
    cgroupPath[readNum] = '\0';
    close(fd);
    std::string fullPath = std::string(cgroupPath);
    fullPath.erase(0, 3);
    fullPath = cgroupBasePath.string() + fullPath;
    printf("Full cgroup path: %s\n", fullPath.c_str());
    
    return fullPath;
}



char* restorator::queryMergedFsPath(){
    //go ought to not touch them and the things are going to exist due to lifecycle tracking on this end
    auto sockP = socketPath.c_str();
    char* mutableSockP = const_cast<char*>(sockP);

    auto ns = namespaceName.c_str();
    char* mutableNs = const_cast<char*>(ns);

    auto targetHash = toRestore->getTargetContainerID().c_str();
    char* mutableTargetHash = const_cast<char*>(targetHash);

    char* fullval = getTagged(mutableSockP, mutableNs, mutableTargetHash);

    return fullval;
}





restorator::~restorator(){
    delete toRestore;
    delete liveContainerRoot;
}