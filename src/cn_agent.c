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
 * (pthread_cancel is not used: a signal makes the blocked call return EINTR, so the worker gives back its
 * descriptor and job slot in ordinary code after the call, with no cancellation cleanup handler involved.)
 *
 *
 * Every open is resolved with realpath() and must land inside an export; then the descriptor itself
 * is asked where it points (iofdinfo), so swapping a path component between the check and the open
 * gains nothing. Names outside the share all answer EACCES, so a peer cannot probe what exists there.
 * Each operation works on its own dup() of the descriptor, so a close racing with it cannot
 * redirect it to a file opened later. In-flight requests, connections, and connections per peer
 * address are capped, and every refusal is logged.
 *
 * stat and directory listing go through the same resolver as open (resolve()). A directory above a share
 * (a proper ancestor of an export, or the root) is virtual: it lists only the way to the shares.
 *
 * Every connection starts with a handshake (cn_auth.h): the agent serves nothing until the peer proves it holds the
 * shared key (-k); -I turns this off. After it, every frame both ways carries an HMAC-SHA256 tag (cn_frame_tag), and
 * a frame that fails it drops the connection. Limit, stated plainly: the traffic is not encrypted.
 */
#include <arpa/inet.h>
#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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
#include <time.h>
#include <unistd.h>

#include "cn_auth.h"
#include "cn_proto.h"

#define MAX_EXPORTS        32
#define MAX_JOBS           64     /* requests in flight per connection */
#define MAX_CONNS          16     /* peer connections at once */
#define MAX_PER_ADDR       4      /* connections at once from one address */
#define MAX_UNAUTH         4      /* connections still in the handshake, at most, so they can never take every slot */
#define MAX_UNAUTH_PER_ADDR 2     /* ... and at most this many from one address, so one address cannot fill them */
#define HANDSHAKE_MS       2000   /* the whole handshake, not each read */
#define PEER_SILENCE_MS    5000   /* drop a peer that sends nothing, not even PING, for this long */

static unsigned char key[CN_KEY_MAX];   /* -k: the shared key */
static size_t keylen;                   /* 0 with -I: no authentication */
static char *exports[MAX_EXPORTS];      /* as given, without the leading '/' */
static char *rexports[MAX_EXPORTS];     /* the same, resolved with realpath() at start */
static int nexports;
static int verbose;
static pthread_mutex_t conns_mtx = PTHREAD_MUTEX_INITIALIZER;   /* guards the slot table */
static struct in_addr slot_addr[MAX_CONNS];
static int slot_used[MAX_CONNS];
enum { S_FREE = 0, S_UNAUTH, S_AUTH, S_EVICTED };
/* S_UNAUTH: in the handshake, counted against MAX_UNAUTH only. S_AUTH: a peer, counted against its address.
 * S_EVICTED: told to leave and shut down, but the slot is not free until its reader has ended; counted nowhere. */
static int slot_state[MAX_CONNS];
static uint64_t slot_since[MAX_CONNS];  /* when a slot was taken, for dropping the longest-waiting stranger */
struct conn;
static struct conn *slot_conn[MAX_CONNS];

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
    volatile int evict;       /* a newer connection needs this stranger's place: give up the handshake */
    int fds[CN_MAX_HANDLES];
    char *vpath[CN_MAX_HANDLES];   /* set when the handle is a virtual directory: its path in the share */
    pthread_mutex_t dir_mtx;       /* one directory read at a time: a directory descriptor has one offset */
    struct jobslot js[MAX_JOBS];
    char peer[64];
    int tagged;               /* keyed link after the handshake: every frame carries a tag */
    unsigned char ktx[32], krx[32];  /* frame keys: to crossnode-net (written under mtx), from it (reader only) */
    uint64_t ctx, crx;        /* frames sent (under mtx) and received since the handshake */
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
        pthread_mutex_lock(&conns_mtx);
        slot_conn[c->slot] = NULL;              /* from here nobody may shut the socket down */
        pthread_mutex_unlock(&conns_mtx);
        close(c->sock);
        pthread_mutex_lock(&conns_mtx);
        slot_used[c->slot] = 0;
        slot_state[c->slot] = S_FREE;
        pthread_mutex_unlock(&conns_mtx);
        pthread_mutex_destroy(&c->mtx);
        pthread_mutex_destroy(&c->dir_mtx);
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

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Like read_full, but everything must arrive before one absolute deadline: a stranger who sends a byte every
 * second gains nothing. */
