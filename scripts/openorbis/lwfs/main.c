#include <fcntl.h>
#include <orbis/libkernel.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void describe(int fd, const char* step, long long result) {
    struct stat info = {0};
    int status = fstat(fd, &info);
    printf("LWFS step=%s result=%lld stat=%d size=%lld position=%lld\n", step, result,
           status, (long long)info.st_size, (long long)lseek(fd, 0, SEEK_CUR));
}

int main(int argc, char** argv) {
    char path[512];
    const char* root = argc > 1 ? argv[1] : "/download0";
    if (snprintf(path, sizeof(path), "%s/shadps4-lwfs-probe.bin", root) >= sizeof(path))
        return 1;
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        printf("LWFS root=%s result=UNAVAILABLE\n", root);
        return 2;
    }
    describe(fd, "attribute-on", sceKernelLwfsSetAttribute(fd, 1));
    lseek(fd, 17, SEEK_SET);
    describe(fd, "allocate-8192", sceKernelLwfsAllocateBlock(fd, 8192));
    describe(fd, "allocate-4096", sceKernelLwfsAllocateBlock(fd, 4096));
    describe(fd, "allocate-zero", sceKernelLwfsAllocateBlock(fd, 0));
    describe(fd, "seek", sceKernelLwfsLseek(fd, 31, SEEK_SET));
    const unsigned char value = 43;
    describe(fd, "write", sceKernelLwfsWrite(fd, &value, sizeof(value)));
    unsigned char actual = 0;
    lseek(fd, 31, SEEK_SET);
    ssize_t bytes = read(fd, &actual, sizeof(actual));
    printf("LWFS step=readback bytes=%lld value=%u\n", (long long)bytes, actual);
    describe(fd, "attribute-off", sceKernelLwfsSetAttribute(fd, 0));
    close(fd);
    unlink(path);
    return 0;
}
