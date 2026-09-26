#pragma once
#include <filesystem>
#include <unistd.h> 
#include <fcntl.h>



//switches over execution context to the targeted cgroup, operates as a normal fork() in regards to returned vals
inline int injectCgroup(std::filesystem::path cgroupPath ){

    //this needs a rework
    cgroupPath /= "cgroup.procs";
    pid_t newProcessPid = fork();

    if(newProcessPid == 0){
        int fd = open(cgroupPath.c_str(), O_WRONLY | O_APPEND);
        
        write(fd, "0", 1);
        
        //kind of messy, but state here will have to be handled by the caller, its a normal fork anyhow
        return 0;
    }


    return newProcessPid;

}
