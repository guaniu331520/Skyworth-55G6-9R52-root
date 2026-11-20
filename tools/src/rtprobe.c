/* rtprobe.c - READ-ONLY probe of /dev/rtkmem mmap semantics on Realtek RTD299x.
 *
 * Goal (step 1 of the zero-brick root route): determine whether mmap's offset
 * argument means a physical address or a page-frame number, and verify that the
 * running kernel's physical image is readable by an unprivileged shell process.
 *
 * This program NEVER writes to the device or to mapped memory. It only maps
 * PROT_READ and samples a few words with SIGSEGV/BUS guards. Worst case is a
 * process crash (kernel panic_on_oops is irrelevant here).
 *
 * Build: clang --target=armv7a-linux-androideabi19 --static -O2 -o rtprobe rtprobe.c
 * Then patch PT_TLS p_align to 64 (old Bionic requirement).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#define KCODE_PHYS   0x00108000UL   /* /proc/iomem: Kernel code start */
#define PROBE_LEN    0x1000UL

static sigjmp_buf g_jmp;

static void fault_handler(int sig) {
    (void)sig;
    siglongjmp(g_jmp, 1);
}

static void install_fault_guards(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* Returns 1 on success, 0 if the read faulted. */
static int safe_read_words(const volatile uint32_t *p, uint32_t *out, int n) {
    if (sigsetjmp(g_jmp, 1)) return 0;
    for (int i = 0; i < n; i++) out[i] = p[i];
    return 1;
}

static void try_map(int fd, const char *label, off_t offset, size_t length, int flags) {
    void *m = mmap(NULL, length, PROT_READ, flags, fd, offset);
    if (m == MAP_FAILED) {
        printf("  %-28s offset=0x%08lx len=0x%zx -> mmap FAILED errno=%d (%s)\n",
               label, (unsigned long)offset, length, errno, strerror(errno));
        fflush(stdout);
        return;
    }
    uint32_t w[8];
    int ok = safe_read_words((volatile uint32_t *)m, w, 8);
    printf("  %-28s offset=0x%08lx -> MAP OK @%p, read=%s\n",
           label, (unsigned long)offset, m, ok ? "OK" : "FAULT");
    if (ok) {
        printf("      words: %08x %08x %08x %08x %08x %08x %08x %08x\n",
               w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        /* ARM kernel text starts with a branch (0xEA0000xx) or stmia sp!,
         * and contains dense 0xE1/0xE3/0xE5/0xEA opcodes. */
        int armish = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t cond = (w[i] >> 28) & 0xF;
            if (cond >= 0xA && cond <= 0xF && w[i] != 0 && w[i] != 0xFFFFFFFF) armish++;
        }
        printf("      ARM-opcode-like words: %d/8 %s\n", armish,
               armish >= 5 ? "<-- looks like kernel text" : "");
    }
    munmap(m, length);
    fflush(stdout);
}

int main(void) {
    install_fault_guards();
    printf("[*] rtprobe: READ-ONLY /dev/rtkmem mmap semantics probe\n");
    printf("[*] uid=%d gid=%d  KCODE_PHYS=0x%08lx\n",
           (int)getuid(), (int)getgid(), (unsigned long)KCODE_PHYS);
    fflush(stdout);

    int fd = open("/dev/rtkmem", O_RDONLY);
    if (fd < 0) {
        printf("[!] open O_RDONLY failed errno=%d (%s), trying O_RDWR\n",
               errno, strerror(errno));
        fd = open("/dev/rtkmem", O_RDWR);
        if (fd < 0) {
            printf("[!] open O_RDWR failed errno=%d (%s) - ABORT\n",
                   errno, strerror(errno));
            return 2;
        }
    }
    printf("[*] fd=%d\n", fd);
    fflush(stdout);

    /* Hypothesis A: offset is a byte physical address (standard
     * remap_pfn_range uses vma->vm_pgoff, i.e. offset>>PAGE_SHIFT). */
    try_map(fd, "A phys-addr MAP_SHARED", (off_t)KCODE_PHYS, PROBE_LEN, MAP_SHARED);
    try_map(fd, "A phys-addr MAP_PRIVATE", (off_t)KCODE_PHYS, PROBE_LEN, MAP_PRIVATE);

    /* Hypothesis B: offset is already a pfn (driver does offset<<PAGE_SHIFT
     * itself, or expects pfn directly). */
    try_map(fd, "B pfn MAP_SHARED", (off_t)(KCODE_PHYS >> 12), PROBE_LEN, MAP_SHARED);
    try_map(fd, "B pfn<<12 MAP_SHARED", (off_t)((KCODE_PHYS >> 12) << 12), PROBE_LEN, MAP_SHARED);

    /* Hypothesis C: offset 0 maps the whole physical RAM from 0. */
    try_map(fd, "C offset0 MAP_SHARED", (off_t)0, PROBE_LEN, MAP_SHARED);

    /* Also probe a couple of other known physical ranges as cross-checks. */
    try_map(fd, "D kdata phys MAP_SHARED", (off_t)0x00c7a000UL, PROBE_LEN, MAP_SHARED);
    try_map(fd, "E 0x100000 MAP_SHARED", (off_t)0x00100000UL, PROBE_LEN, MAP_SHARED);

    close(fd);
    printf("[*] done (no writes performed)\n");
    return 0;
}
