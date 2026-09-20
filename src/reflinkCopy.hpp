#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>


void reflinkCopy(std::filesystem::path source, std::filesystem::path dest){
    int srcFd = open(source.c_str(), O_RDONLY);
    int destFd = open(dest.c_str(), O_WRONLY | O_CREAT, 0644);

    if(srcFd < 0 || destFd < 0){
        printf("Failed to open source or destination file for reflink copy\n");
        close(srcFd);
        close(destFd);
        return;
    }

    int ret = ioctl(destFd, FICLONE, srcFd);
    if(ret < 0){
        printf("Failed to perform reflink copy from %s to %s\n", source.c_str(), dest.c_str());
    }

    close(srcFd);
    close(destFd);
}



