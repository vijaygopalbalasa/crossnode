/*
 * crossnode-agent: serves this machine's exported paths to Crossnode peers.
 *
 * One reader thread per peer connection answers PING at once and hands every other request to a
 * worker thread of its own. A request that blocks on a slow device therefore never delays the
 * liveness replies a peer uses to tell "slow" from "dead".
 *
 * A worker's device call is interrupted with a signal (EINTR) when the peer cancels the request
 * (its caller got a signal), when the peer closes the handle, and when the connection ends, so a
 * read that would never return cannot pin a job, a connection or a peer slot. The signal is sent
 * again until the worker is done, which closes the window where it lands just before the call.
 * (pthread_cancel is not used: on QNX 8.0 a cancelled thread is ended again at the first
 * cancellation point inside its cleanup handler, so the handler cannot release anything.)
 *
 * Every open is resolved with realpath() and must land inside an export; then the descriptor itself
 * is asked where it points (iofdinfo), so swapping a path component between the check and the open
 * gains nothing. Names outside the share all answer EACCES, so a peer cannot probe what exists there.
 * Each operation works on its own dup() of the descriptor, so a close racing with it cannot
 * redirect it to a file opened later. In-flight requests, connections, and connections per peer
 * address are capped, and every refusal is logged.
 *
 * Gate 1 limit, stated plainly: no peer authentication yet, so bind it to a private link address.
 */
#include <arpa/inet.h>
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iomgr.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "cn_proto.h"

#define MAX_EXPORTS        32
#define MAX_JOBS           64     /* requests in flight per connection */
#define MAX_CONNS          16     /* peer connections at once */
#define MAX_PER_ADDR       4      /* connections at once from one address */
#define PEER_SILENCE_MS    5000   /* drop a peer that sends nothing, not even PING, for this long */

static char *exports[MAX_EXPORTS];      /* as given, without the leading '/' */
static char *rexports[MAX_EXPORTS];     /* the same, resolved with realpath() at start */
static int nexports;
static int verbose;
static pthread_mutex_t conns_mtx = PTHREAD_MUTEX_INITIALIZER;   /* guards the slot table */
static struct in_addr slot_addr[MAX_CONNS];
static int slot_used[MAX_CONNS];

struct jobslot {
    int used;
    uint32_t seq;
    int32_t handle;
    int cancel;               /* cancellation asked for */
    int tid_ok;
    pthread_t tid;
};

struct conn {
    int sock;
    pthread_mutex_t mtx;      /* guards everything below and every write to sock */
    int dead;
    int refs;
    int jobs;
    int slot;                 /* index in the slot table */
    int fds[CN_MAX_HANDLES];
    struct jobslot js[MAX_JOBS];
    char peer[64];
};

struct job {
    struct conn *c;
    int js;                   /* index in c->js */
    struct cn_hdr h;
    unsigned char *payload;
    unsigned char *buf;
    int fd;                   /* this job's private dup, -1 if none */
};

static void vlog(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define CN_INTERRUPT SIGUSR2   /* interrupts a worker's blocked device call; the handler does nothing */

static void on_interrupt(int sig)
{
    (void)sig;
}

/* Has the peer given up on this job? Checked right before each device call. */
static int job_cancelled(const struct job *j)
{
    pthread_mutex_lock(&j->c->mtx);
    int x = j->c->js[j->js].cancel;
    pthread_mutex_unlock(&j->c->mtx);
    return x;
}

static void conn_put(struct conn *c)
{
    pthread_mutex_lock(&c->mtx);
    int last = (--c->refs == 0);
    pthread_mutex_unlock(&c->mtx);
    if (last) {
        vlog("connection %s released\n", c->peer);
        close(c->sock);
        pthread_mutex_lock(&conns_mtx);
        slot_used[c->slot] = 0;
        pthread_mutex_unlock(&conns_mtx);
        pthread_mutex_destroy(&c->mtx);
        free(c);
    }
}

/* With c->mtx held: ask one job to stop. A job not yet started stops before its device call. */
static void cancel_job_locked(struct conn *c, int i)
{
    struct jobslot *s = &c->js[i];
    if (!s->used || s->cancel)
        return;
    s->cancel = 1;
    if (s->tid_ok)
        pthread_kill(s->tid, CN_INTERRUPT);
    vlog("cancelled request %u\n", s->seq);
}

/* With c->mtx held: signal every cancelled job that is still running, in case it was about to
 * enter its device call when the first signal arrived. Returns how many are still running. */
static int resignal_locked(struct conn *c)
{
    int running = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (c->js[i].used && c->js[i].cancel) {
            running++;
            if (c->js[i].tid_ok)
                pthread_kill(c->js[i].tid, CN_INTERRUPT);
        }
    }
    return running;
}

