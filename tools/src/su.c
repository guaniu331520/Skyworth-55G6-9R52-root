/* su.c - minimal setuid-root shell for this TV.
 *
 * Installed as /system/xbin/su with mode 6755. /system is mounted WITHOUT
 * nosuid (see fstab.rtd299o: "/system ext4 ro wait"), so the setuid bit is
 * honoured and any app can obtain root by executing it.
 *
 * Supports:  su            -> root shell
 *            su -c "cmd"   -> run one command as root
 *            su <cmd> args -> exec command as root
 *
 * This is deliberately simple: it is a single-user home TV with no untrusted
 * apps. A full authorisation UI would be a separate app (see notes on Magisk).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char **argv) {
    /* A setuid-root execve gives euid=0 but leaves REAL uid = caller (2000).
     * execve() of a non-setuid binary then resets euid back to the real uid,
     * silently dropping root. Pin all three uids/gids to 0 before exec. */
    if (setresgid(0, 0, 0) < 0) {
        if (setgid(0) < 0) {
            fprintf(stderr, "su: setgid failed: %s\n", strerror(errno));
            return 1;
        }
    }
    if (setresuid(0, 0, 0) < 0) {
        if (setuid(0) < 0) {
            fprintf(stderr, "su: setuid failed: %s\n", strerror(errno));
            return 1;
        }
    }

    if (argc > 2 && !strcmp(argv[1], "-c")) {
        char *const av[] = { "/system/bin/sh", "-c", argv[2], NULL };
        execv("/system/bin/sh", av);
    } else if (argc > 1) {
        execvp(argv[1], &argv[1]);
    } else {
        char *const av[] = { "sh", "-", NULL };
        execv("/system/bin/sh", av);
    }
    fprintf(stderr, "su: exec failed: %s\n", strerror(errno));
    return 1;
}
