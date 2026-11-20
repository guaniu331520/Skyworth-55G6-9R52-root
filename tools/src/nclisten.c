/* stdin -> TCP server (device listens, PC connects).
 * Usage: dd if=/dev/block/X | busybox lzop -1 | nclisten <PORT>
 * Needed because this LAN blocks device->PC connects but allows PC->device. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nclisten <port>\n"); return 1; }
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 2; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(argv[1]));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 3; }
    if (listen(srv, 1) < 0) { perror("listen"); return 4; }
    fprintf(stderr, "nclisten: waiting on port %s\n", argv[1]);
    int c = accept(srv, NULL, NULL);
    if (c < 0) { perror("accept"); return 5; }
    close(srv);
    fprintf(stderr, "nclisten: client connected\n");
    static char buf[262144];
    ssize_t n, total = 0;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(c, buf + off, (size_t)(n - off));
            if (w <= 0) { perror("write"); close(c); return 6; }
            off += w;
        }
        total += n;
    }
    if (n < 0) perror("read");
    fprintf(stderr, "nclisten: sent %lld bytes\n", (long long)total);
    close(c);
    return 0;
}