static int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;            /* EOF, error, or SO_RCVTIMEO expired: the peer is gone */
        }
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static void send_reply(struct conn *c, const struct cn_hdr *req, int32_t err, int32_t val,
                       const void *data, uint32_t len)
{
    struct cn_hdr r = {
        .magic = CN_MAGIC, .op = (uint16_t)(req->op | CN_REPLY), .flags = 0, .seq = req->seq,
        .handle = req->handle, .a = err, .b = val, .len = len,
    };
    unsigned char *buf = malloc(sizeof r + len);
    if (buf == NULL)
        return;                   /* the peer sees no reply and its liveness check stays honest */
    memcpy(buf, &r, sizeof r);
    if (len)
        memcpy(buf + sizeof r, data, len);
    pthread_mutex_lock(&c->mtx);
    if (!c->dead && write_full(c->sock, buf, sizeof r + len) != 0) {
        c->dead = 1;
        shutdown(c->sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&c->mtx);
    free(buf);
}

/* A path is allowed when it has no empty, "." or ".." component and sits at or under an export. */
static int path_allowed(const char *p, char **list)
{
    if (p[0] == '\0' || p[0] == '/')
        return 0;
    for (const char *s = p; *s != '\0';) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        s += n;
        if (*s == '/')
            s++;
    }
    for (int i = 0; i < nexports; i++) {
        size_t n = strlen(list[i]);
        if (strncmp(p, list[i], n) == 0 && (p[n] == '\0' || p[n] == '/'))
            return 1;
    }
    return 0;
}

/* A private duplicate of the handle's descriptor; the job closes it when done. */
static int get_fd(struct conn *c, int32_t h)
{
    int fd = -1;
    pthread_mutex_lock(&c->mtx);
    if (!c->dead && h >= 0 && h < CN_MAX_HANDLES && c->fds[h] >= 0)
        fd = dup(c->fds[h]);
    pthread_mutex_unlock(&c->mtx);
    return fd;
}

