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
 * Gate 1 limits, stated plainly: stat() answers from a placeholder, directory listing is not
 * forwarded yet, pread/pwrite are not supported, there is one peer per process and no authentication.
 */
struct cn_ocb;
#define IOFUNC_OCB_T struct cn_ocb
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
#include <time.h>
#include <unistd.h>

#include "cn_proto.h"

#define CN_PING_MS      500   /* liveness probe period while connected */
#define CN_DEAD_MS     2000   /* silence after which the peer is declared down */
#define CN_CONNECT_MS  1500   /* deadline for a new connection */
#define CN_RETRY_MS    1000   /* after a failure, new calls fail at once for this long */
#define NPEND           128   /* calls waiting on the peer at the same time */

enum ocb_state { OCB_LOCAL = 0, OCB_OPENING, OCB_OPEN };

struct cn_ocb {
    iofunc_ocb_t hdr;         /* must be first */
    int state;
    int32_t rh;               /* the peer's handle, -1 if none */
    uint32_t gen;             /* connection generation the handle belongs to */
};

struct pend {
    int used, done, cancelled;
    int op;
    uint32_t seq;
    rcvid_t rcvid;
    int err;
    int32_t val;
    uint32_t gen;
    unsigned char *data;
    uint32_t dlen;
    struct cn_ocb *ocb;       /* OPEN only: the OCB bound to the waiting client */
    struct _msg_info info;    /* OPEN only: that client's connection, to unbind on failure */
};

enum conn_state { C_DOWN = 0, C_CONNECTING, C_UP };

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
    unsigned char rx[sizeof(struct cn_hdr) + CN_MAX_DATA];
    size_t rxlen;
    struct pend pend[NPEND];
    int wake[2];
    struct sockaddr_in peer;
    int self_coid;
    int pulse_code;
} G = { .mtx = PTHREAD_MUTEX_INITIALIZER, .sock = -1 };

static iofunc_attr_t dir_attr, file_attr;
static iofunc_mount_t cn_mount;
static iofunc_funcs_t cn_ocb_funcs;
static resmgr_connect_funcs_t cf;
static resmgr_io_funcs_t iof;
static const char *name;
static int verbose;

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

static IOFUNC_OCB_T *cn_ocb_calloc(resmgr_context_t *ctp, IOFUNC_ATTR_T *attr)
{
    (void)ctp;
    (void)attr;
    return ocb_new();
}

static void cn_ocb_free(IOFUNC_OCB_T *ocb)
{
    free(ocb);
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
    if (G.state != C_DOWN)
        vlog("peer down: %s\n", why);
    G.state = C_DOWN;
    G.gen++;
    G.last_fail = now_ms();
    G.outlen = 0;
    G.rxlen = 0;
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
    G.state = C_UP;
    G.last_rx = G.last_ping = now_ms();
    vlog("peer up\n");
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
        up_locked();
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

static void handle_frame_locked(const struct cn_hdr *h, const unsigned char *p)
{
    G.last_rx = now_ms();
    if (h->op == (CN_OP_PING | CN_REPLY))
        return;
    struct pend *e = find_seq_locked(h->seq);
    if (e == NULL || e->done) {
        /* Nobody waits for this answer any more; never leak a handle the peer just opened. */
        if (h->op == (CN_OP_OPEN | CN_REPLY) && h->a == 0)
            queue_close_locked(h->b);
        return;
    }
    if (e->cancelled) {
        if (e->op == CN_OP_OPEN && h->a == 0)
            queue_close_locked(h->b);
        e->done = 1;
        G.ring = 1;
        return;
    }
    e->err = h->a;
    e->val = h->b;
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
                teardown_locked("protocol error");
                return;
            }
            if (G.rxlen - off < sizeof h + h.len)
                break;
            handle_frame_locked(&h, G.rx + off + sizeof h);
            off += sizeof h + h.len;
        }
        memmove(G.rx, G.rx + off, G.rxlen - off);
        G.rxlen -= off;
    }
}

