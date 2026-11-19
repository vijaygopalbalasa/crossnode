/*
 * crossnode-net: makes one peer's exported paths appear under /net/<name>.
 *
 * Open, read, write and devctl messages from unmodified local programs are forwarded to the peer's
 * crossnode-agent. No handler ever waits on the network: it queues the request, keeps the client's
 * rcvid in a pending table and returns _RESMGR_NOREPLY. One I/O thread owns the socket, matches
 * replies, sends liveness probes and rings a pulse; the dispatch thread then replies to the waiting
 * client. So:
 *   - a peer that dies or goes silent fails every waiting call with EHOSTDOWN within CN_DEAD_MS;
 *   - a signal to a waiting client fails its call with EINTR at once (unblock handler);
 *   - close() never waits on the peer.
 *
 * Only root, or members of the group given with -g, may use /net/<name>. Replies from the peer are
 * checked against what was asked before any client sees them.
 *
 * stat() and fstat() are answered by the peer; a directory reads as dirent records built from the peer's
 * entry list. Limits, stated plainly: lseek works only on directories (files answer ESPIPE), there is no
 * pread or pwrite, and one peer per process. The agent must prove it holds the shared key (-k) before any
 * request is sent; -I turns that off. Traffic after the handshake is not encrypted.
 */
struct cn_ocb;
#define IOFUNC_OCB_T struct cn_ocb
#include <dirent.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include <arpa/inet.h>
#include <devctl.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cn_auth.h"
#include "cn_proto.h"

#define CN_PING_MS      500   /* liveness probe period while connected */
#define CN_DEAD_MS     2000   /* silence after which the peer is declared down */
#define CN_CONNECT_MS  1500   /* deadline for a new connection */
#define CN_RETRY_MS    1000   /* after a failure, new calls fail at once for this long */
#define NPEND           128   /* calls waiting on the peer at the same time */

/* OCB_LOCAL: answered here (the mount root's own attributes). OCB_STATPATH: a stat() by name, which
 * arrives as a connect message combined with the stat and a close; the path waits for the stat.
 * OCB_STATUS: DIR/NAME/.status, the link's own state, written by this process and never forwarded. */
enum ocb_state { OCB_LOCAL = 0, OCB_OPENING, OCB_OPEN, OCB_STATPATH, OCB_STATUS };

#define CN_STATUS_NAME ".status"


struct cn_ocb {
    iofunc_ocb_t hdr;         /* must be first */
    int state;
    int32_t rh;               /* the peer's handle, -1 if none */
    uint32_t gen;             /* connection generation the handle belongs to */
    char *path;               /* OCB_STATPATH: the name to stat, relative to the share root */
    int isdir;                /* the peer said the opened object is a directory */
    uint32_t dpos;            /* directories: index of the next entry to hand out */
    char *text;               /* OCB_STATUS: the snapshot taken at open, so one read sees one moment */
    iofunc_notify_t notify[3];/* select, poll and ionotify waiters on this open (input, output, out-of-band) */
    int armed;                /* a NOTIFY request is out at the peer for this open */
    int ready;                /* the peer said input is there; cleared when a read goes out */
    size_t tlen;
};

struct pend {
    int used, done, cancelled;
    int op;
    uint32_t seq;
    rcvid_t rcvid;
    pid_t cpid;               /* the waiting caller's process, thread and connection: an unblock pulse names */
    int ctid, cscoid;         /* so a caller is found by these as well as by rcvid (see cancel_waiting) */
    int err;
    int32_t val;
    uint32_t gen;
    uint32_t req;             /* bytes asked for (READ, DEVCTL, READDIR) or sent (WRITE) */
    uint16_t rflags;          /* flags of the reply */
    uint32_t cookie;          /* READDIR: index of the first entry asked for */
    uint32_t cap;             /* READDIR: bytes the caller's buffer holds */
    io_stat_t sm;             /* STAT: the caller's stat message, which says in what format to answer */
    unsigned char *data;
    uint32_t dlen;
    struct cn_ocb *ocb;       /* OPEN: the OCB bound to the waiting client; READDIR: the directory's OCB */
    struct _msg_info info;    /* OPEN only: that client's connection, to unbind on failure */
};

enum conn_state { C_DOWN = 0, C_CONNECTING, C_AUTH, C_UP };

static struct {
    pthread_mutex_t mtx;      /* guards everything in this struct */
    int state;
    int sock;
    uint32_t gen;             /* bumped on every disconnect */
    uint32_t seq;
    uint64_t last_fail, last_rx, last_ping, connect_deadline;
    int want_connect;
    int ring;                 /* completions are waiting for the dispatch thread */
    unsigned char *out;
    size_t outlen, outcap;
    unsigned char *wire;             /* keyed links: whole frames with their tags, in the order they go out */
    size_t wirelen, wirecap;
    unsigned char ktx[32], krx[32];  /* frame keys of this connection: to the agent, from the agent */
    uint64_t ctx, crx;               /* frames sent and received since the handshake */
    unsigned char rx[sizeof(struct cn_hdr) + CN_MAX_DATA + CN_TAG_LEN];
    size_t rxlen;
    struct pend pend[NPEND];
    int wake[2];
    struct sockaddr_in peer;
    int self_coid;
    int pulse_code;
    unsigned char key[CN_KEY_MAX];   /* -k; keylen 0 means -I, no authentication */
    size_t keylen;
    int auth_stage;                  /* 0: waiting for the agent's challenge, 1: waiting for its proof */
    unsigned char na[CN_NONCE_LEN], nc[CN_NONCE_LEN];
    unsigned char authout[sizeof(struct cn_hdr) + CN_NONCE_LEN + CN_MAC_LEN];
    size_t authlen;
    const char *why;                 /* set when a frame is refused: the reason for the log */
    /* For DIR/NAME/.status only; nothing else reads these. */
    uint64_t up_at, down_at;         /* when the link last came up, last went down */
    unsigned ups, drops, refused;    /* times up, times dropped after being up or authenticating, frames refused */
    char last_drop[128], last_refused[128];
    char last_connect[128];          /* why the latest connection attempt failed; cleared when the link comes up */
} G = { .mtx = PTHREAD_MUTEX_INITIALIZER, .sock = -1 };

static iofunc_attr_t dir_attr, file_attr, status_attr;
static char peer_text[64];               /* ADDRESS:PORT as given with -c, for .status */
static iofunc_mount_t cn_mount;
static iofunc_funcs_t cn_ocb_funcs;
static resmgr_connect_funcs_t cf;
static resmgr_io_funcs_t iof;
static const char *name;
static int verbose;
static gid_t allowed_gid = (gid_t)-1;   /* -g: this group may use the path as well as root */
static int io_prio;                      /* -r: the I/O thread's priority; 0 = this process's own */

static void vlog(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "crossnode-net[%s]: ", name);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static struct cn_ocb *ocb_new(void)
{
    struct cn_ocb *o = calloc(1, sizeof *o);
    if (o != NULL)
        o->rh = -1;
    return o;
}

static void ocb_destroy(struct cn_ocb *o)
{
    free(o->path);
    free(o->text);
    free(o);
}

static IOFUNC_OCB_T *cn_ocb_calloc(resmgr_context_t *ctp, IOFUNC_ATTR_T *attr)
{
    (void)ctp;
    (void)attr;
    return ocb_new();
}

static void cn_ocb_free(IOFUNC_OCB_T *ocb)
{
    ocb_destroy(ocb);
}

/* ---- I/O thread side (all called with G.mtx held) ---------------------------------------------- */

static int out_append(const struct cn_hdr *h, const void *p, size_t n)
{
    size_t need = G.outlen + sizeof *h + n;
    if (need > G.outcap) {
        size_t cap = G.outcap ? G.outcap : 4096;
        while (cap < need)
            cap *= 2;
        unsigned char *q = realloc(G.out, cap);
        if (q == NULL)
            return -1;
        G.out = q;
        G.outcap = cap;
    }
    memcpy(G.out + G.outlen, h, sizeof *h);
    G.outlen += sizeof *h;
    if (n) {
        memcpy(G.out + G.outlen, p, n);
        G.outlen += n;
    }
    return 0;
}

static int wire_append(const void *p, size_t n)
{
    size_t need = G.wirelen + n;
    if (need > G.wirecap) {
        size_t cap = G.wirecap ? G.wirecap : 4096;
        while (cap < need)
            cap *= 2;
        unsigned char *q = realloc(G.wire, cap);
        if (q == NULL)
            return -1;
        G.wire = q;
        G.wirecap = cap;
    }
    memcpy(G.wire + G.wirelen, p, n);
    G.wirelen += n;
    return 0;
}

