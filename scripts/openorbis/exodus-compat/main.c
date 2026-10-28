#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* root = argc > 1 ? argv[1] : "/data";
    char path[512], alias[512], directory[512], child[512], missing[512];
    if (snprintf(directory, sizeof(directory), "%s/shadps4-exodus-hardware", root) >=
            sizeof(directory) ||
        snprintf(path, sizeof(path), "%s/file.bin", directory) >= sizeof(path) ||
        snprintf(alias, sizeof(alias), "%s/./file.bin", directory) >= sizeof(alias) ||
        snprintf(child, sizeof(child), "%s/child", directory) >= sizeof(child) ||
        snprintf(missing, sizeof(missing), "%s/missing/child", directory) >= sizeof(missing)) {
        return 2;
    }
    if (mkdir(directory, 0700) != 0) {
        printf("EXODUS_FS unavailable errno=%d; use a writable root and a fresh directory\n",
               errno);
        return 2;
    }
    int first = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    int second = open(alias, O_RDWR);
    if (first < 0 || second < 0) {
        if (first >= 0)
            close(first);
        if (second >= 0)
            close(second);
        unlink(path);
        rmdir(directory);
        return 2;
    }
    int passed = 1;
    struct stat by_path = {0}, by_fd = {0}, by_alias = {0};
    int identities = stat(path, &by_path) == 0 && fstat(first, &by_fd) == 0 &&
                     fstat(second, &by_alias) == 0 && by_path.st_ino != 0 &&
                     by_path.st_ino == by_fd.st_ino && by_fd.st_ino == by_alias.st_ino;
    printf("EXODUS_FS inode=%s\n", identities ? "PASS" : "FAIL");
    passed &= identities;

    int locks = flock(first, LOCK_EX | LOCK_NB) == 0;
    errno = 0;
    locks &= flock(second, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK;
    locks &= flock(first, LOCK_SH) == 0 && flock(second, LOCK_SH | LOCK_NB) == 0;
    locks &= flock(first, LOCK_UN) == 0 && flock(second, LOCK_UN) == 0;
    locks &= flock(first, LOCK_EX | LOCK_NB) == 0;
    close(first);
    locks &= flock(second, LOCK_EX | LOCK_NB) == 0;
    locks &= flock(second, LOCK_UN) == 0;
    printf("EXODUS_FS flock=%s\n", locks ? "PASS" : "FAIL");
    passed &= locks;

    const char bytes[] = "file-metadata";
    int metadata = write(second, bytes, sizeof(bytes)) == sizeof(bytes) &&
                   fstat(second, &by_fd) == 0 && stat(path, &by_path) == 0 &&
                   by_fd.st_size == sizeof(bytes) && by_path.st_size == by_fd.st_size;
    errno = 0;
    metadata &= ftruncate(second, -1) == -1 && errno == EINVAL;
    close(second);
    printf("EXODUS_FS metadata=%s\n", metadata ? "PASS" : "FAIL");
    passed &= metadata;

    int directories = mkdir(child, 0700) == 0;
    errno = 0;
    directories &= rmdir(directory) == -1 && errno == ENOTEMPTY;
    directories &= access(path, F_OK) == 0 && access(child, F_OK) == 0;
    errno = 0;
    directories &= mkdir(missing, 0700) == -1 && errno == ENOENT;
    printf("EXODUS_FS directories=%s\n", directories ? "PASS" : "FAIL");
    passed &= directories;

    int null_fd = open("/dev/null", O_RDWR);
    char value = 1;
    int null_device = null_fd >= 0;
    if (null_fd >= 0) {
        null_device &= write(null_fd, &value, 1) == 1 && read(null_fd, &value, 1) == 0;
        close(null_fd);
    }
    printf("EXODUS_FS null-device=%s\n", null_device ? "PASS" : "FAIL");
    passed &= null_device;
    unlink(path);
    rmdir(child);
    rmdir(directory);
    printf("EXODUS_FS result=%s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
