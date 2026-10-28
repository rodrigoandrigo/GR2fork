#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
extern int sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
extern int sceKernelMprotect(const void*, size_t, int);
extern int sceKernelMunmap(void*, size_t);
extern int sceKernelReleaseDirectMemory(int64_t, size_t);
extern int sceKernelUsleep(unsigned);

int main(void) {
    const size_t size = 16u << 20;
    int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 65536, 0,
                                     &physical))
        return 1;
    void* first = 0;
    void* second = 0;
    int failed = sceKernelMapDirectMemory(&first, size, 3, 0, physical, 65536) != 0;
    if (!failed)
        failed = sceKernelMapDirectMemory(&second, size, 3, 0, physical, 65536) != 0;
    if (!failed) {
        volatile uint64_t* writer = first;
        volatile uint64_t* reader = second;
        for (size_t offset = 0; offset < size; offset += 65536)
            writer[offset / 8] = 0x1234567800000000ull + offset;
        failed = sceKernelMprotect((char*)second + 65536, 65536, 1) != 0;
        for (size_t offset = 0; offset < size; offset += 65536)
            if (reader[offset / 8] != 0x1234567800000000ull + offset)
                failed = 1;
        if (sceKernelMprotect((char*)second + 65536, 65536, 3))
            failed = 1;
        else {
            reader[65536 / 8] = 0xaabbccdd;
            if (writer[65536 / 8] != 0xaabbccdd)
                failed = 1;
        }
        uint8_t copied[16384];
        volatile uint8_t* pattern = (volatile uint8_t*)first + 0x7e8000;
        for (unsigned i = 0; i < sizeof(copied); ++i)
            pattern[i] = (uint8_t)(i ^ 0xa5);
        memcpy(copied, (char*)second + 0x7e8000, sizeof(copied));
        for (unsigned i = 0; i < sizeof(copied); ++i)
            if (copied[i] != (uint8_t)(i ^ 0xa5))
                failed = 1;
        FILE* io = fopen("/data/ram-alias-windows.bin", "wb+");
        if (!io) {
            failed = 1;
        } else {
            const size_t transfer = sizeof(copied);
            if (fwrite((char*)second + 0x7e8000, 1, transfer, io) != transfer || fflush(io))
                failed = 1;
            memset((char*)first + 0x7e8000, 0, transfer);
            sceKernelUsleep(1500000);
            if (fseek(io, 0, SEEK_SET) ||
                fread((char*)first + 0x7e8000, 1, transfer, io) != transfer)
                failed = 1;
            for (unsigned i = 0; i < transfer; ++i)
                if (((volatile uint8_t*)second)[0x7e8000 + i] != (uint8_t)(i ^ 0xa5))
                    failed = 1;
            if (fclose(io))
                failed = 1;
            remove("/data/ram-alias-windows.bin");
        }
        volatile uint8_t* code = (volatile uint8_t*)first + 4096;
        code[0] = 0xb8;
        code[1] = 42;
        code[2] = code[3] = code[4] = 0;
        code[5] = 0xc3;
        if (sceKernelMprotect(second, 65536, 5))
            failed = 1;
        else if (((int (*)(void))((char*)second + 4096))() != 42)
            failed = 1;
        if (sceKernelMprotect(second, 65536, 3))
            failed = 1;
        else {
            ((volatile uint8_t*)second)[4097] = 43;
            if (code[1] != 43)
                failed = 1;
        }
        void* aliases[64] = {0};
        for (unsigned i = 0; i < 64; ++i) {
            if (sceKernelMapDirectMemory(&aliases[i], size, 3, 0, physical, 65536)) {
                failed = 1;
                break;
            }
            ((volatile uint64_t*)aliases[i])[(i + 1) * 65536 / 8] =
                0xabc0000000000000ull + i;
        }
        for (unsigned i = 0; i < 64; ++i) {
            if (aliases[i]) {
                if (((volatile uint64_t*)aliases[i])[65536 / 8] != 0xabc0000000000000ull)
                    failed = 1;
                sceKernelMunmap(aliases[i], size);
            }
        }
        writer[(1u << 20) / 8] = 0x1234567800000000ull + (1u << 20);
        if (sceKernelMunmap((char*)second + (1u << 20), 65536))
            failed = 1;
        else {
            void* replacement = (char*)second + (1u << 20);
            if (sceKernelMapDirectMemory(&replacement, 65536, 3, 0x10,
                                         physical + (1u << 20), 65536) ||
                replacement != (char*)second + (1u << 20))
                failed = 1;
            else if (*(volatile uint64_t*)replacement !=
                     0x1234567800000000ull + (1u << 20))
                failed = 1;
        }
    }
    if (second)
        sceKernelMunmap(second, size);
    if (first)
        sceKernelMunmap(first, size);
    if (sceKernelReleaseDirectMemory(physical, size))
        failed = 1;
    printf("RAM_ALIAS_WINDOWS result=%s\n", failed ? "FAIL" : "PASS");
    return failed;
}