static void queue_close_locked(int32_t rh)
{
    struct cn_hdr h = { .magic = CN_MAGIC, .op = CN_OP_CLOSE, .seq = ++G.seq, .handle = rh };
    (void)out_append(&h, NULL, 0);
}

static void teardown_locked(const char *why)
{
    if (G.sock >= 0) {
        close(G.sock);
        G.sock = -1;
    }
    if (G.state == C_AUTH)
        vlog("peer down: authentication failed (%s)\n", why);
    else if (G.state != C_DOWN)
        vlog("peer down: %s\n", why);
    if (G.state == C_UP || G.state == C_AUTH) {
        G.drops++;
        snprintf(G.last_drop, sizeof G.last_drop, "%s%s", G.state == C_AUTH ? "authentication failed: " : "", why);
    } else if (G.state == C_CONNECTING) {
        snprintf(G.last_connect, sizeof G.last_connect, "%s", why);
    }
    if (G.state != C_DOWN)
        G.down_at = now_ms();
    G.authlen = 0;
    G.auth_stage = 0;
    G.state = C_DOWN;
    G.gen++;
    G.last_fail = now_ms();
    G.outlen = 0;
    G.wirelen = 0;
    G.rxlen = 0;
    G.ctx = G.crx = 0;
    memset(G.ktx, 0, sizeof G.ktx);
    memset(G.krx, 0, sizeof G.krx);
    for (int i = 0; i < NPEND; i++) {
        struct pend *e = &G.pend[i];
        if (e->used && !e->done) {
            e->done = 1;
            e->err = EHOSTDOWN;
            G.ring = 1;
        }
    }
}

static void up_locked(void)
{
    if (G.keylen) {          /* the handshake just finished: every later frame carries a tag */
        cn_frame_keys(G.key, G.keylen, G.na, G.nc, G.ktx, G.krx);
        G.ctx = G.crx = 0;
    }
    G.state = C_UP;
    G.last_rx = G.last_ping = G.up_at = now_ms();
    G.ups++;
    G.last_connect[0] = '\0';
    vlog("peer up\n");
}

static void connected_locked(void)
{
    if (G.keylen == 0) {
        up_locked();
        return;
    }
    G.state = C_AUTH;               /* nothing but the handshake until the agent has proved the key */
    G.auth_stage = 0;
    G.authlen = 0;
}

static void start_connect_locked(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        teardown_locked("no socket");
        return;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    G.sock = s;
    G.state = C_CONNECTING;
    G.connect_deadline = now_ms() + CN_CONNECT_MS;
    if (connect(s, (struct sockaddr *)&G.peer, sizeof G.peer) == 0)
        connected_locked();
    else if (errno != EINPROGRESS)
        teardown_locked(strerror(errno));
}

static struct pend *find_seq_locked(uint32_t seq)
{
    for (int i = 0; i < NPEND; i++) {
        if (G.pend[i].used && G.pend[i].seq == seq)
            return &G.pend[i];
    }
    return NULL;
}

static int stat_valid(const unsigned char *p)
{
    struct cn_stat st;
    memcpy(&st, p, sizeof st);
    switch (st.mode & S_IFMT) {
    case S_IFREG: case S_IFDIR: case S_IFCHR: case S_IFBLK: case S_IFIFO: case S_IFLNK: case S_IFSOCK:
        break;
    default:
        return 0;
    }
    return st.size < ((uint64_t)1 << 62);
}

/* Entries must be whole, in bounds, with a usable name, and add up to the count the header gives. */
static int dents_valid(const unsigned char *p, uint32_t len, int32_t count)
{
    uint32_t off = 0;
    int32_t n = 0;
    while (off < len) {
        struct cn_dent d;
        if (len - off < sizeof d)
            return 0;
        memcpy(&d, p + off, sizeof d);
        if (d.namelen == 0 || d.namelen > CN_MAX_NAME || d.reserved != 0)
            return 0;
        size_t sz = CN_DENT_SIZE(d.namelen);
        if (len - off < sz)
            return 0;
        const unsigned char *name = p + off + sizeof d;
        for (uint32_t i = 0; i < d.namelen; i++) {
            if (name[i] == '\0' || name[i] == '/')
                return 0;
        }
        off += (uint32_t)sz;
        n++;
    }
    return off == len && n == count;
}

/* A reply must match its request: same operation, no more bytes than asked, a sane count. */
static int reply_valid(const struct pend *e, const struct cn_hdr *h, const unsigned char *p)
{
    if (h->op != (uint16_t)(e->op | CN_REPLY) || h->a < 0)
        return 0;
    if (h->a != 0)
        return h->len == 0;
    switch (e->op) {
    case CN_OP_OPEN:    return h->len == 0 && h->b >= 0 && h->b < CN_MAX_HANDLES && (h->flags & ~CN_F_DIR) == 0;
    case CN_OP_READ:    return h->len <= e->req && h->b == (int32_t)h->len;
    case CN_OP_WRITE:   return h->len == 0 && h->b >= 0 && (uint32_t)h->b <= e->req;
    case CN_OP_DEVCTL:  return h->len == 0 || h->len == e->req;
    case CN_OP_STAT:    return h->len == sizeof(struct cn_stat) && stat_valid(p);
    case CN_OP_READDIR: return h->len <= e->req && h->b >= 0 && dents_valid(p, h->len, h->b);
    case CN_OP_NOTIFY:  return h->len == 0;
    default:            return 0;
    }
}

/* One frame while authenticating. Returns -1 to drop the connection. */
static int handle_auth_frame_locked(const struct cn_hdr *h, const unsigned char *p)
{
    if (h->op != (CN_OP_AUTH | CN_REPLY))
        return -1;
    if (G.auth_stage == 0) {
        if (h->len != CN_NONCE_LEN)
            return -1;
        memcpy(G.na, p, CN_NONCE_LEN);
        if (cn_random(G.nc, CN_NONCE_LEN) != 0)
            return -1;
        struct cn_hdr r = { .magic = CN_MAGIC, .op = CN_OP_AUTH, .seq = ++G.seq, .handle = -1,
                            .len = CN_NONCE_LEN + CN_MAC_LEN };
        memcpy(G.authout, &r, sizeof r);
        memcpy(G.authout + sizeof r, G.nc, CN_NONCE_LEN);
        cn_auth_mac(G.key, G.keylen, 'C', G.na, G.nc, G.authout + sizeof r + CN_NONCE_LEN);
        G.authlen = sizeof r + CN_NONCE_LEN + CN_MAC_LEN;
        G.auth_stage = 1;
        return 0;
    }
    unsigned char want[CN_MAC_LEN];
    if (h->a != 0 || h->len != CN_MAC_LEN)
        return -1;
    cn_auth_mac(G.key, G.keylen, 'A', G.nc, G.na, want);
    if (!cn_ct_equal(want, p, CN_MAC_LEN))
        return -1;
    up_locked();
    return 0;
}

/* Returns -1 when the frame breaks the protocol; the caller then drops the connection. */
static int handle_frame_locked(const struct cn_hdr *h, const unsigned char *p)
{
    G.last_rx = now_ms();
    if (G.state == C_AUTH)
        return handle_auth_frame_locked(h, p);
    if (h->op == (CN_OP_AUTH | CN_REPLY)) {
        /* The agent opened with a challenge, so it wants a key and this side was started with -I. */
        G.why = "the agent requires authentication (start crossnode-net with -k KEYFILE)";
        return -1;
    }
    if (h->op == (CN_OP_PING | CN_REPLY))
        return 0;
    struct pend *e = find_seq_locked(h->seq);
    if (e == NULL || e->done) {
        /* Nobody waits for this answer any more; never leak a handle the peer just opened. */
        if (h->op == (CN_OP_OPEN | CN_REPLY) && h->a == 0 && h->b >= 0)
            queue_close_locked(h->b);
        return 0;
    }
    if (!reply_valid(e, h, p))
        return -1;
    if (e->cancelled) {
        if (e->op == CN_OP_OPEN && h->a == 0)
            queue_close_locked(h->b);
        e->done = 1;
        G.ring = 1;
        return 0;
    }
    e->err = h->a;
    e->val = h->b;
    e->rflags = h->flags;
    e->gen = G.gen;
    if (h->len) {
        e->data = malloc(h->len);
        if (e->data != NULL) {
            memcpy(e->data, p, h->len);
            e->dlen = h->len;
        } else {
            e->err = ENOMEM;
        }
    }
    e->done = 1;
    G.ring = 1;
    return 0;
}

