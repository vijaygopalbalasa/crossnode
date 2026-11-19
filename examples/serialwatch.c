/*
 * serialwatch: print each line that arrives on a serial port, the way a QNX 7 program written for Qnet would watch
 * another machine's port: plain POSIX, no Crossnode code. Give it /net/<node>/dev/serN and it runs unchanged.
 *
 *   serialwatch PORT [BAUD] [SECONDS]
 *
 * Sets the port to BAUD (default: leave it), then waits with select() for up to SECONDS (default 30) of silence,
 * printing every line as "PORT: line". Exits 0 when the silence limit passes, 1 on an error (printed).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static speed_t to_speed(long b)
{
    switch (b) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: serialwatch PORT [BAUD] [SECONDS]\n");
        return 2;
    }
    const char *port = argv[1];
    long baud = argc > 2 ? atol(argv[2]) : 0;
    long secs = argc > 3 ? atol(argv[3]) : 30;
    int fd = open(port, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "serialwatch: %s: %s\n", port, strerror(errno));
        return 1;
    }
    if (baud > 0) {
        struct termios t;
        speed_t sp = to_speed(baud);
        if (sp == 0 || tcgetattr(fd, &t) != 0 || cfsetispeed(&t, sp) != 0 || cfsetospeed(&t, sp) != 0 ||
            tcsetattr(fd, TCSANOW, &t) != 0) {
            fprintf(stderr, "serialwatch: %s: cannot set %ld baud: %s\n", port, baud, strerror(errno));
            return 1;
        }
        printf("%s: set to %ld baud; waiting for lines\n", port, baud);
    } else {
        printf("%s: waiting for lines\n", port);
    }
    fflush(stdout);
    char line[512];
    size_t used = 0;
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
        int n = select(fd + 1, &r, NULL, NULL, &tv);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            fprintf(stderr, "serialwatch: %s: select: %s\n", port, strerror(errno));
            return 1;
        }
        if (n == 0) {
            printf("%s: quiet for %ld s, done\n", port, secs);
            return 0;
        }
        char buf[256];
        ssize_t k = read(fd, buf, sizeof buf);
        if (k < 0) {
            fprintf(stderr, "serialwatch: %s: read: %s\n", port, strerror(errno));
            return 1;
        }
        if (k == 0) {
            printf("%s: closed\n", port);
            return 0;
        }
        for (ssize_t i = 0; i < k; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (used > 0) {
                    line[used] = '\0';
                    printf("%s: %s\n", port, line);
                    fflush(stdout);
                    used = 0;
                }
            } else if (used < sizeof line - 1) {
                line[used++] = buf[i];
            }
        }
    }
}
