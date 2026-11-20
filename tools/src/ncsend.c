/* Minimal stdin -> TCP sender for fast eMMC streaming off the TV.
 * Usage: dd if=/dev/block/X | busybox lzop -1 | ncsend <PC_IP> <PORT>
 * No writes to any partition. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ncsend <host> <port>\n");
        return 1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 2; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(argv[2]));
    a.sin_addr.s_addr = inet_addr(argv[1]);
    if (a.sin_addr.s_addr == INADDR_NONE) { fprintf(stderr, "bad ip\n"); return 3; }
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("connect"); return 4;
    }
    static char buf[262144];
    ssize_t n, total = 0;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(s, buf + off, (size_t)(n - off));
            if (w <= 0) { perror("write"); close(s); return 5; }
            off += w;
        }
        total += n;
    }
    if (n < 0) { perror("read"); close(s); return 6; }
    fprintf(stderr, "ncsend: sent %lld bytes\n", (long long)total);
    close(s);
    return 0;
}