static void refuse_locked(const char *why)
{
    G.refused++;
    snprintf(G.last_refused, sizeof G.last_refused, "%s", why);
    teardown_locked(why);
}

static void io_read_locked(void)
{
    for (;;) {
        ssize_t r = recv(G.sock, G.rx + G.rxlen, sizeof G.rx - G.rxlen, 0);
        if (r == 0) {
            teardown_locked("the peer closed the connection");
            return;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                teardown_locked(strerror(errno));
            return;
        }
        G.rxlen += (size_t)r;
        size_t off = 0;
        while (G.rxlen - off >= sizeof(struct cn_hdr)) {
            struct cn_hdr h;
            memcpy(&h, G.rx + off, sizeof h);
            if (h.magic != CN_MAGIC || !(h.op & CN_REPLY) || h.len > CN_MAX_DATA) {
                refuse_locked("protocol error");
                return;
            }
            size_t tag = G.state == C_UP && G.keylen ? CN_TAG_LEN : 0;   /* the handshake frames carry none */
            if (G.rxlen - off < sizeof h + h.len + tag)
                break;
            if (tag) {
                unsigned char want[CN_TAG_LEN];
                cn_frame_tag(G.krx, G.crx++, G.rx + off, G.rx + off + sizeof h, h.len, want);
#ifndef CN_NO_VERIFY
                if (!cn_ct_equal(want, G.rx + off + sizeof h + h.len, CN_TAG_LEN)) {
                    refuse_locked("a frame failed its integrity check (changed, replayed or out of order)");
                    return;
                }
#endif
            }
            int was_auth = G.state == C_AUTH;
            if (handle_frame_locked(&h, G.rx + off + sizeof h) != 0) {
                refuse_locked(G.why ? G.why : was_auth ? "the agent's answer was wrong" : "a reply did not match its request");
                G.why = NULL;
                return;
            }
            off += sizeof h + h.len + tag;
        }
        memmove(G.rx, G.rx + off, G.rxlen - off);
        G.rxlen -= off;
    }
}

static void io_write_locked(void)
{
    if (G.state == C_AUTH) {                 /* only the handshake may be sent before the agent is trusted */
        if (G.authlen) {
            ssize_t w = send(G.sock, G.authout, G.authlen, 0);
            if (w > 0) {
                memmove(G.authout, G.authout + w, G.authlen - (size_t)w);
                G.authlen -= (size_t)w;
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                teardown_locked(strerror(errno));
            }
        }
        return;
    }
    if (G.keylen == 0) {                     /* -I: frames go out as they are */
        ssize_t w = send(G.sock, G.out, G.outlen, 0);
        if (w > 0) {
            memmove(G.out, G.out + w, G.outlen - (size_t)w);
            G.outlen -= (size_t)w;
        } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            teardown_locked(strerror(errno));
        }
        return;
    }
    /* Keyed: each whole frame gets its tag as it moves from the queue to the wire, so the counter follows the order
     * in which frames reach the agent (only this thread sends, under the lock). */
    size_t off = 0;
    while (G.outlen - off >= sizeof(struct cn_hdr)) {
        struct cn_hdr h;
        memcpy(&h, G.out + off, sizeof h);
        size_t fl = sizeof h + h.len;
        if (G.outlen - off < fl)
            break;
        unsigned char tag[CN_TAG_LEN];
        cn_frame_tag(G.ktx, G.ctx++, G.out + off, G.out + off + sizeof h, h.len, tag);
        if (wire_append(G.out + off, fl) != 0 || wire_append(tag, sizeof tag) != 0) {
            teardown_locked("out of memory");
            return;
        }
        off += fl;
    }
    memmove(G.out, G.out + off, G.outlen - off);
    G.outlen -= off;
    if (G.wirelen == 0)
        return;
    ssize_t w = send(G.sock, G.wire, G.wirelen, 0);
    if (w > 0) {
        memmove(G.wire, G.wire + w, G.wirelen - (size_t)w);
        G.wirelen -= (size_t)w;
    } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        teardown_locked(strerror(errno));
    }
}

static void *io_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct pollfd fds[2];
        int nfds = 1;
        pthread_mutex_lock(&G.mtx);
        uint64_t t = now_ms();
        if (G.state == C_DOWN && G.want_connect) {
            G.want_connect = 0;
            start_connect_locked();
        }
        if (G.state == C_CONNECTING && t >= G.connect_deadline)
            teardown_locked("connect timed out");
        if (G.state == C_AUTH && t >= G.connect_deadline)
            teardown_locked("timed out");
        if (G.state == C_UP) {
#ifndef CN_NO_LIVENESS
            if (t - G.last_rx >= CN_DEAD_MS) {
                teardown_locked("no answer within the liveness limit");
            } else
#endif
            if (t - G.last_ping >= CN_PING_MS) {
                struct cn_hdr h = { .magic = CN_MAGIC, .op = CN_OP_PING, .seq = ++G.seq, .handle = -1 };
                (void)out_append(&h, NULL, 0);
                G.last_ping = t;
            }
        }
        fds[0].fd = G.wake[0];
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (G.sock >= 0) {
            fds[1].fd = G.sock;
            size_t pending = G.state == C_AUTH ? G.authlen : G.outlen + G.wirelen;
            fds[1].events = (short)(G.state == C_CONNECTING ? POLLOUT : (POLLIN | (pending ? POLLOUT : 0)));
            fds[1].revents = 0;
            nfds = 2;
        }
        /* Sleep exactly until the next deadline, so the limits hold to the millisecond. */
        int64_t wait = 1000;
        if (G.state == C_CONNECTING || G.state == C_AUTH)
            wait = (int64_t)G.connect_deadline - (int64_t)t;
        if (G.state == C_UP) {
            int64_t dead = (int64_t)(G.last_rx + CN_DEAD_MS) - (int64_t)t;
            int64_t ping = (int64_t)(G.last_ping + CN_PING_MS) - (int64_t)t;
#ifndef CN_NO_LIVENESS
            wait = dead < ping ? dead : ping;
#else
            (void)dead;
            wait = ping;
#endif
        }
        if (wait < 0)
            wait = 0;
        if (wait > 1000)
            wait = 1000;
        int ring = G.ring;
        G.ring = 0;
        pthread_mutex_unlock(&G.mtx);

        if (ring)
            MsgSendPulse(G.self_coid, -1, G.pulse_code, 0);
        (void)poll(fds, (nfds_t)nfds, (int)wait);

        pthread_mutex_lock(&G.mtx);
        if (fds[0].revents & POLLIN) {
            char b[64];
            while (read(G.wake[0], b, sizeof b) > 0)
                ;
        }
        if (nfds == 2 && fds[1].revents != 0 && G.sock == fds[1].fd) {
            short re = fds[1].revents;
            if (G.state == C_CONNECTING) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(G.sock, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err == 0 && (re & POLLOUT))
                    connected_locked();
                else
                    teardown_locked(err ? strerror(err) : "connect failed");
            } else if (G.state == C_UP || G.state == C_AUTH) {
                if (re & POLLIN)
                    io_read_locked();
                if ((G.state == C_UP || G.state == C_AUTH) && (re & POLLOUT) &&
                    (G.state == C_AUTH ? G.authlen : G.outlen + G.wirelen))
                    io_write_locked();
                if ((G.state == C_UP || G.state == C_AUTH) && (re & (POLLERR | POLLHUP | POLLNVAL)) && !(re & POLLIN))
                    teardown_locked("connection error");
            }
        }
        pthread_mutex_unlock(&G.mtx);
    }
    return NULL;
}

static void wake_io(void)
{
    char b = 1;
    (void)write(G.wake[1], &b, 1);
}

/* ---- dispatch thread side ------------------------------------------------------------------------ */

/*
 * Queue one request. gen_ocb, when given, must belong to the live connection; open_ocb is the OCB
 * an OPEN binds. Returns _RESMGR_NOREPLY when the client now waits on the pending table, or an errno.
 */
struct aux {
    const io_stat_t *sm;      /* STAT: the caller's message */
    uint32_t cap;             /* READDIR: the caller's buffer size */
};