static int read_by(int fd, void *buf, size_t n, uint64_t deadline, volatile int *stop)
{
    unsigned char *p = buf;
    while (n > 0) {
        uint64_t now = mono_ms();
        if (now >= deadline || *stop)
            return -1;
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int pr = poll(&pf, 1, (int)(deadline - now > 100 ? 100 : deadline - now));
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr == 0)
            continue;                   /* a slice passed: look at the deadline and the stop flag again */
        if (pr < 0)
            return -1;
        ssize_t r = recv(fd, p, n, MSG_DONTWAIT);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
        } else if (r == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return -1;
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

static void send_reply_f(struct conn *c, const struct cn_hdr *req, int32_t err, int32_t val,
                         const void *data, uint32_t len, uint16_t flags)
{
    struct cn_hdr r = {
        .magic = CN_MAGIC, .op = (uint16_t)(req->op | CN_REPLY), .flags = flags, .seq = req->seq,
        .handle = req->handle, .a = err, .b = val, .len = len,
    };
    unsigned char *buf = malloc(sizeof r + len + CN_TAG_LEN);
    if (buf == NULL)
        return;                   /* the peer sees no reply and its liveness check stays honest */
    memcpy(buf, &r, sizeof r);
    if (len)
        memcpy(buf + sizeof r, data, len);
    pthread_mutex_lock(&c->mtx);
    size_t n = sizeof r + len;
    if (c->tagged) {              /* the counter follows the order of writes, which this lock fixes */
        cn_frame_tag(c->ktx, c->ctx++, buf, buf + sizeof r, len, buf + n);
        n += CN_TAG_LEN;
    }
    if (!c->dead && write_full(c->sock, buf, n) != 0) {
        c->dead = 1;
        shutdown(c->sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&c->mtx);
    free(buf);
}

static void send_reply(struct conn *c, const struct cn_hdr *req, int32_t err, int32_t val,
                       const void *data, uint32_t len)
{
    send_reply_f(c, req, err, val, data, len, 0);
}

/* No empty, "." or ".." component: the path cannot climb out of, or slip around, a share. */
static int path_clean(const char *p)
{
    for (const char *s = p; *s != '\0';) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        s += n;
        if (*s == '/')
            s++;
    }
    return 1;
}

/* A path is allowed when it is clean and sits at or under an export. */
static int path_allowed(const char *p, char **list)
{
    if (p[0] == '\0' || p[0] == '/' || !path_clean(p))
        return 0;
    for (int i = 0; i < nexports; i++) {
        size_t n = strlen(list[i]);
        if (strncmp(p, list[i], n) == 0 && (p[n] == '\0' || p[n] == '/'))
            return 1;
    }
    return 0;
}

/* A virtual directory is the root, or a directory that only leads to shares: a proper ancestor of an
 * export. It lists just the way to the shares, never what else lives beside them. */
static int path_virtual(const char *p)
{
    if (p[0] == '\0')
        return nexports > 0;
    if (p[0] == '/' || !path_clean(p))
        return 0;
    size_t n = strlen(p);
    for (int i = 0; i < nexports; i++) {
        if (strncmp(exports[i], p, n) == 0 && exports[i][n] == '/')
            return 1;
    }
    return 0;
}

/*
 * The one place a request's path is resolved and contained, for open, stat and listing alike.
 * Returns 0 and fills real (a resolved path inside a share), or sets *virt for a virtual directory,
 * or returns the errno the caller must answer. Names outside every share answer EACCES.
 */
static int resolve(const unsigned char *rel, uint32_t len, char *path, char *real, int *virt)
{
    *virt = 0;
    if (len > CN_MAX_PATH)
        return ENAMETOOLONG;
    path[0] = '/';
    if (len)
        memcpy(path + 1, rel, len);
    path[len + 1] = '\0';
    if (strlen(path + 1) != len)
        return EACCES;
    if (!path_allowed(path + 1, exports)) {
        if (path_virtual(path + 1)) {
            *virt = 1;
            return 0;
        }
        vlog("refused %s\n", path);
        return EACCES;
    }
    /* Resolve links first: the real target must still be inside an export. */
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
        return e;
    }
    if (!path_allowed(real + 1, rexports)) {
        vlog("refused %s (resolves to %s)\n", path, real);
        return EACCES;
    }
    return 0;
}

