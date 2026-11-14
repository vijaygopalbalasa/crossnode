/*
 * cnprobe: uses a path the way an application would and prints one line of key=value results per
 * step. Times are wall-clock milliseconds on this machine; they show behaviour bounds, not speed.
 *
 *   cnprobe get PATH | set PATH N | add PATH N
 *   cnprobe watch PATH COUNT PERIOD_MS     devctl GET in a loop, stops at the first error
 *   cnprobe open PATH                      time one open
 *   cnprobe stale PATH WAIT_MS             open, GET, wait, GET again, then time close
 *   cnprobe eintr PATH ALARM_MS [WAIT_MS]  open, wait, then GET with a signal arriving after ALARM_MS
 *   cnprobe exit PATH [WAIT_MS]            a child opens PATH, waits, exits without closing; time the exit
 *   cnprobe eintr-open PATH ALARM_MS        open with a signal arriving after ALARM_MS
 *   cnprobe seek PATH                       lseek on an open fd
 *   cnprobe readn PATH N                    one read of N bytes; prints the count returned
 *   cnprobe conns ADDRESS PORT N            open N TCP connections, report how many stay open
 *   cnprobe stat PATH | fstat PATH         stat() a path, or open it and fstat() the descriptor
 *   cnprobe ls PATH | lsr PATH             list a directory (lsr: list, rewinddir, list again)
 *   cnprobe eintr-stat PATH ALARM_MS        stat() with a signal arriving after ALARM_MS
 *   cnprobe rawping ADDRESS PORT            send an unauthenticated PING; report whether it was answered and whether the agent hung up
 *   cnprobe trickle ADDRESS PORT N          open N connections that send one byte every 1.5 s; report how long each was held
 *   cnprobe now                            print this machine's monotonic clock in ms
 */
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "hello_dcmd.h"
#include "../src/cn_proto.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static const char *ename(int e)
{
    switch (e) {
    case EOK:          return "EOK";
    case EHOSTDOWN:    return "EHOSTDOWN";
    case ETIMEDOUT:    return "ETIMEDOUT";
    case ESTALE:       return "ESTALE";
    case EINTR:        return "EINTR";
    case ENOENT:       return "ENOENT";
    case EACCES:       return "EACCES";
    case EBADF:        return "EBADF";
    case ENOTTY:       return "ENOTTY";
    case EINVAL:       return "EINVAL";
    case ENOSYS:       return "ENOSYS";
    case EMSGSIZE:     return "EMSGSIZE";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EAGAIN:       return "EAGAIN";
    case ENXIO:        return "ENXIO";
    case ESPIPE:       return "ESPIPE";
    case ENOTDIR:      return "ENOTDIR";
    default:           return "EOTHER";
    }
}

static void pause_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static int open_or_report(const char *path, int flags)
{
    double t0 = now_ms();
    int fd = open(path, flags);
    if (fd < 0) {
        int e = errno;
        printf("op=open rc=%s errno=%d ms=%.0f\n", ename(e), e, now_ms() - t0);
    }
    return fd;
}

static int do_get(int fd, int *v, int *info)
{
    *v = 0;
    *info = -1;
    return devctl(fd, DCMD_HELLO_GET, v, sizeof *v, info);
}

static void on_alarm(int sig)
{
    (void)sig;
}