static int submit(resmgr_context_t *ctp, int op, struct cn_ocb *gen_ocb, struct cn_ocb *open_ocb,
                  int32_t handle, int32_t a, int32_t b, const void *payload, uint32_t plen, uint32_t req,
                  const struct aux *x)
{
    pthread_mutex_lock(&G.mtx);
    if (gen_ocb != NULL && gen_ocb->gen != G.gen) {
        /* The handle came from an earlier connection: the peer is down, or it came back and forgot it. */
        int err = (G.state == C_UP) ? ESTALE : EHOSTDOWN;
        pthread_mutex_unlock(&G.mtx);
        return err;
    }
    if (G.state == C_DOWN) {
        if (G.last_fail != 0 && now_ms() - G.last_fail < CN_RETRY_MS) {
            pthread_mutex_unlock(&G.mtx);
            return EHOSTDOWN;
        }
        G.want_connect = 1;
    }
    struct pend *e = NULL;
    for (int i = 0; i < NPEND; i++) {
        if (!G.pend[i].used) {
            e = &G.pend[i];
            break;
        }
    }
    if (e == NULL) {
        pthread_mutex_unlock(&G.mtx);
        return EAGAIN;
    }
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->op = op;
    e->seq = ++G.seq;
    e->rcvid = ctp->rcvid;
    e->cpid = ctp->info.pid;
    e->ctid = ctp->info.tid;
    e->cscoid = ctp->info.scoid;
    e->req = req;
    e->cookie = (uint32_t)a;
    if (x != NULL) {
        e->cap = x->cap;
        if (x->sm != NULL)
            e->sm = *x->sm;
    }
    if (open_ocb != NULL) {
        e->ocb = open_ocb;
        e->info = ctp->info;
    }
    struct cn_hdr h = {
        .magic = CN_MAGIC, .op = (uint16_t)op, .seq = e->seq, .handle = handle, .a = a, .b = b, .len = plen,
    };
#ifndef CN_NO_PRIO
    /* Carry the caller's priority, so the peer can do the work at it (crossnode-agent -P). */
    if (ctp->info.priority > 0)
        h.flags = (uint16_t)(CN_F_PRIO | (ctp->info.priority & CN_PRIO_MASK));
#endif
    if (out_append(&h, payload, plen) != 0) {
        e->used = 0;
        pthread_mutex_unlock(&G.mtx);
        return ENOMEM;
    }
    pthread_mutex_unlock(&G.mtx);
    wake_io();
    return _RESMGR_NOREPLY;
}

/* Undo an OCB bound for an open that did not succeed: remove the client mapping, then free it. */
/* Ask the peer to answer when this open has input. No client waits on the answer: it wakes the open's select, poll
 * and ionotify waiters. On a link failure the entry fails like any other, which wakes them too. */
static int notify_peer(struct cn_ocb *ocb)
{
#ifdef CN_NO_NOTIFY
    (void)ocb;
    return -1;                  /* mutation build: never asks, so a select on a peer's device never wakes */
#endif
    pthread_mutex_lock(&G.mtx);
    if (ocb->gen != G.gen || G.state != C_UP) {
        pthread_mutex_unlock(&G.mtx);
        return -1;
    }
    struct pend *e = NULL;
    for (int i = 0; i < NPEND; i++) {
        if (!G.pend[i].used) {
            e = &G.pend[i];
            break;
        }
    }
    if (e == NULL) {
        pthread_mutex_unlock(&G.mtx);
        return -1;
    }
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->op = CN_OP_NOTIFY;
    e->seq = ++G.seq;
    e->ocb = ocb;
    struct cn_hdr h = { .magic = CN_MAGIC, .op = CN_OP_NOTIFY, .seq = e->seq, .handle = ocb->rh };
    if (out_append(&h, NULL, 0) != 0) {
        e->used = 0;
        pthread_mutex_unlock(&G.mtx);
        return -1;
    }
    pthread_mutex_unlock(&G.mtx);
    wake_io();
    return 0;
}

static void drop_opening_ocb(resmgr_context_t *ctp, const struct _msg_info *info, struct cn_ocb *ocb)
{
    struct _msg_info keep = ctp->info;
    ctp->info = *info;
    if (resmgr_unbind(ctp) != 0)
        vlog("unbind after a failed open: %s\n", strerror(errno));
    iofunc_attr_t *attr = ocb->hdr.attr;
    iofunc_attr_lock(attr);
    (void)iofunc_ocb_detach(ctp, &ocb->hdr);
    iofunc_attr_unlock(attr);
    ctp->info = keep;
    ocb_destroy(ocb);
}

static void complete(resmgr_context_t *ctp, struct pend *e)
{
    switch (e->op) {
    case CN_OP_OPEN:
        if (e->err == EOK) {
            e->ocb->rh = e->val;
            e->ocb->gen = e->gen;
            e->ocb->isdir = (e->rflags & CN_F_DIR) != 0;
            e->ocb->state = OCB_OPEN;
            MsgReply(e->rcvid, EOK, NULL, 0);
        } else {
            drop_opening_ocb(ctp, &e->info, e->ocb);
            MsgError(e->rcvid, e->err);
        }
        break;
    case CN_OP_READ:
        if (e->err != EOK)
            MsgError(e->rcvid, e->err);
        else
            MsgReply(e->rcvid, (long)e->dlen, e->data, e->dlen);
        break;
    case CN_OP_WRITE:
        if (e->err != EOK)
            MsgError(e->rcvid, e->err);
        else
            MsgReply(e->rcvid, e->val, NULL, 0);
        break;
    case CN_OP_DEVCTL: {
        if (e->err != EOK) {
            MsgError(e->rcvid, e->err);
            break;
        }
        struct _io_devctl_reply r;
        memset(&r, 0, sizeof r);
        r.ret_val = e->val;
        r.nbytes = e->dlen;
        iov_t iov[2];
        SETIOV(&iov[0], &r, sizeof r);
        SETIOV(&iov[1], e->data, e->dlen);
        MsgReplyv(e->rcvid, EOK, iov, e->dlen ? 2 : 1);
        break;
    }
    case CN_OP_STAT: {
        if (e->err != EOK) {
            MsgError(e->rcvid, e->err);
            break;
        }
        /* Let the library format the answer the way this caller asked (it knows the stat layouts); the
         * attributes themselves are the peer's. */
        struct cn_stat cs;
        memcpy(&cs, e->data, sizeof cs);
        iofunc_attr_t a;
        iofunc_attr_init(&a, (mode_t)cs.mode, NULL, NULL);
        a.mount = &cn_mount;
        a.mode = (mode_t)cs.mode;
        a.uid = (uid_t)cs.uid;
        a.gid = (gid_t)cs.gid;
        a.nlink = (nlink_t)cs.nlink;
        a.rdev = (dev_t)cs.rdev;
        a.nbytes = (off_t)cs.size;
        a.inode = (ino_t)cs.ino;
        a.atime = (time_t)cs.atime;
        a.mtime = (time_t)cs.mtime;
        a.ctime = (time_t)cs.ctime;
        a.mtime_ns = a.atime_ns = a.ctime_ns = 0;
        a.flags = 0;                    /* nothing is dirty: report the peer's times as they are */
        iofunc_ocb_t fake;
        memset(&fake, 0, sizeof fake);
        fake.attr = &a;
        /* The library formats the answer in the layout the caller asked for (it reads that from the saved
         * request) and leaves the layout in the context's status, which is the reply status. It writes the
         * context's iov array, so the context needs room for one. */
        union {
            resmgr_context_t c;
            unsigned char room[sizeof(resmgr_context_t) + 2 * sizeof(iov_t)];
        } sc;
        memset(&sc, 0, sizeof sc);
        memcpy(&sc, ctp, sizeof(resmgr_context_t));
        int r = iofunc_stat_default(&sc.c, &e->sm, &fake);
        if (r == _RESMGR_NPARTS(1))
            MsgReplyv(e->rcvid, sc.c.status, sc.c.iov, 1);
        else
            MsgError(e->rcvid, r > 0 ? r : EIO);
        break;
    }
    case CN_OP_READDIR: {
        if (e->err != EOK) {
            MsgError(e->rcvid, e->err);
            break;
        }
        /* Turn the peer's neutral entries into this system's dirent records, as many as the caller's
         * buffer holds; the rest are asked for again by the next read. */
        unsigned char *buf = malloc(e->cap ? e->cap : 1);
        if (buf == NULL) {
            MsgError(e->rcvid, ENOMEM);
            break;
        }
        size_t used = 0, off = 0;
        uint32_t taken = 0;
        while (off < e->dlen) {
            struct cn_dent d;
            memcpy(&d, e->data + off, sizeof d);
            size_t reclen = dirent_calc_reclen((size_t)d.namelen + 1);
            if (used + reclen > e->cap)
                break;
            struct dirent *de = (struct dirent *)(void *)(buf + used);
            memset(de, 0, reclen);
            de->d_ino = (ino_t)d.ino;
            de->d_offset = (off_t)e->cookie + (off_t)taken + 1;
            de->d_reclen = (int16_t)reclen;
            de->d_namelen = (int16_t)d.namelen;
            memcpy(de->d_name, e->data + off + sizeof d, d.namelen);
            used += reclen;
            off += CN_DENT_SIZE(d.namelen);
            taken++;
        }
        if (e->val > 0 && taken == 0) {
            MsgError(e->rcvid, EINVAL);     /* the first entry does not fit in the caller's buffer */
        } else {
            pthread_mutex_lock(&G.mtx);
            if (e->ocb != NULL && e->ocb->dpos == e->cookie)
                e->ocb->dpos += taken;
            pthread_mutex_unlock(&G.mtx);
            MsgReply(e->rcvid, (long)used, buf, used);
        }
        free(buf);
        break;
    }
    case CN_OP_NOTIFY:
        /* No client waits on this one: wake every select, poll or ionotify waiter on the open. When the link failed,
         * wake them too, so their read fails with the reason instead of waiting forever. */
        if (e->ocb != NULL) {
            e->ocb->armed = 0;
            e->ocb->ready = 1;
            iofunc_notify_trigger(e->ocb->notify, 1, IOFUNC_NOTIFY_INPUT);
        }
        break;
    default:
        MsgError(e->rcvid, ENOSYS);
        break;
    }
}

