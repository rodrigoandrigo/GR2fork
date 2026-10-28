#include <stdint.h>
#include <stdio.h>

extern int sceKernelLoadStartModule(const char*, uint64_t, const void*, unsigned, const void*, int*);
extern int sceKernelDlsym(int, const char*, void**);
extern int scePthreadCreate(void**, const void*, void* (*)(void*), void*, const char*);
extern int scePthreadJoin(void*, void**);

static int (*acquire_guard)(uint64_t*);
static void (*release_guard)(uint64_t*);
static void (*abort_guard)(uint64_t*);
static uint64_t shared_guard;
static unsigned constructors;
static unsigned value;

static void* initialize(void* unused) {
    (void)unused;
    if (acquire_guard(&shared_guard)) {
        ++constructors;
        value = 0x12345678;
        release_guard(&shared_guard);
    }
    return (void*)(uintptr_t)(value != 0x12345678);
}

int main(void) {
    int started = 0;
    int module = sceKernelLoadStartModule("/system/common/lib/libSceLibcInternal.sprx", 0, 0, 0, 0,
                                          &started);
    if (module < 0 || sceKernelDlsym(module, "__cxa_guard_acquire", (void**)&acquire_guard) ||
        sceKernelDlsym(module, "__cxa_guard_release", (void**)&release_guard) ||
        sceKernelDlsym(module, "__cxa_guard_abort", (void**)&abort_guard))
        return 1;
    uint64_t guard = 0;
    if (acquire_guard(&guard) != 1 || (guard & 0xff))
        return 2;
    abort_guard(&guard);
    if ((guard & 0xff) || acquire_guard(&guard) != 1)
        return 3;
    release_guard(&guard);
    if (!(guard & 0xff) || acquire_guard(&guard) != 0)
        return 4;
    void* threads[8];
    unsigned created = 0;
    for (; created < 8; ++created)
        if (scePthreadCreate(&threads[created], 0, initialize, 0, "libc-guard"))
            break;
    int failed = created != 8;
    for (unsigned i = 0; i < created; ++i) {
        void* result = 0;
        if (scePthreadJoin(threads[i], &result) || result)
            failed = 1;
    }
    printf("LIBC_GUARDS constructors=%u value=%08x result=%s\n", constructors, value,
           !failed && constructors == 1 ? "PASS" : "FAIL");
    return failed || constructors != 1;
}
