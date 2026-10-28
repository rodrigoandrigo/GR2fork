#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ucontext.h>

static volatile sig_atomic_t inner_count;
static volatile sig_atomic_t outer_count;

static void inner_handler(int signal, siginfo_t* info, void* context) {
    (void)signal;
    (void)info;
    (void)context;
    ++inner_count;
}

static void outer_handler(int signal, siginfo_t* info, void* raw_context) {
    (void)signal;
    (void)info;
    ucontext_t* context = raw_context;
    ++outer_count;
    if (outer_count > 10000)
        _Exit(2);
    raise(SIGUSR1);
    context->uc_mcontext.mc_rax = 0x12345678;
    context->uc_mcontext.mc_rip += 2;
}

int main(void) {
    struct sigaction inner = {0}, outer = {0};
    inner.sa_sigaction = inner_handler;
    outer.sa_sigaction = outer_handler;
    inner.sa_flags = outer.sa_flags = SA_SIGINFO;
    sigemptyset(&inner.sa_mask);
    sigemptyset(&outer.sa_mask);
    if (sigaction(SIGUSR1, &inner, 0) || sigaction(SIGILL, &outer, 0))
        return 1;
    for (unsigned i = 0; i < 10000; ++i) {
        uint64_t value;
        __asm__ volatile("xor %%eax, %%eax; ud2" : "=a"(value) : : "memory", "cc");
        if (value != 0x12345678)
            return 3;
    }
    int failed = inner_count != 10000 || outer_count != 10000;
    printf("NESTED_SIGNALS outer=%d inner=%d result=%s\n", outer_count, inner_count,
           failed ? "FAIL" : "PASS");
    return failed;
}