/* Pulse from the I/O thread: answer every client whose request finished or failed. */
static int on_doorbell(message_context_t *mctp, int code, unsigned flags, void *handle)
{
    (void)code;
    (void)flags;
    (void)handle;
    resmgr_context_t *ctp = (resmgr_context_t *)mctp;
    for (;;) {
        struct pend e;
        int found = 0;
        pthread_mutex_lock(&G.mtx);
        for (int i = 0; i < NPEND; i++) {
            struct pend *p = &G.pend[i];
            if (p->used && p->done) {
                e = *p;
                p->used = 0;
                p->data = NULL;
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&G.mtx);
        if (!found)
            break;
        if (!e.cancelled)
            complete(ctp, &e);
        free(e.data);
    }
    return 0;
}

/* A waiting client got a signal (or is being killed): answer EINTR now and forget the request. */
static void cancel_waiting(resmgr_context_t *ctp, rcvid_t rcvid)
{
    struct pend *e = NULL;
    struct cn_ocb *ocb = NULL;
    struct _msg_info info;
    rcvid_t reply_to = rcvid;
    int op = 0;
    /* An unblock pulse can come for a caller inside a combined message (stat by name), so a caller is found
     * by the process, thread and connection the pulse's info names, not only by rcvid. A thread has one call
     * outstanding, so this is unambiguous. */
    const struct _msg_info *who = &ctp->info;
    pthread_mutex_lock(&G.mtx);
    for (int i = 0; i < NPEND; i++) {
        struct pend *p = &G.pend[i];
        if (p->used && !p->cancelled && !p->done && p->op != CN_OP_NOTIFY &&
            (p->rcvid == rcvid || (p->cpid == who->pid && p->ctid == who->tid && p->cscoid == who->scoid))) {
            e = p;
            break;
        }
    }
    if (e != NULL) {
        /* A pulse can be stale: if the call it was for was answered and the same thread's next call was
         * received before the pulse, the match above is that next call. Let a caller go only if the kernel
         * says it asked to be (or is gone). */
        struct _msg_info mi;
        if (MsgInfo(e->rcvid, &mi) == 0 && !(mi.flags & _NTO_MI_UNBLOCK_REQ)) {
            pthread_mutex_unlock(&G.mtx);
            return;
        }
        e->cancelled = 1;
        reply_to = e->rcvid;            /* the id the caller is actually blocked on */
        op = e->op;
        ocb = e->ocb;
        e->ocb = NULL;
        info = e->info;
        if (G.state == C_UP) {
            /* Tell the peer to stop the work too, so a device call that never returns is let go. */
            struct cn_hdr h = { .magic = CN_MAGIC, .op = CN_OP_CANCEL, .seq = ++G.seq, .handle = -1,
                                .a = (int32_t)e->seq };
            (void)out_append(&h, NULL, 0);
        }
        /* Free the slot now. A late answer then matches nothing, and a late successful open is
         * closed on the peer (handle_frame_locked), so nothing leaks either way. */
        e->done = 1;
        G.ring = 1;
    }
    pthread_mutex_unlock(&G.mtx);
    if (e == NULL)
        return;
    if (op == CN_OP_OPEN && ocb != NULL) {
        /* The library holds the binding while an unblock is handled and closes the OCB itself when
         * it lets go. So only the client mapping is removed here; freeing the OCB as well would free
         * it twice. */
        ocb->state = OCB_LOCAL;
        struct _msg_info keep = ctp->info;
        ctp->info = info;
        if (resmgr_unbind(ctp) != 0)
            vlog("unbind after an interrupted open: %s\n", strerror(errno));
        ctp->info = keep;
    }
    MsgError(reply_to, EINTR);
    wake_io();
}

static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, struct cn_ocb *ocb)
{
    (void)ocb;
    cancel_waiting(ctp, (rcvid_t)msg->value.sival_long);
    return _RESMGR_NOREPLY;
}

static int connect_unblock(resmgr_context_t *ctp, io_pulse_t *msg, iofunc_attr_t *handle, void *reserved)
{
    (void)handle;
    (void)reserved;
    cancel_waiting(ctp, (rcvid_t)msg->value.sival_long);
    return _RESMGR_NOREPLY;
}

/* DIR/NAME/.status: what an operator asks first. Plain text, one fact per line, a snapshot of one moment. */
static char *status_text(size_t *len)
{
    static const char *const names[] = { "down", "connecting", "authenticating", "up" };
    char b[1024];
    size_t n = 0;
#define ADD(...) do { if (n < sizeof b) n += (size_t)snprintf(b + n, sizeof b - n, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&G.mtx);
    uint64_t t = now_ms();
    int waiting = 0;
    for (int i = 0; i < NPEND; i++)
        if (G.pend[i].used && !G.pend[i].done)
            waiting++;
    ADD("peer        %s at %s\n", name, peer_text);
    if (G.state == C_UP)
        ADD("state       up for %llu s\n", (unsigned long long)((t - G.up_at) / 1000u));
    else if (G.state == C_DOWN && G.down_at != 0)
        ADD("state       down for %llu s (the next call reconnects)\n", (unsigned long long)((t - G.down_at) / 1000u));
    else if (G.state == C_DOWN)
        ADD("state       not connected yet (the first call connects)\n");
    else
        ADD("state       %s\n", names[G.state]);
    ADD("link        %s\n", G.keylen ? "authenticated; every frame carries an HMAC-SHA256 tag"
                                     : "NOT AUTHENTICATED (-I)");
    if (G.state == C_UP)
        ADD("last heard  %s (down after %d s of silence)\n", t - G.last_rx < 1000 ? "within the last second" : "over a second ago",
            CN_DEAD_MS / 1000);
    ADD("waiting     %d call%s\n", waiting, waiting == 1 ? "" : "s");
    ADD("connects    %u\n", G.ups);
    ADD("drops       %u\n", G.drops);
    if (G.last_drop[0])
        ADD("last down   %s\n", G.last_drop);
    if (G.state != C_UP && G.last_connect[0])
        ADD("retry       the last attempt failed: %s\n", G.last_connect);
    ADD("refused     %u frame%s%s%s\n", G.refused, G.refused == 1 ? "" : "s", G.last_refused[0] ? ", last: " : "",
        G.last_refused);
    pthread_mutex_unlock(&G.mtx);
#undef ADD
    if (n >= sizeof b)
        n = sizeof b - 1;
    char *r = malloc(n + 1);
    if (r != NULL) {
        memcpy(r, b, n);
        r[n] = '\0';
        *len = n;
    }
    return r;
}

/* Root, or the -g group, as file_attr's owner and mode say; checked before anything is forwarded. */
static int check_client(resmgr_context_t *ctp, unsigned ioflag)
{
    struct _client_info *ci = NULL;
    int st = iofunc_client_info_ext(ctp, 0, &ci, IOFUNC_CLIENTINFO_GETGROUPS);
    if (st != EOK)
        return st;
    mode_t want = S_IREAD;
    if (ioflag & _IO_FLAG_WR)
        want |= S_IWRITE;
    st = iofunc_check_access(ctp, &file_attr, want, ci);
    iofunc_client_info_ext_free(&ci);
    return st;
}

