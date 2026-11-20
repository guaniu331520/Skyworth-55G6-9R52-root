/* rtdump.c - READ-ONLY physical-memory dumper via /dev/rtkmem mmap.
 *
 * Step 2 of the zero-brick root route. Maps physical RAM in 1 MiB chunks with
 * PROT_READ|MAP_SHARED and writes to a file. Each chunk is guarded by
 * SIGSEGV/SIGBUS so an unreadable page becomes zero-filled instead of a crash.
 *
 * Build: clang --target=armv7a-linux-androideabi19 --static -O2 -o rtdump rtdump.c
 * PT_TLS p_align must be patched to 64 for the old Bionic linker.
 *
 * Usage: rtdump <outfile> <start_hex> <end_hex>
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

#define CHUNK (1u << 20)

static sigjmp_buf g_jmp;
static void fault_handler(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: %s <outfile> <start_hex> <end_hex>\n", argv[0]);
        return 2;
    }
    const char *out = argv[1];
    uint64_t start = strtoull(argv[2], NULL, 16);
    uint64_t end   = strtoull(argv[3], NULL, 16);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    int fd = open("/dev/rtkmem", O_RDONLY);
    if (fd < 0) { printf("open rtkmem: %s\n", strerror(errno)); return 2; }
    FILE *f = fopen(out, "wb");
    if (!f) { printf("fopen %s: %s\n", out, strerror(errno)); return 2; }

    unsigned char *buf = malloc(CHUNK);
    if (!buf) { printf("malloc failed\n"); return 2; }

    printf("[*] dumping phys 0x%llx..0x%llx -> %s (chunk %u bytes)\n",
           (unsigned long long)start, (unsigned long long)end, out, CHUNK);
    fflush(stdout);

    uint64_t total = 0, faults = 0;
    for (uint64_t p = start; p < end; p += CHUNK) {
        size_t len = CHUNK;
        if (p + len > end) len = (size_t)(end - p);
        memset(buf, 0, len);
        void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)p);
        if (m != MAP_FAILED) {
            if (sigsetjmp(g_jmp, 1) == 0) {
                memcpy(buf, m, len);
            } else {
                faults++;
                memset(buf, 0, len);
            }
            munmap(m, len);
        } else {
            faults++;
        }
        fwrite(buf, 1, len, f);
        total += len;
        if ((total / CHUNK) % 4 == 0) {
            printf("    %llu MiB written (faults=%llu)\n",
                   (unsigned long long)(total >> 20), (unsigned long long)faults);
            fflush(stdout);
        }
    }
    fclose(f);
    free(buf);
    close(fd);
    printf("[*] done: %llu bytes, unreadable chunks=%llu\n",
           (unsigned long long)total, (unsigned long long)faults);
    return 0;
}