static void do_open(struct job *j)
{
    struct conn *c = j->c;
    const struct cn_hdr *h = &j->h;
    char path[CN_MAX_PATH + 2];
    if (h->len == 0 || h->len > CN_MAX_PATH) {
        send_reply(c, h, ENAMETOOLONG, -1, NULL, 0);
        return;
    }
    path[0] = '/';
    memcpy(path + 1, j->payload, h->len);
    path[h->len + 1] = '\0';
    if (strlen(path + 1) != h->len || !path_allowed(path + 1, exports)) {
        vlog("refused %s\n", path);
        send_reply(c, h, EACCES, -1, NULL, 0);
        return;
    }
    /* Resolve links first: the real target must still be inside an export. */
    char real[PATH_MAX];
    if (realpath(path, real) == NULL) {
        /* ENOENT only when the name itself is absent from a directory inside the share; a link
         * that dangles may point anywhere, so it is refused like any other name outside. */
        int e = EACCES;
        struct stat st;
        if (errno == ENOENT && lstat(path, &st) != 0 && errno == ENOENT) {
            char dir[CN_MAX_PATH + 2], rdir[PATH_MAX];
            strcpy(dir, path);
            char *slash = strrchr(dir, '/');
            if (slash != NULL && slash != dir)
                *slash = '\0';
            if (realpath(dir, rdir) != NULL && path_allowed(rdir + 1, rexports))
                e = ENOENT;
        }
        send_reply(c, h, e, -1, NULL, 0);
        return;
    }
    if (!path_allowed(real + 1, rexports)) {
        vlog("refused %s (resolves to %s)\n", path, real);
        send_reply(c, h, EACCES, -1, NULL, 0);
        return;
    }
    /* Only access mode, non-blocking and append travel; nothing is ever created or truncated. */
    int oflag = (h->a & (O_ACCMODE | O_NONBLOCK | O_APPEND)) | O_NOCTTY | O_NOFOLLOW;
    if (job_cancelled(j)) {
        send_reply(c, h, EINTR, -1, NULL, 0);
        return;
    }
    int fd = open(real, oflag);
    if (fd < 0) {
        send_reply(c, h, errno, -1, NULL, 0);
        return;
    }
    /* Ask the descriptor what it actually opened; a component swapped after realpath() fails here. */
    char got[PATH_MAX];
    struct _fdinfo fi;
    got[0] = '\0';
    int gl = iofdinfo(fd, 0, &fi, got, (int)sizeof got);
    got[sizeof got - 1] = '\0';
    if (gl < 0 || got[0] != '/' || !path_allowed(got + 1, rexports)) {
        vlog("refused %s (the descriptor %s)\n", real, gl < 0 ? "could not be checked" : "points elsewhere");
        close(fd);
        send_reply(c, h, EACCES, -1, NULL, 0);
        return;
    }
    int slot = -1;
    pthread_mutex_lock(&c->mtx);
    if (!c->dead) {
        for (int i = 0; i < CN_MAX_HANDLES; i++) {
            if (c->fds[i] < 0) {
                c->fds[i] = fd;
                slot = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&c->mtx);
    if (slot < 0) {
        close(fd);
        send_reply(c, h, EMFILE, -1, NULL, 0);
        return;
    }
    vlog("open %s -> handle %d\n", path, slot);
    send_reply(c, h, 0, slot, NULL, 0);
}

static void do_read(struct job *j)
{
    const struct cn_hdr *h = &j->h;
    if (h->a < 0 || (uint32_t)h->a > CN_MAX_DATA) {
        send_reply(j->c, h, EINVAL, 0, NULL, 0);
        return;
    }
    if ((j->fd = get_fd(j->c, h->handle)) < 0) {
        send_reply(j->c, h, EBADF, 0, NULL, 0);
        return;
    }
    if ((j->buf = malloc((size_t)h->a + 1)) == NULL) {
        send_reply(j->c, h, ENOMEM, 0, NULL, 0);
        return;
    }
    ssize_t r = job_cancelled(j) ? (errno = EINTR, -1) : read(j->fd, j->buf, (size_t)h->a);
    if (r < 0)
        send_reply(j->c, h, errno, 0, NULL, 0);
    else
        send_reply(j->c, h, 0, (int32_t)r, j->buf, (uint32_t)r);
}

static void do_write(struct job *j)
{
    const struct cn_hdr *h = &j->h;
    if ((j->fd = get_fd(j->c, h->handle)) < 0) {
        send_reply(j->c, h, EBADF, 0, NULL, 0);
        return;
    }
    ssize_t w = job_cancelled(j) ? (errno = EINTR, -1) : write(j->fd, j->payload, h->len);
    if (w < 0)
        send_reply(j->c, h, errno, 0, NULL, 0);
    else
        send_reply(j->c, h, 0, (int32_t)w, NULL, 0);
}

/* Generic devctl: only the command's direction bits and the caller's byte count matter. */
static void do_devctl(struct job *j)
{
    const struct cn_hdr *h = &j->h;
    if (h->b < 0 || (uint32_t)h->b > CN_MAX_DATA || h->len > (uint32_t)h->b) {
        send_reply(j->c, h, EINVAL, 0, NULL, 0);
        return;
    }
    if ((j->fd = get_fd(j->c, h->handle)) < 0) {
        send_reply(j->c, h, EBADF, 0, NULL, 0);
        return;
    }
    size_t n = (size_t)h->b;
    if ((j->buf = calloc(1, n + 1)) == NULL) {
        send_reply(j->c, h, ENOMEM, 0, NULL, 0);
        return;
    }
    if (h->len)
        memcpy(j->buf, j->payload, h->len);
    int info = 0;
    int err = job_cancelled(j) ? EINTR : devctl(j->fd, h->a, n ? j->buf : NULL, n, &info);
    unsigned dir = get_device_direction(h->a);
    uint32_t out = (err == EOK && (dir & DEVDIR_FROM)) ? (uint32_t)n : 0;
    send_reply(j->c, h, err, info, j->buf, out);
}

/* Close a handle; any job still working on it is cancelled, since nobody can want its answer. */
static void do_close(struct conn *c, int32_t h)
{
    pthread_mutex_lock(&c->mtx);
    if (h >= 0 && h < CN_MAX_HANDLES && c->fds[h] >= 0) {
        close(c->fds[h]);
        c->fds[h] = -1;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (c->js[i].used && c->js[i].handle == h)
                cancel_job_locked(c, i);
        }
    }
    pthread_mutex_unlock(&c->mtx);
}

static void do_cancel(struct conn *c, uint32_t seq)
{
    pthread_mutex_lock(&c->mtx);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (c->js[i].used && c->js[i].seq == seq)
            cancel_job_locked(c, i);
    }
    pthread_mutex_unlock(&c->mtx);
}