static int cn_open(resmgr_context_t *ctp, io_open_t *msg, iofunc_attr_t *handle, void *extra)
{
    (void)handle;
    const char *path = msg->connect.path;
    int plain = msg->connect.subtype == _IO_CONNECT_OPEN;
    /* The mount root's own attributes are answered here, so they show the access policy and work with
     * the peer down; opening it to list it is forwarded like any other open. */
    if (path[0] == '\0' && !plain)
        return iofunc_open_default(ctp, msg, &dir_attr, extra);
    if (plain || msg->connect.subtype == _IO_CONNECT_COMBINE || msg->connect.subtype == _IO_CONNECT_COMBINE_CLOSE) {
        /* fall through */
    } else {
        return ENOSYS;                  /* unlink, rename, mknod, readlink, link: nothing is forwarded */
    }
    int allowed = check_client(ctp, msg->connect.ioflag);
    if (allowed != EOK)
        return allowed;
    size_t plen = strlen(path);
    if (plen > CN_MAX_PATH)
        return ENAMETOOLONG;

    struct cn_ocb *ocb = ocb_new();
    if (ocb == NULL)
        return ENOMEM;
    if (strcmp(path, CN_STATUS_NAME) == 0) {
        /* Answered here, from this process's own state, so it works with the peer down. Read only. */
        if (msg->connect.ioflag & _IO_FLAG_WR) {
            ocb_destroy(ocb);
            return EROFS;
        }
        if ((msg->connect.ioflag & O_DIRECTORY) || (msg->connect.eflag & _IO_CONNECT_EFLAG_DIR)) {
            ocb_destroy(ocb);
            return ENOTDIR;
        }
        if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
            ocb_destroy(ocb);
            return EEXIST;
        }
        ocb->text = status_text(&ocb->tlen);
        if (ocb->text == NULL) {
            ocb_destroy(ocb);
            return ENOMEM;
        }
        ocb->state = OCB_STATUS;
        iofunc_attr_lock(&status_attr);
        status_attr.nbytes = ocb->tlen;
        int st = iofunc_ocb_attach(ctp, msg, &ocb->hdr, &status_attr, &iof);
        iofunc_attr_unlock(&status_attr);
        if (st != EOK)
            ocb_destroy(ocb);
        return st;
    }
    if (!plain) {
        /* stat() and friends arrive as this connect combined with the operation and a close. The OCB
         * only remembers the name; the stat handler asks the peer. */
        ocb->path = strdup(path);
        if (ocb->path == NULL) {
            ocb_destroy(ocb);
            return ENOMEM;
        }
        ocb->state = OCB_STATPATH;
            iofunc_attr_lock(&file_attr);
        int st = iofunc_ocb_attach(ctp, msg, &ocb->hdr, &file_attr, &iof);
        iofunc_attr_unlock(&file_attr);
        if (st != EOK)
            ocb_destroy(ocb);
        return st;
    }
    unsigned io = msg->connect.ioflag;
    int rd = (io & _IO_FLAG_RD) != 0, wr = (io & _IO_FLAG_WR) != 0;
    int oflag = (rd && wr) ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    oflag |= (int)(io & (O_APPEND | O_NONBLOCK));
    /* opendir() asks for a directory: attach to the directory attribute so the library accepts it, and
     * have the peer refuse with ENOTDIR if the object is not one. */
    int want_dir = (io & O_DIRECTORY) != 0 || (msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) != 0;
    if (want_dir)
        oflag |= CN_OFLAG_DIR;
    iofunc_attr_t *attr = want_dir ? &dir_attr : &file_attr;

    ocb->state = OCB_OPENING;
    iofunc_attr_lock(attr);
    int st = iofunc_ocb_attach(ctp, msg, &ocb->hdr, attr, &iof);
    iofunc_attr_unlock(attr);
    if (st != EOK) {
        ocb_destroy(ocb);
        return st;
    }
    st = submit(ctp, CN_OP_OPEN, NULL, ocb, -1, oflag, 0, path, (uint32_t)plen, 0, NULL);
    if (st != _RESMGR_NOREPLY)
        drop_opening_ocb(ctp, &ctp->info, ocb);
    return st;
}

/* stat() by name or fstat() on an open descriptor: the peer's attributes, through the peer's own checks. */
static int cn_stat(resmgr_context_t *ctp, io_stat_t *msg, struct cn_ocb *ocb)
{
    struct aux x = { .sm = msg, .cap = 0 };
    switch (ocb->state) {
    case OCB_STATUS:
        status_attr.nbytes = ocb->tlen;     /* each open has its own snapshot; one dispatch thread, so no race */
        return iofunc_stat_default(ctp, msg, &ocb->hdr);
    case OCB_LOCAL:
        return iofunc_stat_default(ctp, msg, &ocb->hdr);
    case OCB_STATPATH:
        return submit(ctp, CN_OP_STAT, NULL, NULL, -1, 0, 0, ocb->path, (uint32_t)strlen(ocb->path), 0, &x);
    case OCB_OPEN:
        return submit(ctp, CN_OP_STAT, ocb, NULL, ocb->rh, 0, 0, NULL, 0, 0, &x);
    default:
        return EBADF;
    }
}

static int cn_read(resmgr_context_t *ctp, io_read_t *msg, struct cn_ocb *ocb)
{
    int st = iofunc_read_verify(ctp, msg, &ocb->hdr, NULL);
    if (st != EOK)
        return st;
    unsigned xt = msg->i.xtype & _IO_XTYPE_MASK;
    if (xt != _IO_XTYPE_NONE && !(xt == _IO_XTYPE_READDIR && ocb->isdir))
        return ENOSYS;
    if (ocb->state == OCB_STATUS) {
        uint64_t off = ocb->hdr.offset, n = _IO_READ_GET_NBYTES(msg);
        if (off > ocb->tlen)
            off = ocb->tlen;
        if (n > ocb->tlen - off)
            n = ocb->tlen - off;
        ocb->hdr.offset = (off_t)(off + n);
        _IO_SET_READ_NBYTES(ctp, (size_t)n);
        SETIOV(ctp->iov, ocb->text + off, (size_t)n);
        return _RESMGR_NPARTS(1);
    }
    if (ocb->state != OCB_OPEN)
        return ocb->hdr.attr == &dir_attr ? ENOSYS : EBADF;
    uint64_t n = _IO_READ_GET_NBYTES(msg);
    if (ocb->isdir) {
        /* A directory reads as dirent records, built from the peer's entry list. */
        struct aux x = { .cap = n > 32768 ? 32768 : (uint32_t)n };
        uint32_t ask = x.cap > CN_MAX_DATA ? CN_MAX_DATA : x.cap;
        if (ask == 0)
            return EOK;
        return submit(ctp, CN_OP_READDIR, ocb, ocb, ocb->rh, (int32_t)ocb->dpos, (int32_t)ask, NULL, 0, ask, &x);
    }
    if (n > CN_MAX_DATA)
        n = CN_MAX_DATA;
    ocb->ready = 0;
    return submit(ctp, CN_OP_READ, ocb, NULL, ocb->rh, (int32_t)n, 0, NULL, 0, (uint32_t)n, NULL);
}

static int cn_write(resmgr_context_t *ctp, io_write_t *msg, struct cn_ocb *ocb)
{
    int st = iofunc_write_verify(ctp, msg, &ocb->hdr, NULL);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (ocb->state != OCB_OPEN)
        return EBADF;
    uint64_t n = _IO_WRITE_GET_NBYTES(msg);
    if (n > CN_MAX_DATA)
        n = CN_MAX_DATA;                /* a short write; callers loop for the rest */
    size_t hdr = (msg->i.type == _IO_WRITE64) ? sizeof msg->i64 : sizeof msg->i;
    unsigned char *buf = malloc((size_t)n + 1);
    if (buf == NULL)
        return ENOMEM;
    ssize_t got = n ? resmgr_msgget(ctp, buf, (size_t)n, hdr) : 0;
    if (got < 0 || (uint64_t)got != n) {
        free(buf);
        return EFAULT;
    }
    st = submit(ctp, CN_OP_WRITE, ocb, NULL, ocb->rh, 0, 0, buf, (uint32_t)n, (uint32_t)n, NULL);
    free(buf);
    return st;
}

