/* vecdump.c — dump high vector region 0xFFFF0000..0xFFFF3FFF
 * with SIGSEGV-safe page probing. Find vector_swi literal.
 * Compile: clang --target=armv7a-linux-androideabi21 --static -o vecdump vecdump.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

static sigjmp_buf g_jmp;
static volatile int g_fault;

static void sigsegv(int sig) {
    (void)sig;
    g_fault = 1;
    siglongjmp(g_jmp, 1);
}

static uint32_t safe_read_u32(const volatile uint32_t *p) {
    g_fault = 0;
    if (sigsetjmp(g_jmp, 1)) return 0xDEADF00D; /* fault */
    uint32_t v = *p;
    if (g_fault) return 0xDEADF00D;
    return v;
}

int main(void) {
    struct sigaction sa;
    sa.sa_handler = sigsegv;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);

    printf("[*] probing high vector region 0xFFFF0000 .. 0xFFFF3FFF\n");
    fflush(stdout);

    uint32_t base = 0xFFFF0000UL;
    for (uint32_t off = 0; off < 0x4000; off += 0x1000) {
        uint32_t addr = base + off;
        uint32_t v0 = safe_read_u32((volatile uint32_t *)addr);
        if (v0 == 0xDEADF00D) {
            printf("  page 0x%08x : UNREADABLE\n", addr);
            fflush(stdout);
            continue;
        }
        printf("  page 0x%08x : READABLE\n", addr);
        /* dump 1KB of this page, flag any value that looks like a kernel ptr */
        for (uint32_t o2 = 0; o2 < 0x400; o2 += 4) {
            uint32_t v = safe_read_u32((volatile uint32_t *)(addr + o2));
            if (v == 0xDEADF00D) continue;
            /* kernel text/data usually in 0x80000000..0xDFFFFFFF on 32-bit ARM */
            if (v >= 0x80000000UL && v <= 0xDFFFFFFFUL) {
                printf("    PTR@0x%08x = 0x%08x  <-- possible kernel address\n",
                       addr + o2, v);
                fflush(stdout);
            }
        }
    }
    printf("[*] done\n");
    return 0;
}