static void io_write_locked(void)
{
    ssize_t w = send(G.sock, G.out, G.outlen, 0);
    if (w > 0) {
        memmove(G.out, G.out + w, G.outlen - (size_t)w);
        G.outlen -= (size_t)w;
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
        if (G.state == C_UP) {
            if (t - G.last_rx >= CN_DEAD_MS) {
                teardown_locked("no answer within the liveness limit");
            } else if (t - G.last_ping >= CN_PING_MS) {
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
            fds[1].events = (short)(G.state == C_CONNECTING ? POLLOUT : (POLLIN | (G.outlen ? POLLOUT : 0)));
            fds[1].revents = 0;
            nfds = 2;
        }
        int ring = G.ring;
        G.ring = 0;
        pthread_mutex_unlock(&G.mtx);

        if (ring)
            MsgSendPulse(G.self_coid, -1, G.pulse_code, 0);
        (void)poll(fds, (nfds_t)nfds, 100);

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
                    up_locked();
                else
                    teardown_locked(err ? strerror(err) : "connect failed");
            } else if (G.state == C_UP) {
                if (re & POLLIN)
                    io_read_locked();
                if (G.state == C_UP && (re & POLLOUT) && G.outlen)
                    io_write_locked();
                if (G.state == C_UP && (re & (POLLERR | POLLHUP | POLLNVAL)) && !(re & POLLIN))
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
static int submit(resmgr_context_t *ctp, int op, struct cn_ocb *gen_ocb, struct cn_ocb *open_ocb,
                  int32_t handle, int32_t a, int32_t b, const void *payload, uint32_t plen)
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
    if (open_ocb != NULL) {
        e->ocb = open_ocb;
        e->info = ctp->info;
    }
    struct cn_hdr h = {
        .magic = CN_MAGIC, .op = (uint16_t)op, .seq = e->seq, .handle = handle, .a = a, .b = b, .len = plen,
    };
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
static void drop_opening_ocb(resmgr_context_t *ctp, const struct _msg_info *info, struct cn_ocb *ocb)
{
    struct _msg_info keep = ctp->info;
    ctp->info = *info;
    (void)resmgr_unbind(ctp);
    iofunc_attr_lock(&file_attr);
    (void)iofunc_ocb_detach(ctp, &ocb->hdr);
    iofunc_attr_unlock(&file_attr);
    ctp->info = keep;
    free(ocb);
}

static void complete(resmgr_context_t *ctp, struct pend *e)
{
    switch (e->op) {
    case CN_OP_OPEN:
        if (e->err == EOK) {
            e->ocb->rh = e->val;
            e->ocb->gen = e->gen;
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
    int op = 0;
    pthread_mutex_lock(&G.mtx);
    for (int i = 0; i < NPEND; i++) {
        if (G.pend[i].used && !G.pend[i].cancelled && G.pend[i].rcvid == rcvid) {
            e = &G.pend[i];
            break;
        }
    }
    if (e != NULL) {
        e->cancelled = 1;
        op = e->op;
        ocb = e->ocb;
        e->ocb = NULL;
        info = e->info;
        if (e->done && op == CN_OP_OPEN && e->err == EOK)
            queue_close_locked(e->val);
    }
    pthread_mutex_unlock(&G.mtx);
    if (e == NULL)
        return;
    if (op == CN_OP_OPEN && ocb != NULL)
        drop_opening_ocb(ctp, &info, ocb);
    MsgError(rcvid, EINTR);
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

static int cn_open(resmgr_context_t *ctp, io_open_t *msg, iofunc_attr_t *handle, void *extra)
{
    (void)handle;
    const char *path = msg->connect.path;
    if (path[0] == '\0')
        return iofunc_open_default(ctp, msg, &dir_attr, extra);   /* /net/<name> itself */

    struct cn_ocb *ocb = ocb_new();
    if (ocb == NULL)
        return ENOMEM;
    if (msg->connect.subtype != _IO_CONNECT_OPEN) {
        /* stat() and friends arrive combined with their open; gate 1 answers from a placeholder. */
        iofunc_attr_lock(&file_attr);
        int st = iofunc_ocb_attach(ctp, msg, &ocb->hdr, &file_attr, &iof);
        iofunc_attr_unlock(&file_attr);
        if (st != EOK)
            free(ocb);
        return st;
    }
    size_t plen = strlen(path);
    if (plen > CN_MAX_PATH) {
        free(ocb);
        return ENAMETOOLONG;
    }
    unsigned io = msg->connect.ioflag;
    int rd = (io & _IO_FLAG_RD) != 0, wr = (io & _IO_FLAG_WR) != 0;
    int oflag = (rd && wr) ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    oflag |= (int)(io & (O_APPEND | O_NONBLOCK));

    ocb->state = OCB_OPENING;
    iofunc_attr_lock(&file_attr);
    int st = iofunc_ocb_attach(ctp, msg, &ocb->hdr, &file_attr, &iof);
    iofunc_attr_unlock(&file_attr);
    if (st != EOK) {
        free(ocb);
        return st;
    }
    st = submit(ctp, CN_OP_OPEN, NULL, ocb, -1, oflag, 0, path, (uint32_t)plen);
    if (st != _RESMGR_NOREPLY)
        drop_opening_ocb(ctp, &ctp->info, ocb);
    return st;
}

static int cn_read(resmgr_context_t *ctp, io_read_t *msg, struct cn_ocb *ocb)
{
    int st = iofunc_read_verify(ctp, msg, &ocb->hdr, NULL);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (ocb->state != OCB_OPEN)
        return ocb->hdr.attr == &dir_attr ? ENOSYS : EBADF;
    uint64_t n = _IO_READ_GET_NBYTES(msg);
    if (n > CN_MAX_DATA)
        n = CN_MAX_DATA;
    return submit(ctp, CN_OP_READ, ocb, NULL, ocb->rh, (int32_t)n, 0, NULL, 0);
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
    st = submit(ctp, CN_OP_WRITE, ocb, NULL, ocb->rh, 0, 0, buf, (uint32_t)n);
    free(buf);
    return st;
}

/* Generic devctl forwarding: the direction bits and the caller's byte count say what travels. */
static int cn_devctl(resmgr_context_t *ctp, io_devctl_t *msg, struct cn_ocb *ocb)
{
    if (ocb->state != OCB_OPEN)
        return ocb->hdr.attr == &dir_attr ? ENOTTY : EBADF;
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
    int st = submit(ctp, CN_OP_DEVCTL, ocb, NULL, ocb->rh, (int32_t)dcmd, (int32_t)n, in, inlen);
    free(in);
    return st;
}

static int cn_close_ocb(resmgr_context_t *ctp, void *reserved, struct cn_ocb *ocb)
{
    pthread_mutex_lock(&G.mtx);
    if (ocb->state == OCB_OPENING) {
        for (int i = 0; i < NPEND; i++) {
            struct pend *e = &G.pend[i];
            if (e->used && e->ocb == ocb) {
                e->ocb = NULL;
                e->cancelled = 1;
                if (e->done && e->err == EOK)
                    queue_close_locked(e->val);
            }
        }
    } else if (ocb->state == OCB_OPEN && ocb->gen == G.gen && G.state == C_UP) {
        queue_close_locked(ocb->rh);    /* fire and forget: close never waits on the peer */
    }
    pthread_mutex_unlock(&G.mtx);
    wake_io();
    return iofunc_close_ocb_default(ctp, reserved, &ocb->hdr);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: crossnode-net -n NAME -c ADDRESS:PORT [-m DIR] [-v]\n"
            "  -n NAME          the peer appears as DIR/NAME\n"
            "  -c ADDRESS:PORT  the peer's crossnode-agent\n"
            "  -m DIR           where peers appear (default /net)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *mountdir = "/net";
    char *target = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "n:c:m:v")) != -1) {
        switch (opt) {
        case 'n': name = optarg; break;
        case 'c': target = optarg; break;
        case 'm': mountdir = optarg; break;
        case 'v': verbose = 1; break;
        default: usage();
        }
    }
    if (name == NULL || target == NULL || strchr(name, '/') != NULL || name[0] == '\0')
        usage();
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
    iofunc_attr_init(&dir_attr, S_IFDIR | 0555, NULL, NULL);
    dir_attr.mount = &cn_mount;
    iofunc_attr_init(&file_attr, S_IFCHR | 0666, NULL, NULL);
    file_attr.mount = &cn_mount;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    cf.open = cn_open;
    cf.unblock = connect_unblock;
    iof.read = cn_read;
    iof.write = cn_write;
    iof.devctl = cn_devctl;
    iof.close_ocb = cn_close_ocb;
    iof.unblock = io_unblock;
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
    if (pthread_create(&t, NULL, io_thread, NULL) != 0) {
        fprintf(stderr, "crossnode-net: cannot start the I/O thread\n");
        return 1;
    }
    printf("crossnode-net: %s -> %s:%d (liveness %d ms, connect %d ms)\n", path, target,
           ntohs(G.peer.sin_port), CN_DEAD_MS, CN_CONNECT_MS);
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
