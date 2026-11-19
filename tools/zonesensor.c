/*
 * zonesensor: a simulated sensor for the zonal demo (a labelled simulation, not a vehicle interface).
 *
 *   zonesensor PATH LABEL UNIT MIN MAX STEP
 *
 * The reading starts at MIN and moves by STEP once per second, turning back at MIN and MAX, so a reader on
 * another machine sees it change. read returns one line, "LABEL value=N UNIT t=S", where S is the number of
 * seconds since the sensor started; devctl GET returns the reading (info value HELLO_INFO_MARK) and SET pins
 * it to a value until the next SET with -1, which lets a demo stage an event. The reading is computed from
 * the monotonic clock when it is read, so no thread or timer is needed.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include "hello_dcmd.h"

static iofunc_attr_t attr;
static resmgr_connect_funcs_t cf;
static resmgr_io_funcs_t iof;
static const char *label, *unit;
static long lo, hi, step;
static long pinned = -1;
static struct timespec t0;
static char text[160];

static long seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - t0.tv_sec);
}

static long reading(void)
{
    if (pinned >= 0)
        return pinned;
    long span = hi - lo;
    if (span <= 0 || step == 0)
        return lo;
    long a = labs(step);
    long period = 2 * span;
    long pos = (seconds() * a) % period;           /* distance travelled along a there-and-back path */
    long v = pos <= span ? lo + pos : hi - (pos - span);
    return step > 0 ? v : hi - (v - lo);
}

static int s_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (ocb->offset == 0)
        snprintf(text, sizeof text, "%s value=%ld %s t=%ld\n", label, reading(), unit, seconds());
    size_t len = strlen(text);
    size_t off = (size_t)ocb->offset < len ? (size_t)ocb->offset : len;
    size_t n = len - off;
    if (n > (size_t)_IO_READ_GET_NBYTES(msg))
        n = (size_t)_IO_READ_GET_NBYTES(msg);
    _IO_SET_READ_NBYTES(ctp, n);
    ocb->offset += n;
    if (n)
        ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_PTR(ctp, text + off, n);
}

static int s_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int st = iofunc_devctl_default(ctp, msg, ocb);
    if (st != _RESMGR_DEFAULT)
        return st;
    int *data = (int *)_DEVCTL_DATA(msg->i);
    size_t got = ctp->info.msglen > 0 ? (size_t)ctp->info.msglen : 0;
    size_t have = got >= sizeof msg->i ? got - sizeof msg->i : 0;
    int nb = 0, rv = 0;
    switch (msg->i.dcmd) {
    case DCMD_HELLO_GET:
        if (msg->i.nbytes < sizeof(int))
            return EINVAL;
        *data = (int)reading();
        nb = sizeof(int);
        rv = HELLO_INFO_MARK;
        break;
    case DCMD_HELLO_SET:
        if (msg->i.nbytes < sizeof(int) || have < sizeof(int))
            return EINVAL;
        pinned = *data < 0 ? -1 : *data;
        break;
    default:
        return ENOTTY;
    }
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.ret_val = rv;
    msg->o.nbytes = nb;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + nb);
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: zonesensor PATH LABEL UNIT MIN MAX STEP\n");
        return 2;
    }
    const char *path = argv[1];
    label = argv[2];
    unit = argv[3];
    lo = strtol(argv[4], NULL, 10);
    hi = strtol(argv[5], NULL, 10);
    step = strtol(argv[6], NULL, 10);
    if (hi < lo || lo < 0) {
        fprintf(stderr, "zonesensor: need 0 <= MIN <= MAX\n");
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        perror("zonesensor: dispatch_create");
        return 1;
    }
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = s_read;
    iof.devctl = s_devctl;
    iofunc_attr_init(&attr, S_IFCHR | 0644, NULL, NULL);
    resmgr_attr_t ra;
    memset(&ra, 0, sizeof ra);
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    if (resmgr_attach(dpp, &ra, path, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        perror("zonesensor: resmgr_attach");
        return 1;
    }
    printf("zonesensor: %s ready (%s, %s, %ld to %ld, step %ld per second; simulation)\n", path, label, unit, lo, hi,
           step);
    fflush(stdout);
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        dispatch_context_t *n = dispatch_block(ctp);
        if (n == NULL)
            continue;
        ctp = n;
        dispatch_handler(ctp);
    }
}
