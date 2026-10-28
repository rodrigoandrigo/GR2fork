#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* root = argc > 1 ? argv[1] : "/data";
    char path[512];
    if (snprintf(path, sizeof(path), "%s/shadps4-create-probe.bin", root) >= sizeof(path))
        return 1;

    unlink(path);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        printf("FILESYSTEM_CREATE root=%s errno=%d result=UNAVAILABLE\n", root, errno);
        return 2;
    }

    const unsigned char expected[] = {17, 29, 43};
    unsigned char actual[sizeof(expected)] = {0};
    int passed = write(fd, expected, sizeof(expected)) == sizeof(expected) &&
                 lseek(fd, 0, SEEK_SET) == 0 &&
                 read(fd, actual, sizeof(actual)) == sizeof(actual) &&
                 memcmp(expected, actual, sizeof(actual)) == 0;
    passed = passed && ftruncate(fd, 4096) == 0 && lseek(fd, 0, SEEK_CUR) == 3 &&
             lseek(fd, 0, SEEK_END) == 4096;
    unsigned char zero = 1;
    passed = passed && lseek(fd, 4095, SEEK_SET) == 4095 && read(fd, &zero, 1) == 1 &&
             zero == 0 && ftruncate(fd, 1) == 0 && lseek(fd, 0, SEEK_CUR) == 4096 &&
             lseek(fd, 0, SEEK_END) == 1;
    close(fd);

    errno = 0;
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    passed = passed && fd == -1 && errno == EEXIST;
    if (fd >= 0)
        close(fd);

    fd = open(path, O_TRUNC | O_RDWR);
    passed = passed && fd >= 0;
    if (fd >= 0) {
        passed = passed && lseek(fd, 0, SEEK_END) == 0;
        close(fd);
    }

    unlink(path);
    fd = open(path, O_CREAT | O_RDONLY, 0600);
    passed = passed && fd >= 0;
    if (fd >= 0) {
        passed = passed && read(fd, actual, sizeof(actual)) == 0;
        close(fd);
    }
    unlink(path);

    printf("FILESYSTEM_CREATE root=%s result=%s\n", root, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
