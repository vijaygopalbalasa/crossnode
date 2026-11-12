/*
 * crossnode-agent: serves this machine's exported paths to Crossnode peers.
 *
 * One reader thread per peer connection answers PING at once and hands every other request to a
 * worker thread of its own. A request that blocks on a slow device therefore never delays the
 * liveness replies a peer uses to tell "slow" from "dead".
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

struct conn {
    int sock;
    pthread_mutex_t mtx;          /* guards dead, refs, fds[] and every write to sock */
    int dead;
    int refs;
    int jobs;
    int slot;                 /* index in the slot table */
    int fds[CN_MAX_HANDLES];
    char peer[64];
};

struct job {
    struct conn *c;
    struct cn_hdr h;
    unsigned char *payload;
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

static void conn_put(struct conn *c)
{
    pthread_mutex_lock(&c->mtx);
    int last = (--c->refs == 0);
    pthread_mutex_unlock(&c->mtx);
    if (last) {
        close(c->sock);
        pthread_mutex_lock(&conns_mtx);
        slot_used[c->slot] = 0;
        pthread_mutex_unlock(&conns_mtx);
        pthread_mutex_destroy(&c->mtx);
        free(c);
    }
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

/* A private duplicate of the handle's descriptor; the caller closes it when done. */
static int get_fd(struct conn *c, int32_t h)
{
    int fd = -1;
    pthread_mutex_lock(&c->mtx);
    if (!c->dead && h >= 0 && h < CN_MAX_HANDLES && c->fds[h] >= 0)
        fd = dup(c->fds[h]);
    pthread_mutex_unlock(&c->mtx);
    return fd;
}

static void do_open(struct conn *c, const struct cn_hdr *h, const unsigned char *payload)
{
    char path[CN_MAX_PATH + 2];
    if (h->len == 0 || h->len > CN_MAX_PATH) {
        send_reply(c, h, ENAMETOOLONG, -1, NULL, 0);
        return;
    }
    path[0] = '/';
    memcpy(path + 1, payload, h->len);
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
    int fd = open(real, oflag);
    if (fd < 0) {
        send_reply(c, h, errno == ENOENT ? ENOENT : errno, -1, NULL, 0);
        return;
    }
    /* Ask the descriptor what it actually opened; a component swapped after realpath() fails here. */
    char got[PATH_MAX];
    struct _fdinfo fi;
    if (iofdinfo(fd, 0, &fi, got, (int)sizeof got) < 0 || got[0] != '/' || !path_allowed(got + 1, rexports)) {
        vlog("refused %s (the descriptor points at %s)\n", real, got);
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

static void do_read(struct conn *c, const struct cn_hdr *h)
{
    int fd = get_fd(c, h->handle);
    if (fd < 0) {
        send_reply(c, h, EBADF, 0, NULL, 0);
        return;
    }
    if (h->a < 0 || (uint32_t)h->a > CN_MAX_DATA) {
        close(fd);
        send_reply(c, h, EINVAL, 0, NULL, 0);
        return;
    }
    unsigned char *buf = malloc((size_t)h->a + 1);
    if (buf == NULL) {
        close(fd);
        send_reply(c, h, ENOMEM, 0, NULL, 0);
        return;
    }
    ssize_t r = read(fd, buf, (size_t)h->a);
    int e = errno;
    close(fd);
    if (r < 0)
        send_reply(c, h, e, 0, NULL, 0);
    else
        send_reply(c, h, 0, (int32_t)r, buf, (uint32_t)r);
    free(buf);
}

static void do_write(struct conn *c, const struct cn_hdr *h, const unsigned char *payload)
{
    int fd = get_fd(c, h->handle);
    if (fd < 0) {
        send_reply(c, h, EBADF, 0, NULL, 0);
        return;
    }
    ssize_t w = write(fd, payload, h->len);
    int e = errno;
    close(fd);
    if (w < 0)
        send_reply(c, h, e, 0, NULL, 0);
    else
        send_reply(c, h, 0, (int32_t)w, NULL, 0);
}

/* Generic devctl: only the command's direction bits and the caller's byte count matter. */
static void do_devctl(struct conn *c, const struct cn_hdr *h, const unsigned char *payload)
{
    int fd = get_fd(c, h->handle);
    if (fd < 0) {
        send_reply(c, h, EBADF, 0, NULL, 0);
        return;
    }
    if (h->b < 0 || (uint32_t)h->b > CN_MAX_DATA || h->len > (uint32_t)h->b) {
        close(fd);
        send_reply(c, h, EINVAL, 0, NULL, 0);
        return;
    }
    size_t n = (size_t)h->b;
    unsigned char *buf = calloc(1, n + 1);
    if (buf == NULL) {
        close(fd);
        send_reply(c, h, ENOMEM, 0, NULL, 0);
        return;
    }
    if (h->len)
        memcpy(buf, payload, h->len);
    int info = 0;
    int err = devctl(fd, h->a, n ? buf : NULL, n, &info);
    close(fd);
    unsigned dir = get_device_direction(h->a);
    uint32_t out = (err == EOK && (dir & DEVDIR_FROM)) ? (uint32_t)n : 0;
    send_reply(c, h, err, info, buf, out);
    free(buf);
}

static void do_close(struct conn *c, int32_t h)
{
    pthread_mutex_lock(&c->mtx);
    if (h >= 0 && h < CN_MAX_HANDLES && c->fds[h] >= 0) {
        close(c->fds[h]);
        c->fds[h] = -1;
    }
    pthread_mutex_unlock(&c->mtx);
}

static void *worker(void *arg)
{
    struct job *j = arg;
    switch (j->h.op) {
    case CN_OP_OPEN:   do_open(j->c, &j->h, j->payload);   break;
    case CN_OP_READ:   do_read(j->c, &j->h);               break;
    case CN_OP_WRITE:  do_write(j->c, &j->h, j->payload);  break;
    case CN_OP_DEVCTL: do_devctl(j->c, &j->h, j->payload); break;
    default:           send_reply(j->c, &j->h, ENOSYS, 0, NULL, 0); break;
    }
    pthread_mutex_lock(&j->c->mtx);
    j->c->jobs--;
    pthread_mutex_unlock(&j->c->mtx);
    conn_put(j->c);
    free(j->payload);
    free(j);
    return NULL;
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
        if (h.op == CN_OP_PING) {
            send_reply(c, &h, 0, 0, NULL, 0);
            free(p);
            continue;
        }
        if (h.op == CN_OP_CLOSE) {
            do_close(c, h.handle);
            free(p);
            continue;
        }
        pthread_mutex_lock(&c->mtx);
        int busy = c->jobs >= MAX_JOBS;
        pthread_mutex_unlock(&c->mtx);
        struct job *j = busy ? NULL : malloc(sizeof *j);
        if (j == NULL) {
            send_reply(c, &h, busy ? EAGAIN : ENOMEM, 0, NULL, 0);
            free(p);
            continue;
        }
        j->c = c;
        j->h = h;
        j->payload = p;
        pthread_mutex_lock(&c->mtx);
        c->refs++;
        c->jobs++;
        pthread_mutex_unlock(&c->mtx);
        pthread_attr_t at;
        pthread_t t;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, worker, j) != 0) {
            send_reply(c, &h, EAGAIN, 0, NULL, 0);
            free(p);
            free(j);
            pthread_mutex_lock(&c->mtx);
            c->jobs--;
            pthread_mutex_unlock(&c->mtx);
            conn_put(c);
        }
        pthread_attr_destroy(&at);
    }
    pthread_mutex_lock(&c->mtx);
    c->dead = 1;
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
