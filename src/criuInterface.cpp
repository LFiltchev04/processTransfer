#include "criuInterface.hpp"


criuInterface::criuInterface(std::string socketPath) {
    this->socketPath = socketPath;
    int pid = fork();
    if(pid == 0){
        char *args[] = {"service", NULL};
        execv("../binaries/criu", args);
        exit(1);
    }

    printf("Criu service started\n");
}

void criuInterface::restoreDump(criu_opts* optPtr, std::string dumpDir){
    
    criu_local_set_service_address(optPtr, this->socketPath.c_str());
    
    //goddamn nonexistent docs, i hope that thats alright but unless i check everything its not certain
    int storeFd = open((this->dumpsDirBase + "/" + dumpDir).c_str(), O_RDONLY);
    criu_local_set_images_dir_fd(optPtr, storeFd);
    criu_local_set_tcp_established(optPtr, true);

    int ret = criu_local_restore(optPtr);
    if(ret != 0){
        printf("Criu restore failed with return code: %d\n", ret);
    }
}



void criuInterface::performDump(int pid, criu_opts* optPtr, std::string dumpDir){
    criu_local_set_service_address(optPtr, this->socketPath.c_str());

    criu_local_set_pid(optPtr, pid);
    
    int storeFd = open((this->dumpsDirBase + "/" + dumpDir).c_str(), O_RDONLY);
    criu_local_set_images_dir_fd(optPtr, storeFd);

    criu_local_set_tcp_established(optPtr, true);

    int ret = criu_local_dump(optPtr);
    if(ret != 0){
        printf("Criu dump failed with return code: %d\n", ret);
    }

}


void criuInterface::flushDirtyPages(criu_opts* optPtr, std::string dumpDir){
    criu_local_set_service_address(optPtr, this->socketPath.c_str());

    int storeFd = open((this->dumpsDirBase + "/" + dumpDir).c_str(), O_RDONLY);
    criu_local_set_images_dir_fd(optPtr, storeFd);
    criu_local_set_tcp_established(optPtr, true);

    criu_local_pre_dump(optPtr);
    //no point in fd reuse, just reopen them its nbd
    close(storeFd);
}

//freezes the process and kills it, have to move immediatley after this call
void criuInterface::finalBackup(criu_opts* optPtr, std::string dumpDir){
    criu_local_set_service_address(optPtr, this->socketPath.c_str());

    int storeFd = open((this->dumpsDirBase + "/" + dumpDir).c_str(), O_RDONLY);
    criu_local_set_images_dir_fd(optPtr, storeFd);
    criu_local_set_tcp_established(optPtr, true);

    //kills the actual process, gotta move it after that
    criu_local_dump(optPtr);

    close(storeFd);
}

std::string criuInterface::getDumpsDirBase(){
    return this->dumpsDirBase;
}