/* Runs when a job ends: give back everything it held. */
static void job_end(struct job *j)
{
    struct conn *c = j->c;
    if (j->fd >= 0)
        close(j->fd);
    free(j->buf);
    free(j->payload);
    pthread_mutex_lock(&c->mtx);
    c->js[j->js].used = 0;
    c->js[j->js].tid_ok = 0;
    c->jobs--;
    vlog("request %u ended (%d still running on %s)\n", j->h.seq, c->jobs, c->peer);
    pthread_mutex_unlock(&c->mtx);
    conn_put(c);
    free(j);
}

static void *worker(void *arg)
{
    struct job *j = arg;
    pthread_mutex_lock(&j->c->mtx);
    j->c->js[j->js].tid = pthread_self();
    j->c->js[j->js].tid_ok = 1;
    pthread_mutex_unlock(&j->c->mtx);
    switch (j->h.op) {
    case CN_OP_OPEN:   do_open(j);   break;
    case CN_OP_READ:   do_read(j);   break;
    case CN_OP_WRITE:  do_write(j);  break;
    case CN_OP_DEVCTL: do_devctl(j); break;
    default:           send_reply(j->c, &j->h, ENOSYS, 0, NULL, 0); break;
    }
    job_end(j);
    return NULL;
}

static void start_job(struct conn *c, const struct cn_hdr *h, unsigned char *p)
{
    int js = -1;
    pthread_mutex_lock(&c->mtx);
    if (c->jobs < MAX_JOBS) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!c->js[i].used) {
                js = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&c->mtx);
    struct job *j = js >= 0 ? calloc(1, sizeof *j) : NULL;
    if (j == NULL) {
        send_reply(c, h, js < 0 ? EAGAIN : ENOMEM, 0, NULL, 0);
        free(p);
        return;
    }
    j->c = c;
    j->js = js;
    j->h = *h;
    j->payload = p;
    j->fd = -1;
    pthread_mutex_lock(&c->mtx);
    c->js[js] = (struct jobslot){ .used = 1, .seq = h->seq, .handle = h->handle };
    c->jobs++;
    c->refs++;
    pthread_mutex_unlock(&c->mtx);

    pthread_attr_t at;
    pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, worker, j);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        send_reply(c, h, EAGAIN, 0, NULL, 0);
        job_end(j);
    }
}