/* Generic devctl forwarding: the direction bits and the caller's byte count say what travels. */
static int cn_devctl(resmgr_context_t *ctp, io_devctl_t *msg, struct cn_ocb *ocb)
{
    if (ocb->state != OCB_OPEN)
        return ocb->hdr.attr == &file_attr ? EBADF : ENOTTY;
    unsigned dcmd = (unsigned)msg->i.dcmd;
    unsigned dir = get_device_direction(dcmd);
    uint32_t n = msg->i.nbytes;
    if (n > CN_MAX_DATA)
        return EMSGSIZE;
    unsigned char *in = NULL;
    uint32_t inlen = 0;
    if ((dir & DEVDIR_TO) && n > 0) {
        in = malloc(n);
        if (in == NULL)
            return ENOMEM;
        ssize_t got = resmgr_msgget(ctp, in, n, sizeof msg->i);
        if (got < 0 || (uint32_t)got != n) {     /* fewer bytes arrived than the header claims */
            free(in);
            return EINVAL;
        }
        inlen = n;
    }
    int st = submit(ctp, CN_OP_DEVCTL, ocb, NULL, ocb->rh, (int32_t)dcmd, (int32_t)n, in, inlen, n, NULL);
    free(in);
    return st;
}

/* select, poll and ionotify. Output is always ready (a write waits on the peer, never on room here). Input is ready
 * once the peer said so; otherwise the waiter is armed and one NOTIFY request goes to the peer, whose answer wakes
 * every waiter on this open (complete). */
static int cn_notify(resmgr_context_t *ctp, io_notify_t *msg, struct cn_ocb *ocb)
{
    if (ocb->state == OCB_STATUS)
        return iofunc_notify(ctp, msg, ocb->notify, _NOTIFY_COND_INPUT | _NOTIFY_COND_OUTPUT, NULL, NULL);
    if (ocb->state != OCB_OPEN || ocb->isdir)
        return ENOSYS;
    int trig = _NOTIFY_COND_OUTPUT;
    if (ocb->ready)
        trig |= _NOTIFY_COND_INPUT;
    pthread_mutex_lock(&G.mtx);
    int down = ocb->gen != G.gen || G.state != C_UP;
    int out = 0;
    for (int i = 0; i < NPEND; i++)
        if (G.pend[i].used && !G.pend[i].done && G.pend[i].op == CN_OP_NOTIFY)
            out++;
    pthread_mutex_unlock(&G.mtx);
    if (down)
        trig |= _NOTIFY_COND_INPUT;     /* the link is gone: the read will say why, so do not let the caller wait */
    else if (!(trig & _NOTIFY_COND_INPUT) && !ocb->armed && (msg->i.flags & _NOTIFY_COND_INPUT) && out >= CN_MAX_NOTIFY)
        return EAGAIN;                  /* enough waiters already: keep the room ordinary calls need */
    int armed = 0;
    int st = iofunc_notify(ctp, msg, ocb->notify, trig, NULL, &armed);
    if (armed && !ocb->armed && !(trig & _NOTIFY_COND_INPUT) && notify_peer(ocb) == 0)
        ocb->armed = 1;
    return st;
}

/* Offsets live on the peer and are not forwarded: say so instead of moving a local copy. A directory is
 * the exception, because its position is only an entry index that this side keeps (rewinddir, seekdir). */
