/* rtperm.c - READ-ONLY capability probe: can /dev/rtkmem be mapped writable?
 *
 * Opens PROT_READ|PROT_WRITE mappings but NEVER stores a single byte.
 * This only answers whether the driver's mmap would permit a later write,
 * so the exploit plan can be finalised before any write is attempted.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void) {
    printf("[*] rtperm: capability probe (NO writes performed)\n");
    int fd = open("/dev/rtkmem", O_RDWR);
    printf("    open O_RDWR -> %d (%s)\n", fd, fd < 0 ? strerror(errno) : "ok");
    if (fd < 0) return 2;
    fflush(stdout);

    off_t offs[] = { 0x00108000, 0x00c7a000, 0x00000000 };
    for (int i = 0; i < 3; i++) {
        void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offs[i]);
        if (m == MAP_FAILED) {
            printf("    RW mmap 0x%08lx -> FAILED errno=%d (%s)\n",
                   (unsigned long)offs[i], errno, strerror(errno));
        } else {
            /* Deliberately do not dereference/write. Just report. */
            printf("    RW mmap 0x%08lx -> OK @%p (mapping released, nothing written)\n",
                   (unsigned long)offs[i], m);
            munmap(m, 0x1000);
        }
        fflush(stdout);
    }
    close(fd);
    printf("[*] done (zero bytes written)\n");
    return 0;
}