static int usage(void)
{
    fprintf(stderr, "usage: cnprobe get|set|add|watch|open|stale|eintr|exit PATH [ARGS]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "now") == 0) {
        printf("%.0f\n", now_ms());
        return 0;
    }
    if (argc < 3)
        return usage();
    const char *cmd = argv[1], *path = argv[2];
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (strcmp(cmd, "get") == 0 || strcmp(cmd, "set") == 0 || strcmp(cmd, "add") == 0) {
        int fd = open_or_report(path, O_RDWR);
        if (fd < 0)
            return 0;
        int v = argc > 3 ? atoi(argv[3]) : 0, info = -1, rc;
        double t0 = now_ms();
        if (cmd[0] == 'g')
            rc = do_get(fd, &v, &info);
        else if (cmd[0] == 's')
            rc = devctl(fd, DCMD_HELLO_SET, &v, sizeof v, &info);
        else
            rc = devctl(fd, DCMD_HELLO_ADD, &v, sizeof v, &info);
        printf("op=%s rc=%s info=%d value=%d ms=%.0f\n", cmd, ename(rc), info, v, now_ms() - t0);
        close(fd);
        return 0;
    }
    if (strcmp(cmd, "watch") == 0) {
        if (argc < 5)
            return usage();
        int count = atoi(argv[3]);
        long period = atol(argv[4]);
        int fd = open_or_report(path, O_RDONLY);
        if (fd < 0)
            return 0;
        for (int i = 0; i < count; i++) {
            int v, info;
            double t0 = now_ms();
            int rc = do_get(fd, &v, &info);
            double ms = now_ms() - t0;
            printf("op=watch i=%d rc=%s value=%d ms=%.0f\n", i, ename(rc), v, ms);
            if (rc != EOK) {
                printf("result=stopped rc=%s call_ms=%.0f t_end=%.0f\n", ename(rc), ms, now_ms());
                break;
            }
            pause_ms(period);
        }
        close(fd);
        return 0;
    }
    if (strcmp(cmd, "open") == 0) {
        double t0 = now_ms();
        int fd = open(path, O_RDONLY);
        int e = fd < 0 ? errno : EOK;
        printf("op=open rc=%s ms=%.0f\n", ename(e), now_ms() - t0);
        if (fd >= 0)
            close(fd);
        return 0;
    }
    if (strcmp(cmd, "stale") == 0) {
        if (argc < 4)
            return usage();
        int fd = open_or_report(path, O_RDONLY);
        if (fd < 0)
            return 0;
        int v, info;
        int rc = do_get(fd, &v, &info);
        printf("op=get-before rc=%s value=%d\n", ename(rc), v);
        pause_ms(atol(argv[3]));
        double t0 = now_ms();
        rc = do_get(fd, &v, &info);
        printf("op=get-after rc=%s ms=%.0f\n", ename(rc), now_ms() - t0);
        t0 = now_ms();
        int crc = close(fd);
        printf("op=close rc=%s ms=%.0f\n", crc == 0 ? "EOK" : ename(errno), now_ms() - t0);
        return 0;
    }
    if (strcmp(cmd, "eintr") == 0) {
        if (argc < 4)
            return usage();
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_alarm;       /* no SA_RESTART: the call must come back with EINTR */
        sigaction(SIGALRM, &sa, NULL);
        int fd = open_or_report(path, O_RDONLY);
        if (fd < 0)
            return 0;
        long ms = atol(argv[3]);
        if (argc > 4)
            pause_ms(atol(argv[4]));
        struct itimerval it = { { 0, 0 }, { ms / 1000, (ms % 1000) * 1000 } };
        int v, info;
        double t0 = now_ms();
        setitimer(ITIMER_REAL, &it, NULL);
        int rc = do_get(fd, &v, &info);
        printf("op=get-with-signal rc=%s ms=%.0f\n", ename(rc), now_ms() - t0);
        close(fd);
        return 0;
    }
    if (strcmp(cmd, "eintr-open") == 0) {
        if (argc < 4)
            return usage();
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
        long ms = atol(argv[3]);
        struct itimerval it = { { 0, 0 }, { ms / 1000, (ms % 1000) * 1000 } };
        double t0 = now_ms();
        setitimer(ITIMER_REAL, &it, NULL);
        int fd = open(path, O_RDONLY);
        int e = fd < 0 ? errno : EOK;
        printf("op=open-with-signal rc=%s ms=%.0f\n", ename(e), now_ms() - t0);
        if (fd >= 0)
            close(fd);
        return 0;
    }
    if (strcmp(cmd, "stat") == 0 || strcmp(cmd, "fstat") == 0) {
        struct stat st;
        double t0 = now_ms();
        int rc, fd = -1;
        if (cmd[0] == 'f') {
            fd = open(path, O_RDONLY);
            if (fd < 0) {
                printf("op=%s rc=%s ms=%.0f\n", cmd, ename(errno), now_ms() - t0);
                return 0;
            }
            rc = fstat(fd, &st);
        } else {
            rc = stat(path, &st);
        }
        int e = rc != 0 ? errno : EOK;
        if (rc != 0)
            printf("op=%s rc=%s ms=%.0f\n", cmd, ename(e), now_ms() - t0);
        else
            printf("op=%s rc=EOK mode=%o size=%lld uid=%d gid=%d nlink=%d mtime=%lld rdev=%d ms=%.0f\n", cmd,
                   (unsigned)st.st_mode, (long long)st.st_size, (int)st.st_uid, (int)st.st_gid,
                   (int)st.st_nlink, (long long)st.st_mtime, (int)st.st_rdev, now_ms() - t0);
        if (fd >= 0)
            close(fd);
        return 0;
    }
    if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "lsr") == 0) {
        double t0 = now_ms();
        DIR *d = opendir(path);
        if (d == NULL) {
            printf("op=%s rc=%s ms=%.0f\n", cmd, ename(errno), now_ms() - t0);
            return 0;
        }
        for (int pass = 0; pass < (cmd[2] == 'r' ? 2 : 1); pass++) {
            int n = 0;
            struct dirent *de;
            errno = 0;
            while ((de = readdir(d)) != NULL) {
                printf("ent=%s\n", de->d_name);
                n++;
            }
            printf("op=%s pass=%d rc=%s n=%d ms=%.0f\n", cmd, pass, errno ? ename(errno) : "EOK", n, now_ms() - t0);
            rewinddir(d);
        }
        closedir(d);
        return 0;
    }
    if (strcmp(cmd, "eintr-stat") == 0) {
        if (argc < 4)
            return usage();
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
        long ms = atol(argv[3]);
        struct itimerval it = { { 0, 0 }, { ms / 1000, (ms % 1000) * 1000 } };
        struct stat st;
        double t0 = now_ms();
        setitimer(ITIMER_REAL, &it, NULL);
        int rc = stat(path, &st);
        printf("op=stat-with-signal rc=%s ms=%.0f\n", rc == 0 ? "EOK" : ename(errno), now_ms() - t0);
        return 0;
    }
    if (strcmp(cmd, "rawping") == 0) {
        if (argc < 4)
            return usage();
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)atoi(argv[3]));
        inet_pton(AF_INET, path, &sa.sin_addr);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
            printf("op=rawping connect=failed\n");
            return 0;
        }
        struct timeval tv = { 3, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct cn_hdr h;
        memset(&h, 0, sizeof h);
        h.magic = CN_MAGIC;
        h.op = CN_OP_PING;
        h.seq = 1;
        h.handle = -1;
        (void)send(s, &h, sizeof h, 0);
        unsigned char buf[4096];
        size_t got = 0;
        int closed = 0, replied = 0;
        for (;;) {
            ssize_t r = recv(s, buf + got, sizeof buf - got, 0);
            if (r == 0) {
                closed = 1;
                break;
            }
            if (r < 0)
                break;
            got += (size_t)r;
        }
        size_t off = 0;
        while (got - off >= sizeof h) {
            memcpy(&h, buf + off, sizeof h);
            if (h.op == (CN_OP_PING | CN_REPLY))
                replied = 1;
            off += sizeof h + h.len;
        }
        printf("op=rawping ping_reply=%s closed=%s\n", replied ? "yes" : "no", closed ? "yes" : "no");
        return 0;
    }
    if (strcmp(cmd, "trickle") == 0) {
        if (argc < 5)
            return usage();
        int n = atoi(argv[4]);
        if (n > 16)
            n = 16;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)atoi(argv[3]));
        inet_pton(AF_INET, path, &sa.sin_addr);
        int fds[16];
        double closed_at[16], t0 = now_ms(), last_send = t0;
        for (int i = 0; i < n; i++) {
            fds[i] = socket(AF_INET, SOCK_STREAM, 0);
            if (fds[i] >= 0 && connect(fds[i], (struct sockaddr *)&sa, sizeof sa) != 0) {
                close(fds[i]);
                fds[i] = -1;
            }
            closed_at[i] = fds[i] < 0 ? 0 : -1;
        }
        while (now_ms() - t0 < 6000) {
            struct pollfd pf[16];
            for (int i = 0; i < n; i++) {
                pf[i].fd = closed_at[i] < 0 ? fds[i] : -1;
                pf[i].events = POLLIN;
                pf[i].revents = 0;
            }
            (void)poll(pf, (nfds_t)n, 100);
            for (int i = 0; i < n; i++) {
                if (closed_at[i] >= 0 || !(pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                char b[256];
                ssize_t r = recv(fds[i], b, sizeof b, 0);
                if (r <= 0)
                    closed_at[i] = now_ms() - t0;
            }
            if (now_ms() - last_send >= 1500) {
                last_send = now_ms();
                for (int i = 0; i < n; i++)
                    if (closed_at[i] < 0)
                        (void)send(fds[i], "x", 1, MSG_NOSIGNAL);
            }
            int open_left = 0;
            for (int i = 0; i < n; i++)
                if (closed_at[i] < 0)
                    open_left++;
            if (!open_left)
                break;
        }
        int instant = 0, held = 0, never = 0;
        double hmax = 0;
        for (int i = 0; i < n; i++) {
            if (closed_at[i] < 0)
                never++;
            else if (closed_at[i] < 300)
                instant++;
            else {
                held++;
                if (closed_at[i] > hmax)
                    hmax = closed_at[i];
            }
        }
        printf("op=trickle tried=%d instant=%d held=%d never=%d held_max_ms=%.0f\n", n, instant, held, never, hmax);
        return 0;
    }
    if (strcmp(cmd, "seek") == 0) {
        int fd = open_or_report(path, O_RDONLY);
        if (fd < 0)
            return 0;
        off_t r = lseek(fd, 0, SEEK_SET);
        printf("op=seek rc=%s\n", r == (off_t)-1 ? ename(errno) : "EOK");
        close(fd);
        return 0;
    }
    if (strcmp(cmd, "readn") == 0) {
        if (argc < 4)
            return usage();
        size_t n = (size_t)atol(argv[3]);
        char *buf = malloc(n + 1);
        int fd = buf ? open_or_report(path, O_RDONLY) : -1;
        if (fd < 0)
            return 0;
        ssize_t r = read(fd, buf, n);
        printf("op=readn asked=%zu rc=%s got=%zd\n", n, r < 0 ? ename(errno) : "EOK", r);
        close(fd);
        return 0;
    }
    if (strcmp(cmd, "conns") == 0) {
        if (argc < 5)
            return usage();
        int n = atoi(argv[4]), open_ok = 0, fds[64];
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)atoi(argv[3]));
        inet_pton(AF_INET, path, &sa.sin_addr);
        if (n > 64)
            n = 64;
        for (int i = 0; i < n; i++) {
            fds[i] = socket(AF_INET, SOCK_STREAM, 0);
            if (fds[i] >= 0 && connect(fds[i], (struct sockaddr *)&sa, sizeof sa) != 0) {
                close(fds[i]);
                fds[i] = -1;
            }
        }
        pause_ms(500);
        for (int i = 0; i < n; i++) {
            char b;
            if (fds[i] < 0)
                continue;
            ssize_t r = recv(fds[i], &b, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r > 0 || (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
                open_ok++;              /* still open: the agent kept it (a keyed agent has sent its challenge) */
            close(fds[i]);
        }
        printf("op=conns tried=%d kept=%d\n", n, open_ok);
        return 0;
    }
    if (strcmp(cmd, "exit") == 0) {
        int p[2];
        if (pipe(p) != 0)
            return 1;
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            int fd = open(path, O_RDONLY);
            char ok = fd >= 0 ? 'y' : 'n';
            (void)write(p[1], &ok, 1);
            if (argc > 3)
                pause_ms(atol(argv[3]));
            (void)write(p[1], &ok, 1);  /* about to exit */
            _exit(0);                   /* the fd is left for the system to close */
        }
        close(p[1]);
        char ok = 'n', bye;
        (void)read(p[0], &ok, 1);
        (void)read(p[0], &bye, 1);
        double t0 = now_ms();
        int status;
        waitpid(pid, &status, 0);
        printf("op=exit-with-open-fd opened=%c ms=%.0f\n", ok, now_ms() - t0);
        return 0;
    }
    return usage();
}
