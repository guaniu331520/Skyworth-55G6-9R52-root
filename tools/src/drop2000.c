/* test helper: spawn a process with AID_SHELL cred so the daemon's scanner
 * can be exercised without rebooting. Not installed on the TV. */
#include <unistd.h>
int main(int argc, char **argv) {
    setgid(2000);
    setuid(2000);
    if (argc > 1) execvp(argv[1], &argv[1]);
    char *const a[] = { "sh", "-", NULL };
    execv("/system/bin/sh", a);
    return 1;
}