/* A private duplicate of the handle's descriptor; the job closes it when done. */
static int get_fd(struct conn *c, int32_t h)
{
    int fd = -1;
    pthread_mutex_lock(&c->mtx);
    /* A virtual directory's descriptor is a placeholder: reading it would show the real "/". */
    if (!c->dead && h >= 0 && h < CN_MAX_HANDLES && c->fds[h] >= 0 && c->vpath[h] == NULL)
        fd = dup(c->fds[h]);
    pthread_mutex_unlock(&c->mtx);
    return fd;
}

/* Give back a handle slot's descriptor and virtual path; c->mtx held. */
static void handle_release_locked(struct conn *c, int i)
{
    if (c->fds[i] >= 0)
        close(c->fds[i]);
    c->fds[i] = -1;
    free(c->vpath[i]);
    c->vpath[i] = NULL;
}

static void do_open(struct job *j)
{
    struct conn *c = j->c;
    const struct cn_hdr *h = &j->h;
    char path[CN_MAX_PATH + 2], real[PATH_MAX];
    int virt;
    int e = resolve(j->payload, h->len, path, real, &virt);
    if (e != 0) {
        send_reply(c, h, e, -1, NULL, 0);
        return;
    }
    int fd, isdir = 0;
    char *vp = NULL;
    if (virt) {
        /* A directory that only leads to shares: read-only, and nothing to open on the real file system. */
        if ((h->a & O_ACCMODE) != O_RDONLY) {
            send_reply(c, h, EACCES, -1, NULL, 0);
            return;
        }
        vp = strdup(path + 1);
        fd = open("/", O_RDONLY | O_NOCTTY);      /* only holds the handle slot */
        isdir = 1;
        if (vp == NULL || fd < 0) {
            free(vp);
            if (fd >= 0)
                close(fd);
            send_reply(c, h, ENOMEM, -1, NULL, 0);
            return;
        }
    } else {
        /* Only access mode, non-blocking and append travel; nothing is ever created or truncated. */
        int oflag = (h->a & (O_ACCMODE | O_NONBLOCK | O_APPEND)) | O_NOCTTY | O_NOFOLLOW;
        if (job_cancelled(j)) {
            send_reply(c, h, EINTR, -1, NULL, 0);
            return;
        }
        fd = open(real, oflag);
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
        size_t gn = strlen(got);
        if (gn > 1 && got[gn - 1] == '/')
            got[gn - 1] = '\0';           /* a directory is reported with a trailing slash */
        if (gl < 0 || got[0] != '/' || !path_allowed(got + 1, rexports)) {
            vlog("refused %s (the descriptor %s)\n", real, gl < 0 ? "could not be checked" : "points elsewhere");
            close(fd);
            send_reply(c, h, EACCES, -1, NULL, 0);
            return;
        }
        struct stat st;
        isdir = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
        if ((h->a & CN_OFLAG_DIR) && !isdir) {
            close(fd);
            send_reply(c, h, ENOTDIR, -1, NULL, 0);
            return;
        }
        if (isdir) {
            /* For a directory the descriptor kept is a second open made with O_DIRECTORY (the kind
             * fdopendir() needs); the checked descriptor stays a witness, and the second open must be the
             * very same directory (same device and inode). */
            int fd2 = open(real, (oflag & ~O_ACCMODE) | O_RDONLY | O_DIRECTORY);
            struct stat s2;
            if (fd2 < 0 || fstat(fd2, &s2) != 0 || s2.st_dev != st.st_dev || s2.st_ino != st.st_ino) {
                int e2 = fd2 < 0 ? errno : EACCES;
                if (fd2 >= 0)
                    close(fd2);
                close(fd);
                send_reply(c, h, e2, -1, NULL, 0);
                return;
            }
            close(fd);
            fd = fd2;
        }
    }
    int slot = -1;
    pthread_mutex_lock(&c->mtx);
    if (!c->dead) {
        for (int i = 0; i < CN_MAX_HANDLES; i++) {
            if (c->fds[i] < 0) {
                c->fds[i] = fd;
                c->vpath[i] = vp;
                slot = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&c->mtx);
    if (slot < 0) {
        close(fd);
        free(vp);
        send_reply(c, h, EMFILE, -1, NULL, 0);
        return;
    }
    vlog("open %s -> handle %d%s\n", path, slot, virt ? " (virtual directory)" : "");
    send_reply_f(c, h, 0, slot, NULL, 0, isdir ? CN_F_DIR : 0);
}

static uint64_t path_ino(const char *p)       /* a stable made-up inode for a virtual directory */
{
    uint64_t x = 1469598103934665603ull;
    for (; *p != '\0'; p++)
        x = (x ^ (unsigned char)*p) * 1099511628211ull;
    return x | 1u;
}

static time_t started_at;

static void stat_from_native(struct cn_stat *o, const struct stat *st)
{
    memset(o, 0, sizeof *o);
    o->ino = (uint64_t)st->st_ino;
    o->size = st->st_size < 0 ? 0 : (uint64_t)st->st_size;
    o->atime = (int64_t)st->st_atime;
    o->mtime = (int64_t)st->st_mtime;
    o->ctime = (int64_t)st->st_ctime;
    o->mode = (uint32_t)st->st_mode;
    o->uid = (uint32_t)st->st_uid;
    o->gid = (uint32_t)st->st_gid;
    o->nlink = (uint32_t)st->st_nlink;
    o->rdev = (uint32_t)st->st_rdev;
    o->blksize = (uint32_t)st->st_blksize;
}

static void stat_virtual(struct cn_stat *o, const char *vpath)
{
    memset(o, 0, sizeof *o);
    o->ino = path_ino(vpath);
    o->atime = o->mtime = o->ctime = (int64_t)started_at;
    o->mode = S_IFDIR | 0555;               /* read-only, and nothing but the way to the shares */
    o->nlink = 2;
    o->blksize = 4096;
}

/* With c->mtx held: the descriptor and virtual path behind a handle. The caller owns both results. */
static int get_handle(struct conn *c, int32_t h, int *fd, char **vp)
{
    *fd = -1;
    *vp = NULL;
    pthread_mutex_lock(&c->mtx);
    int ok = !c->dead && h >= 0 && h < CN_MAX_HANDLES && c->fds[h] >= 0;
    if (ok) {
        *fd = dup(c->fds[h]);
        if (c->vpath[h] != NULL)
            *vp = strdup(c->vpath[h]);
        if (*fd < 0 || (c->vpath[h] != NULL && *vp == NULL)) {
            if (*fd >= 0)
                close(*fd);
            *fd = -1;
            free(*vp);
            *vp = NULL;
            ok = 0;
        }
    }
    pthread_mutex_unlock(&c->mtx);
    return ok ? 0 : -1;
}

static void do_stat(struct job *j)
{
    const struct cn_hdr *h = &j->h;
    struct cn_stat out;
    struct stat st;
    if (h->handle >= 0) {
        char *vp;
        if (get_handle(j->c, h->handle, &j->fd, &vp) != 0) {
            send_reply(j->c, h, EBADF, 0, NULL, 0);
            return;
        }
        if (vp != NULL) {
            stat_virtual(&out, vp);
            free(vp);
        } else if (fstat(j->fd, &st) != 0) {
            send_reply(j->c, h, errno, 0, NULL, 0);
            return;
        } else {
            stat_from_native(&out, &st);
        }
        send_reply(j->c, h, 0, 0, &out, sizeof out);
        return;
    }
    char path[CN_MAX_PATH + 2], real[PATH_MAX];
    int virt;
    int e = resolve(j->payload, h->len, path, real, &virt);
    if (e != 0) {
        send_reply(j->c, h, e, 0, NULL, 0);
        return;
    }
    if (virt) {
        stat_virtual(&out, path + 1);
    } else {
        /* Regular files and directories cannot have side effects when opened, so they are stat'ed through a
         * descriptor that is checked the way do_open checks one (iofdinfo inside the share, same device and
         * inode as the resolved name). Anything else (a device, a FIFO) is not opened, because that would run
         * its open handler: it is looked at by name before and after a second resolve, which narrows a
         * path swap but cannot rule it out. */
        struct stat pre;
        if (lstat(real, &pre) != 0) {
            send_reply(j->c, h, errno, 0, NULL, 0);
            return;
        }
        if (S_ISREG(pre.st_mode) || S_ISDIR(pre.st_mode)) {
            int fd = open(real, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW);
            if (fd < 0) {
                send_reply(j->c, h, errno, 0, NULL, 0);
                return;
            }
            char got[PATH_MAX];
            struct _fdinfo fi;
            got[0] = '\0';
            int gl = iofdinfo(fd, 0, &fi, got, (int)sizeof got);
            got[sizeof got - 1] = '\0';
            size_t gn = strlen(got);
            if (gn > 1 && got[gn - 1] == '/')
                got[gn - 1] = '\0';
            if (gl < 0 || got[0] != '/' || !path_allowed(got + 1, rexports) || fstat(fd, &st) != 0 ||
                st.st_dev != pre.st_dev || st.st_ino != pre.st_ino) {
                vlog("refused stat of %s (the descriptor points elsewhere)\n", real);
                close(fd);
                send_reply(j->c, h, EACCES, 0, NULL, 0);
                return;
            }
            close(fd);
        } else {
            char again[PATH_MAX];
            if (realpath(path, again) == NULL || strcmp(again, real) != 0 || lstat(real, &st) != 0 ||
                st.st_dev != pre.st_dev || st.st_ino != pre.st_ino) {
                send_reply(j->c, h, EACCES, 0, NULL, 0);
                return;
            }
        }
        stat_from_native(&out, &st);
    }
    send_reply(j->c, h, 0, 0, &out, sizeof out);
}

struct dent_list {
    unsigned char *buf;
    size_t used, max;
    uint32_t count;
};

/* Append one entry if it fits; returns 0 when it does not. */
static int dent_add(struct dent_list *l, uint64_t ino, const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > CN_MAX_NAME || strchr(name, '/') != NULL)
        return 1;                               /* cannot be sent: leave it out */
    size_t sz = CN_DENT_SIZE(n);
    if (l->used + sz > l->max)
        return 0;
    struct cn_dent d = { .ino = ino, .namelen = (uint32_t)n };
    memset(l->buf + l->used, 0, sz);
    memcpy(l->buf + l->used, &d, sizeof d);
    memcpy(l->buf + l->used + sizeof d, name, n);
    l->used += sz;
    l->count++;
    return 1;
}

static void do_readdir(struct job *j)
{
    const struct cn_hdr *h = &j->h;
    if (h->a < 0 || h->b < 0 || (uint32_t)h->b > CN_MAX_DATA) {
        send_reply(j->c, h, EINVAL, 0, NULL, 0);
        return;
    }
    char *vp;
    if (get_handle(j->c, h->handle, &j->fd, &vp) != 0) {
        send_reply(j->c, h, EBADF, 0, NULL, 0);
        return;
    }
    struct dent_list l = { .buf = calloc(1, (size_t)h->b + 1), .max = (size_t)h->b };
    if (l.buf == NULL) {
        free(vp);
        send_reply(j->c, h, ENOMEM, 0, NULL, 0);
        return;
    }
    j->buf = l.buf;
    uint32_t want = (uint32_t)h->a, idx = 0;
    int full = 0, err = 0;
    if (vp != NULL) {
        /* "." and "..", then each distinct next component of an export below this directory. */
        size_t n = strlen(vp);
        const char *seen[MAX_EXPORTS];
        int nseen = 0;
        for (int step = -2; step < nexports && !full; step++) {
            char name[CN_MAX_NAME + 1], full_path[CN_MAX_PATH + 2];
            uint64_t ino;
            if (step < 0) {
                snprintf(name, sizeof name, "%s", step == -2 ? "." : "..");
                ino = path_ino(vp);
            } else {
                const char *e = exports[step];
                if (n > 0 && !(strncmp(e, vp, n) == 0 && e[n] == '/'))
                    continue;
                const char *rest = n > 0 ? e + n + 1 : e;
                const char *slash = strchr(rest, '/');
                size_t cl = slash ? (size_t)(slash - rest) : strlen(rest);
                if (cl == 0 || cl > CN_MAX_NAME)
                    continue;
                memcpy(name, rest, cl);
                name[cl] = '\0';
                int dup = 0;
                for (int k = 0; k < nseen; k++) {
                    if (strncmp(seen[k], rest, cl) == 0 && (seen[k][cl] == '\0' || seen[k][cl] == '/'))
                        dup = 1;
                }
                if (dup)
                    continue;
                seen[nseen++] = rest;
                snprintf(full_path, sizeof full_path, "/%.*s", (int)(rest - e) + (int)cl, e);
                struct stat st;
                if (stat(full_path, &st) != 0)
                    continue;                  /* a share that is not there is not listed */
                ino = (uint64_t)st.st_ino;
            }
            if (idx++ < want)
                continue;
            if (!dent_add(&l, ino, name))
                full = 1;
        }
    } else {
        pthread_mutex_lock(&j->c->dir_mtx);
        DIR *d = fdopendir(j->fd);
        if (d == NULL) {
            err = errno;
        } else {
            j->fd = -1;                         /* closedir() closes it */
            rewinddir(d);
            struct dirent *de;
            errno = 0;
            while (!full && (de = readdir(d)) != NULL) {
                if (idx++ < want)
                    continue;
                if (!dent_add(&l, (uint64_t)de->d_ino, de->d_name))
                    full = 1;
            }
            if (!full && errno != 0)
                err = errno;
            closedir(d);
        }
        pthread_mutex_unlock(&j->c->dir_mtx);
    }
    free(vp);
    if (err != 0)
        send_reply(j->c, h, err, 0, NULL, 0);
    else if (full && l.count == 0)
        send_reply(j->c, h, EINVAL, 0, NULL, 0);    /* not even one entry fits in what was asked for */
    else
        send_reply(j->c, h, 0, (int32_t)l.count, l.buf, (uint32_t)l.used);
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
        handle_release_locked(c, h);
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
    case CN_OP_STAT:   do_stat(j);   break;
    case CN_OP_READDIR: do_readdir(j); break;
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

/* With conns_mtx held: how many slots in this state come from this address. */
static int addr_count(struct in_addr a, int state)
{
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (slot_used[i] && slot_state[i] == state && slot_addr[i].s_addr == a.s_addr)
            n++;
    return n;
}

/* With conns_mtx held: tell a handshake to give up, and cut its socket so its slot frees at once. */
static void evict_locked(int i)
{
    slot_state[i] = S_EVICTED;
    slot_conn[i]->evict = 1;
    shutdown(slot_conn[i]->sock, SHUT_RDWR);
}

/* With conns_mtx held: the longest-waiting handshake, optionally only from one address (a.s_addr != 0). */
static int oldest_unauth_locked(struct in_addr a, int only_addr)
{
    int old = -1;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (slot_used[i] && slot_state[i] == S_UNAUTH && slot_conn[i] != NULL &&
            (!only_addr || slot_addr[i].s_addr == a.s_addr) && (old < 0 || slot_since[i] < slot_since[old]))
            old = i;
    }
    return old;
}

/* The first thing on a connection: prove both ends hold the key, or hang up. Returns 0 when it is a peer. */
static int handshake(struct conn *c)
{
    unsigned char na[CN_NONCE_LEN], req[CN_NONCE_LEN + CN_MAC_LEN], want[CN_MAC_LEN];
    uint64_t deadline = mono_ms() + HANDSHAKE_MS;      /* a stranger gets two seconds in all */
    if (cn_random(na, sizeof na) != 0)
        return -1;
    struct cn_hdr h = { .magic = CN_MAGIC, .op = CN_OP_AUTH | CN_REPLY, .handle = -1, .len = sizeof na };
    unsigned char out[sizeof h + CN_MAC_LEN];
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, na, sizeof na);
    int ok = write_full(c->sock, out, sizeof h + sizeof na) == 0 && read_by(c->sock, &h, sizeof h, deadline, &c->evict) == 0 &&
             h.magic == CN_MAGIC && h.op == CN_OP_AUTH && h.len == sizeof req &&
             read_by(c->sock, req, sizeof req, deadline, &c->evict) == 0;
    if (ok) {
        cn_auth_mac(key, keylen, 'C', na, req, want);
        ok = cn_ct_equal(want, req + CN_NONCE_LEN, CN_MAC_LEN);
    }
    if (!ok) {
        fprintf(stderr, "crossnode-agent: authentication failed from %s (no valid proof of the key)\n", c->peer);
        return -1;
    }
    /* A peer now, if it was not told to leave meanwhile and its address has room: decided under the lock. */
    pthread_mutex_lock(&conns_mtx);
    int promote = !c->evict && slot_state[c->slot] == S_UNAUTH &&
                  addr_count(slot_addr[c->slot], S_AUTH) < MAX_PER_ADDR;
    if (promote)
        slot_state[c->slot] = S_AUTH;
    pthread_mutex_unlock(&conns_mtx);
    if (!promote)
        return -1;
    struct cn_hdr r = { .magic = CN_MAGIC, .op = CN_OP_AUTH | CN_REPLY, .seq = h.seq, .handle = -1, .len = CN_MAC_LEN };
    memcpy(out, &r, sizeof r);
    cn_auth_mac(key, keylen, 'A', req, na, out + sizeof r);
    if (write_full(c->sock, out, sizeof r + CN_MAC_LEN) != 0)
        return -1;
    cn_frame_keys(key, keylen, na, req, c->krx, c->ktx);   /* req starts with crossnode-net's nonce */
    c->ctx = c->crx = 0;
    c->tagged = 1;
    struct timeval rtv = { PEER_SILENCE_MS / 1000, (PEER_SILENCE_MS % 1000) * 1000 };
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
    vlog("peer %s authenticated\n", c->peer);
    return 0;
}