static int cn_lseek(resmgr_context_t *ctp, io_lseek_t *msg, struct cn_ocb *ocb)
{
    if (ocb->state == OCB_STATUS) {
        status_attr.nbytes = ocb->tlen;
        return iofunc_lseek_default(ctp, msg, &ocb->hdr);
    }
    if (ocb->state != OCB_OPEN || !ocb->isdir)
        return ESPIPE;
    int64_t pos;
    switch (msg->i.whence) {
    case SEEK_SET: pos = (int64_t)msg->i.offset; break;
    case SEEK_CUR: pos = (int64_t)ocb->dpos + (int64_t)msg->i.offset; break;
    default:       return ESPIPE;
    }
    if (pos < 0 || pos > 0x7fffffff)
        return EINVAL;
    ocb->dpos = (uint32_t)pos;
    msg->o = (uint64_t)pos;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

static int cn_close_ocb(resmgr_context_t *ctp, void *reserved, struct cn_ocb *ocb)
{
    pthread_mutex_lock(&G.mtx);
    /* No pending call may keep a pointer to this OCB (an open it was waiting on, a directory read). */
    for (int i = 0; i < NPEND; i++) {
        struct pend *e = &G.pend[i];
        if (e->used && e->ocb == ocb) {
            e->ocb = NULL;
            e->cancelled = 1;
            if (e->op == CN_OP_OPEN && e->done && e->err == EOK)
                queue_close_locked(e->val);
        }
    }
    if (ocb->state == OCB_OPEN && ocb->gen == G.gen && G.state == C_UP)
        queue_close_locked(ocb->rh);    /* fire and forget: close never waits on the peer */
    pthread_mutex_unlock(&G.mtx);
    wake_io();
    iofunc_notify_remove(ctp, ocb->notify);
    return iofunc_close_ocb_default(ctp, reserved, &ocb->hdr);
}

/* -f CONF: one line per peer, "NAME ADDRESS:PORT KEYFILE"; blank lines and lines starting with # are skipped. */
#define CN_MAX_PEERS 32
struct peercfg {
    char name[64], addr[64];
    unsigned char key[CN_KEY_MAX];
    size_t keylen;
};

/* A peer name becomes one path component under DIR: letters, digits, '-', '_' and '.', but not "." or "..", and
 * not the status file's name. */
static int name_ok(const char *n)
{
    if (n[0] == '\0' || strcmp(n, ".") == 0 || strcmp(n, "..") == 0 || strcmp(n, CN_STATUS_NAME) == 0 || strlen(n) > 63)
        return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    return 1;
}

static int port_ok(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    return end != s && *end == '\0' && v > 0 && v <= 65535;
}

static int read_conf(const char *path, struct peercfg *pc, int max)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "crossnode-net: %s: %s\n", path, strerror(errno));
        return -1;
    }
    char line[512];
    int n = 0, lineno = 0, bad = 0;
    while (!bad && fgets(line, sizeof line, f) != NULL) {
        lineno++;
        char nm[64], ad[64], kf[256], extra[2];
        if (strchr(line, '\n') == NULL && !feof(f)) {
            fprintf(stderr, "crossnode-net: %s line %d: longer than %zu characters\n", path, lineno, sizeof line - 2);
            bad = 1;
            break;
        }
        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';                     /* # starts a comment, also after the fields */
        char *t = line + strspn(line, " \t");
        if (*t == '\n' || *t == '\0')
            continue;
        int k = sscanf(t, "%63s %63s %255s %1s", nm, ad, kf, extra);
        struct in_addr ia;
        char host[64];
        const char *colon = strrchr(ad, ':');
        if (k != 3) {
            fprintf(stderr, "crossnode-net: %s line %d: want NAME ADDRESS:PORT KEYFILE\n", path, lineno);
            bad = 1;
        } else if (!name_ok(nm)) {
            fprintf(stderr, "crossnode-net: %s line %d: bad name %s\n", path, lineno, nm);
            bad = 1;
        } else if (colon == NULL || colon - ad >= (long)sizeof host || !port_ok(colon + 1) ||
                   (snprintf(host, sizeof host, "%.*s", (int)(colon - ad), ad), inet_pton(AF_INET, host, &ia) != 1)) {
            fprintf(stderr, "crossnode-net: %s line %d: bad address %s\n", path, lineno, ad);
            bad = 1;
        } else if (n == max) {
            fprintf(stderr, "crossnode-net: %s: more than %d peers\n", path, max);
            bad = 1;
        } else {
            for (int i = 0; i < n; i++) {
                if (strcmp(pc[i].name, nm) == 0) {
                    fprintf(stderr, "crossnode-net: %s line %d: %s is named twice\n", path, lineno, nm);
                    bad = 1;
                }
            }
            char err[256];
            int kl = bad ? 0 : cn_load_key(kf, pc[n].key, err, sizeof err);
            if (kl < 0) {
                fprintf(stderr, "crossnode-net: %s line %d: %s\n", path, lineno, err);
                bad = 1;
            } else if (!bad) {
                snprintf(pc[n].name, sizeof pc[n].name, "%s", nm);
                snprintf(pc[n].addr, sizeof pc[n].addr, "%s", ad);
                pc[n].keylen = (size_t)kl;
                n++;
            }
        }
    }
    fclose(f);
    if (!bad && n == 0) {
        fprintf(stderr, "crossnode-net: %s lists no peer\n", path);
        bad = 1;
    }
    if (bad) {
        for (int i = 0; i < max; i++)
            memset(pc[i].key, 0, sizeof pc[i].key);
        return -1;
    }
    return n;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: crossnode-net -n NAME -c ADDRESS:PORT (-k KEYFILE | -I) [-g GID] [-m DIR] [-r PRIO] [-v]\n"
            "       crossnode-net -f CONF [-g GID] [-m DIR] [-r PRIO] [-v]\n"
            "  -f CONF          one peer per line: NAME ADDRESS:PORT KEYFILE (# starts a comment). Every line is\n"
            "                   checked first; then each peer gets its own process, holding only its own key,\n"
            "                   so a peer that hangs or fails cannot hold up another\n"
            "  -k KEYFILE       shared key with the agent (at least 16 bytes, mode 600)\n"
            "  -I               no authentication (only against an agent that was also started with -I)\n"
            "  -n NAME          the peer appears as DIR/NAME\n"
            "  -c ADDRESS:PORT  the peer's crossnode-agent\n"
            "  -g GID           members of this group may use DIR/NAME too (default: root only)\n"
            "  -m DIR           where peers appear (default /net)\n"
            "  -r PRIO          run the link's I/O thread at PRIO (default: this process's own)\n"
            "DIR/NAME/.status reads as the link's state: up or down, authentication, last heard, waiting calls,\n"
            "connects, drops and refused frames, each with its last reason.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *mountdir = "/net";
    char *target = NULL;
    const char *keyfile = NULL, *conf = NULL;
    int insecure = 0;
    int opt;
    while ((opt = getopt(argc, argv, "n:c:g:m:vk:Ir:f:")) != -1) {
        switch (opt) {
        case 'n': name = optarg; break;
        case 'c': target = optarg; break;
        case 'g': allowed_gid = (gid_t)atoi(optarg); break;
        case 'm': mountdir = optarg; break;
        case 'v': verbose = 1; break;
        case 'k': keyfile = optarg; break;
        case 'I': insecure = 1; break;
        case 'r': io_prio = atoi(optarg); break;
        case 'f': conf = optarg; break;
        default: usage();
        }
    }
    static char conf_addr[64];
    if (conf != NULL) {
        if (name != NULL || target != NULL || keyfile != NULL || insecure) {
            fprintf(stderr, "crossnode-net: -f replaces -n, -c, -k and -I\n");
            return 2;
        }
        static struct peercfg pc[CN_MAX_PEERS];
        int np = read_conf(conf, pc, CN_MAX_PEERS);
        if (np < 0)
            return 2;
        int me = np - 1;              /* this process serves the last peer; a child serves each of the others */
        pid_t kids[CN_MAX_PEERS];
        signal(SIGCHLD, SIG_IGN);     /* a child that exits is reaped by the system, never left a zombie */
        for (int i = 0; i < np - 1; i++) {
            pid_t c = fork();
            if (c < 0) {
                perror("crossnode-net: fork");
                for (int j = 0; j < i; j++)    /* all or nothing: stop the peers already started */
                    kill(kids[j], SIGTERM);
                return 1;
            }
            kids[i] = c;
            if (c == 0) {
                me = i;
                break;
            }
        }
        static char conf_name[64];
        snprintf(conf_name, sizeof conf_name, "%s", pc[me].name);
        snprintf(conf_addr, sizeof conf_addr, "%s", pc[me].addr);
        name = conf_name;
        target = conf_addr;
        memcpy(G.key, pc[me].key, pc[me].keylen);
        G.keylen = pc[me].keylen;
        for (int i = 0; i < np; i++)          /* keep only this link's key */
            memset(pc[i].key, 0, sizeof pc[i].key);
    }
    if (name == NULL || target == NULL || !name_ok(name))
        usage();
    if (conf == NULL && (keyfile == NULL) == (insecure == 0)) {
        fprintf(stderr, "crossnode-net: give either -k KEYFILE or -I (no authentication), not both and not neither\n");
        return 2;
    }
    if (keyfile != NULL) {
        char err[256];
        int n = cn_load_key(keyfile, G.key, err, sizeof err);
        if (n < 0) {
            fprintf(stderr, "crossnode-net: %s\n", err);
            return 2;
        }
        G.keylen = (size_t)n;
    }
    snprintf(peer_text, sizeof peer_text, "%s", target);
    char *colon = strrchr(target, ':');
    if (colon == NULL)
        usage();
    *colon = '\0';
    memset(&G.peer, 0, sizeof G.peer);
    G.peer.sin_family = AF_INET;
    G.peer.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, target, &G.peer.sin_addr) != 1 || G.peer.sin_port == 0) {
        fprintf(stderr, "crossnode-net: bad peer address\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    if (pipe(G.wake) != 0) {
        perror("crossnode-net: pipe");
        return 1;
    }
    fcntl(G.wake[0], F_SETFL, fcntl(G.wake[0], F_GETFL) | O_NONBLOCK);
    fcntl(G.wake[1], F_SETFL, fcntl(G.wake[1], F_GETFL) | O_NONBLOCK);

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        perror("crossnode-net: dispatch_create");
        return 1;
    }
    memset(&cn_ocb_funcs, 0, sizeof cn_ocb_funcs);
    cn_ocb_funcs.nfuncs = _IOFUNC_NFUNCS;
    cn_ocb_funcs.ocb_calloc = cn_ocb_calloc;
    cn_ocb_funcs.ocb_free = cn_ocb_free;
    memset(&cn_mount, 0, sizeof cn_mount);
    cn_mount.funcs = &cn_ocb_funcs;
    int group = allowed_gid != (gid_t)-1;
    iofunc_attr_init(&dir_attr, S_IFDIR | (group ? 0750 : 0700), NULL, NULL);
    dir_attr.mount = &cn_mount;
    iofunc_attr_init(&file_attr, S_IFCHR | (group ? 0660 : 0600), NULL, NULL);
    file_attr.mount = &cn_mount;
    iofunc_attr_init(&status_attr, S_IFREG | (group ? 0440 : 0400), NULL, NULL);
    status_attr.mount = &cn_mount;
    dir_attr.uid = file_attr.uid = status_attr.uid = 0;
    dir_attr.gid = file_attr.gid = status_attr.gid = group ? allowed_gid : 0;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    cf.open = cn_open;
    cf.unblock = connect_unblock;
    iof.read = cn_read;
    iof.write = cn_write;
    iof.devctl = cn_devctl;
    iof.close_ocb = cn_close_ocb;
    iof.lseek = cn_lseek;
    iof.stat = cn_stat;
    iof.unblock = io_unblock;
    iof.notify = cn_notify;
    /* One dispatch thread, so no attribute locking; this also keeps the library away from an OCB
     * that an unblock handler has just freed. Metadata changes are not forwarded yet. */
    iof.lock_ocb = NULL;
    iof.unlock_ocb = NULL;
    iof.chmod = NULL;
    iof.chown = NULL;
    iof.utime = NULL;

    resmgr_attr_t ra;
    memset(&ra, 0, sizeof ra);
    ra.nparts_max = 2;
    ra.msg_max_size = 4096;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", mountdir, name);
    if (resmgr_attach(dpp, &ra, path, _FTYPE_ANY, _RESMGR_FLAG_DIR, &cf, &iof, &dir_attr) == -1) {
        perror("crossnode-net: resmgr_attach");
        return 1;
    }
    G.pulse_code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_doorbell, NULL);
    G.self_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (G.pulse_code == -1 || G.self_coid == -1) {
        perror("crossnode-net: pulse setup");
        return 1;
    }
    pthread_t t;
    /* The I/O thread moves every request and reply, so it must not wait behind lower-priority work on this
     * machine: -r puts it above that work. (The dispatch thread already runs at each caller's priority.) */
    pthread_attr_t at;
    pthread_attr_init(&at);
    if (io_prio > 0) {
        struct sched_param sp;
        int pol;
        if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0) {
            sp.sched_priority = io_prio;
            pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
            pthread_attr_setschedpolicy(&at, pol);
            pthread_attr_setschedparam(&at, &sp);
        }
    }
    int trc = pthread_create(&t, &at, io_thread, NULL);
    pthread_attr_destroy(&at);
    if (trc != 0) {
        fprintf(stderr, "crossnode-net: cannot start the I/O thread\n");
        return 1;
    }
    printf("crossnode-net: %s -> %s:%d (liveness %d ms, connect %d ms, %s)\n", path, target,
           ntohs(G.peer.sin_port), CN_DEAD_MS, CN_CONNECT_MS, G.keylen ? "authenticated" : "NO AUTHENTICATION: -I");
    fflush(stdout);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        dispatch_context_t *n = dispatch_block(ctp);
        if (n == NULL) {
            if (errno == EINTR)
                continue;
            perror("crossnode-net: dispatch_block");
            return 1;
        }
        ctp = n;
        dispatch_handler(ctp);
    }
}
