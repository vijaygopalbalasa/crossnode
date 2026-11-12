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
 *   cnprobe now                            print this machine's monotonic clock in ms
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hello_dcmd.h"

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