static void *reader(void *arg)
{
    struct conn *c = arg;
    int ok = keylen == 0 || handshake(c) == 0;
    for (; ok;) {
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
        if (c->tagged) {
            unsigned char got[CN_TAG_LEN], want[CN_TAG_LEN];
            if (read_full(c->sock, got, sizeof got) != 0) {
                free(p);
                break;
            }
            cn_frame_tag(c->krx, c->crx++, &h, p, h.len, want);
            if (!cn_ct_equal(want, got, CN_TAG_LEN)) {
                fprintf(stderr, "crossnode-agent: a frame from %s failed its integrity check; dropping the connection\n", c->peer);
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
    for (int i = 0; i < CN_MAX_HANDLES; i++)
        handle_release_locked(c, i);
    shutdown(c->sock, SHUT_RDWR);
    pthread_mutex_unlock(&c->mtx);
    vlog("peer %s gone\n", c->peer);
    conn_put(c);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: crossnode-agent (-k KEYFILE | -I) -e PATH [-e PATH ...] [-l ADDRESS] [-p PORT] [-v]\n"
            "  -k KEYFILE  shared key (at least 16 bytes, mode 600); peers must prove they hold it\n"
            "  -I          run with NO authentication (anyone who reaches the port can use the shares)\n"
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
    const char *keyfile = NULL;
    int insecure = 0;
    while ((opt = getopt(argc, argv, "e:l:p:vk:I")) != -1) {
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
        case 'k': keyfile = optarg; break;
        case 'I': insecure = 1; break;
        default: usage();
        }
    }
    if (nexports == 0 || port <= 0 || port > 65535)
        usage();
    if (keyfile != NULL && insecure) {
        fprintf(stderr, "crossnode-agent: -k and -I together make no sense\n");
        return 2;
    }
    if (keyfile == NULL && !insecure) {
        fprintf(stderr, "crossnode-agent: no -k KEYFILE (give a key, or -I to run without authentication)\n");
        return 2;
    }
    if (keyfile != NULL) {
        char err[256];
        int n = cn_load_key(keyfile, key, err, sizeof err);
        if (n < 0) {
            fprintf(stderr, "crossnode-agent: %s\n", err);
            return 2;
        }
        keylen = (size_t)n;
    }
    for (int i = 0; i < nexports; i++) {
        char given[PATH_MAX], real[PATH_MAX];
        snprintf(given, sizeof given, "/%s", exports[i]);
        rexports[i] = strdup(realpath(given, real) != NULL ? real + 1 : exports[i]);
    }
    signal(SIGPIPE, SIG_IGN);
    started_at = time(NULL);
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
    printf(keylen ? " (peers must prove a shared key)\n" : " (NO AUTHENTICATION: -I)\n");
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
        int slot = -1;
        const char *why = NULL;
        pthread_mutex_lock(&conns_mtx);
        for (int i = 0; i < MAX_CONNS && slot < 0; i++)
            if (!slot_used[i])
                slot = i;
        if (slot < 0) {
            why = "all slots in use";
        } else if (addr_count(pa.sin_addr, S_AUTH) >= MAX_PER_ADDR) {
            why = "too many from that address";
            slot = -1;
        } else if (keylen) {
            /* A real peer finishes the handshake in milliseconds, a stranger holds on for the full two
             * seconds, so a newcomer is let in and someone is told to go. Never someone from another
             * address than the flooder's: an address first loses its own oldest handshake, and only when
             * the places are all taken by different addresses does the address holding the most lose one.
             * This runs only when a free slot is already in hand, so an eviction always admits somebody. */
            int total = 0, heaviest = -1, heavy_n = 0;
            for (int i = 0; i < MAX_CONNS; i++) {
                if (slot_used[i] && slot_state[i] == S_UNAUTH) {
                    total++;
                    int n = addr_count(slot_addr[i], S_UNAUTH);
                    if (n > heavy_n) {
                        heavy_n = n;
                        heaviest = i;
                    }
                }
            }
            int victim = -1;
            if (addr_count(pa.sin_addr, S_UNAUTH) >= MAX_UNAUTH_PER_ADDR)
                victim = oldest_unauth_locked(pa.sin_addr, 1);
            else if (total >= MAX_UNAUTH && heaviest >= 0)
                victim = oldest_unauth_locked(slot_addr[heaviest], 1);
            if (victim >= 0) {
                evict_locked(victim);
                fprintf(stderr, "crossnode-agent: dropped an unauthenticated connection to make room (too many in the handshake)\n");
            }
        }
        if (slot >= 0) {
            slot_used[slot] = 1;
            slot_conn[slot] = NULL;
            slot_since[slot] = mono_ms();
            slot_addr[slot] = pa.sin_addr;
            slot_state[slot] = keylen ? S_UNAUTH : S_AUTH;
        }
        pthread_mutex_unlock(&conns_mtx);
        if (slot < 0) {
            fprintf(stderr, "crossnode-agent: refused a connection from %s (%s)\n", inet_ntoa(pa.sin_addr), why);
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
            slot_state[slot] = S_FREE;
            pthread_mutex_unlock(&conns_mtx);
            continue;
        }
        c->sock = s;
        c->slot = slot;
        pthread_mutex_lock(&conns_mtx);
        slot_conn[slot] = c;
        pthread_mutex_unlock(&conns_mtx);
        c->refs = 1;
        pthread_mutex_init(&c->mtx, NULL);
        pthread_mutex_init(&c->dir_mtx, NULL);
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
