/*
 * cnmitm: a man in the middle for checking Crossnode's per-frame integrity (test tool only).
 *
 *   cnmitm LISTEN_PORT TARGET_HOST TARGET_PORT MODE TRAILER
 *
 * Listens on 127.0.0.1:LISTEN_PORT, connects each client to TARGET_HOST:TARGET_PORT (a crossnode-agent) and
 * passes the bytes on. Frames from the agent are parsed: the first two are the handshake (challenge and proof),
 * every later frame is a 28-byte header, its payload and TRAILER bytes of integrity tag (32 for Crossnode's
 * keyed links, 0 to model an unprotected stream). On every connection, one frame after the handshake is
 * changed according to MODE:
 *   flip     the first devctl reply: one bit of its first payload byte is flipped (a GET of 42 would read 43)
 *   replay   the first devctl reply is sent twice
 *   flipreq  the first devctl request that carries data: one bit of its first payload byte is flipped
 *            (a SET of 42 would set 43), so the agent's own check is exercised
 *   pass     nothing is changed
 * Frames from the client are parsed the same way (its one handshake frame carries no tag). One line per
 * change goes to stdout.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/cn_proto.h"

static const char *mode;
static size_t trailer;
static struct sockaddr_in target;

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

struct pair {
    int client, agent;
};

/* Client to agent: frame by frame; in flipreq mode, the first devctl request with data is changed. */
static void *upstream(void *arg)
{
    struct pair *pp = arg;
    static unsigned char frame[sizeof(struct cn_hdr) + CN_MAX_DATA + 64];
    int n = 0, done = 0;
    for (;;) {
        struct cn_hdr h;
        if (read_full(pp->client, &h, sizeof h) != 0 || h.magic != CN_MAGIC || h.len > CN_MAX_DATA)
            break;
        size_t tail = n < 1 ? 0 : trailer;           /* the client's handshake frame carries no tag */
        memcpy(frame, &h, sizeof h);
        if (read_full(pp->client, frame + sizeof h, h.len + tail) != 0)
            break;
        size_t total = sizeof h + h.len + tail;
        n++;
        if (!done && n > 1 && h.op == CN_OP_DEVCTL && h.len > 0 && strcmp(mode, "flipreq") == 0) {
            done = 1;
            frame[sizeof h] ^= 0x01;
            printf("flipped one bit in the devctl request seq %u\n", h.seq);
            fflush(stdout);
        }
        if (write_full(pp->agent, frame, total) != 0)
            break;
    }
    shutdown(pp->agent, SHUT_RDWR);
    shutdown(pp->client, SHUT_RDWR);
    return NULL;
}

/* Agent to client: frame by frame, changing the first devctl reply after the handshake. */
static void downstream(struct pair *pp)
{
    unsigned char frame[sizeof(struct cn_hdr) + CN_MAX_DATA + 64];
    int n = 0, done = 0;
    for (;;) {
        struct cn_hdr h;
        if (read_full(pp->agent, &h, sizeof h) != 0 || h.magic != CN_MAGIC || h.len > CN_MAX_DATA)
            break;
        size_t tail = n < 2 ? 0 : trailer;           /* the handshake frames carry no tag */
        memcpy(frame, &h, sizeof h);
        if (read_full(pp->agent, frame + sizeof h, h.len + tail) != 0)
            break;
        size_t total = sizeof h + h.len + tail;
        n++;
        if (!done && n > 2 && h.op == (CN_OP_DEVCTL | CN_REPLY) && h.len > 0) {
            done = 1;
            if (strcmp(mode, "flip") == 0) {
                frame[sizeof h] ^= 0x01;
                printf("flipped one bit in the devctl reply to seq %u\n", h.seq);
            } else if (strcmp(mode, "replay") == 0) {
                if (write_full(pp->client, frame, total) != 0)
                    break;
                printf("replayed the devctl reply to seq %u\n", h.seq);
            }
            fflush(stdout);
        }
        if (write_full(pp->client, frame, total) != 0)
            break;
    }
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: cnmitm LISTEN_PORT TARGET_HOST TARGET_PORT flip|replay|flipreq|pass TRAILER\n");
        return 2;
    }
    mode = argv[4];
    trailer = (size_t)atoi(argv[5]);
    signal(SIGPIPE, SIG_IGN);
    memset(&target, 0, sizeof target);
    target.sin_family = AF_INET;
    target.sin_port = htons((uint16_t)atoi(argv[3]));
    if (inet_pton(AF_INET, argv[2], &target.sin_addr) != 1) {
        fprintf(stderr, "cnmitm: bad address %s\n", argv[2]);
        return 2;
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in me;
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = htons((uint16_t)atoi(argv[1]));
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&me, sizeof me) != 0 || listen(ls, 4) != 0) {
        perror("cnmitm: listen");
        return 1;
    }
    printf("cnmitm: %s mode on 127.0.0.1:%s for %s:%s, tag bytes %zu\n", mode, argv[1], argv[2], argv[3], trailer);
    fflush(stdout);
    for (;;) {
        struct pair p;
        p.client = accept(ls, NULL, NULL);
        if (p.client < 0)
            continue;
        p.agent = socket(AF_INET, SOCK_STREAM, 0);
        if (p.agent < 0 || connect(p.agent, (struct sockaddr *)&target, sizeof target) != 0) {
            close(p.client);
            if (p.agent >= 0)
                close(p.agent);
            continue;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, upstream, &p) != 0) {
            close(p.client);
            close(p.agent);
            continue;
        }
        downstream(&p);
        shutdown(p.client, SHUT_RDWR);
        shutdown(p.agent, SHUT_RDWR);
        pthread_join(t, NULL);
        close(p.client);
        close(p.agent);
    }
}