static void *reader(void *arg)
{
    struct conn *c = arg;
    for (;;) {
        struct cn_hdr h;
        if (read_full(c->sock, &h, sizeof h) != 0)
            break;
        if (h.magic != CN_MAGIC || (h.op & CN_REPLY) || h.len > CN_MAX_DATA) {
            vlog("protocol error from %s\n", c->peer);
            break;
        }
        unsigned char *p = NULL;
        if (h.len) {
            p = malloc(h.len);
            if (p == NULL || read_full(c->sock, p, h.len) != 0) {
                free(p);
                break;
            }
        }
        switch (h.op) {
        case CN_OP_PING:
            send_reply(c, &h, 0, 0, NULL, 0);
            free(p);
            break;
        case CN_OP_CLOSE:
            do_close(c, h.handle);
            free(p);
            break;
        case CN_OP_CANCEL:
            do_cancel(c, (uint32_t)h.a);
            free(p);
            break;
        default:
            start_job(c, &h, p);
            break;
        }
        pthread_mutex_lock(&c->mtx);
        (void)resignal_locked(c);
        pthread_mutex_unlock(&c->mtx);
    }
    /* The connection is over: stop every job, signalling again until each one has ended. */
    pthread_mutex_lock(&c->mtx);
    c->dead = 1;
    for (int i = 0; i < MAX_JOBS; i++)
        cancel_job_locked(c, i);
    pthread_mutex_unlock(&c->mtx);
    int left = 0;
    for (int tries = 0; tries < 40; tries++) {
        pthread_mutex_lock(&c->mtx);
        left = resignal_locked(c);
        pthread_mutex_unlock(&c->mtx);
        if (left == 0)
            break;
        usleep(50 * 1000);
    }
    if (left > 0)
        fprintf(stderr, "crossnode-agent: %d request(s) from %s did not stop; their device calls ignore signals\n",
                left, c->peer);
    pthread_mutex_lock(&c->mtx);
    for (int i = 0; i < CN_MAX_HANDLES; i++) {
        if (c->fds[i] >= 0) {
            close(c->fds[i]);
            c->fds[i] = -1;
        }
    }
    shutdown(c->sock, SHUT_RDWR);
    pthread_mutex_unlock(&c->mtx);
    vlog("peer %s gone\n", c->peer);
    conn_put(c);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: crossnode-agent -e PATH [-e PATH ...] [-l ADDRESS] [-p PORT] [-v]\n"
            "  -e PATH     export PATH and everything under it (for example /dev/hello or /dev)\n"
            "  -l ADDRESS  listen on this IPv4 address (default 127.0.0.1)\n"
            "  -p PORT     listen on this TCP port (default 7777)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *addr = "127.0.0.1";
    int port = 7777;
    int opt;
    while ((opt = getopt(argc, argv, "e:l:p:v")) != -1) {
        switch (opt) {
        case 'e': {
            if (nexports == MAX_EXPORTS)
                usage();
            char *e = optarg;
            while (*e == '/')
                e++;
            char *copy = strdup(e);
            size_t n = strlen(copy);
            while (n > 0 && copy[n - 1] == '/')
                copy[--n] = '\0';
            if (n == 0) {
                fprintf(stderr, "crossnode-agent: exporting / is not allowed\n");
                return 2;
            }
            exports[nexports++] = copy;
            break;
        }
        case 'l': addr = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default: usage();
        }
    }
    if (nexports == 0 || port <= 0 || port > 65535)
        usage();
    for (int i = 0; i < nexports; i++) {
        char given[PATH_MAX], real[PATH_MAX];
        snprintf(given, sizeof given, "/%s", exports[i]);
        rexports[i] = strdup(realpath(given, real) != NULL ? real + 1 : exports[i]);
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction si;
    memset(&si, 0, sizeof si);
    si.sa_handler = on_interrupt;           /* no SA_RESTART: the device call returns EINTR */
    sigaction(CN_INTERRUPT, &si, NULL);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "crossnode-agent: bad address %s\n", addr);
        return 2;
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (ls < 0 || setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 8) != 0) {
        perror("crossnode-agent: listen");
        return 1;
    }
    printf("crossnode-agent: serving %s:%d, exports:", addr, port);
    for (int i = 0; i < nexports; i++) {
        printf(" /%s", exports[i]);
        if (strcmp(exports[i], rexports[i]) != 0)
            printf(" (= /%s)", rexports[i]);
    }
    printf(" (no authentication in this build)\n");
    fflush(stdout);

    for (;;) {
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        int s = accept(ls, (struct sockaddr *)&pa, &pl);
        if (s < 0) {
            if (errno != EINTR)
                perror("crossnode-agent: accept");
            continue;
        }
        int slot = -1, same = 0;
        pthread_mutex_lock(&conns_mtx);
        for (int i = 0; i < MAX_CONNS; i++) {
            if (slot_used[i] && slot_addr[i].s_addr == pa.sin_addr.s_addr)
                same++;
            else if (!slot_used[i] && slot < 0)
                slot = i;
        }
        if (same >= MAX_PER_ADDR)
            slot = -1;
        if (slot >= 0) {
            slot_used[slot] = 1;
            slot_addr[slot] = pa.sin_addr;
        }
        pthread_mutex_unlock(&conns_mtx);
        if (slot < 0) {
            fprintf(stderr, "crossnode-agent: refused a connection from %s (%s)\n", inet_ntoa(pa.sin_addr),
                    same >= MAX_PER_ADDR ? "too many from that address" : "all slots in use");
            close(s);
            continue;
        }
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        struct timeval rtv = { PEER_SILENCE_MS / 1000, (PEER_SILENCE_MS % 1000) * 1000 };
        struct timeval stv = { 2, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
        struct conn *c = calloc(1, sizeof *c);
        if (c == NULL) {
            close(s);
            pthread_mutex_lock(&conns_mtx);
            slot_used[slot] = 0;
            pthread_mutex_unlock(&conns_mtx);
            continue;
        }
        c->sock = s;
        c->slot = slot;
        c->refs = 1;
        pthread_mutex_init(&c->mtx, NULL);
        for (int i = 0; i < CN_MAX_HANDLES; i++)
            c->fds[i] = -1;
        snprintf(c->peer, sizeof c->peer, "%s:%d", inet_ntoa(pa.sin_addr), ntohs(pa.sin_port));
        vlog("peer %s connected\n", c->peer);
        pthread_attr_t at;
        pthread_t t;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, reader, c) != 0)
            conn_put(c);            /* closes the socket and gives the slot back */
        pthread_attr_destroy(&at);
    }
}
