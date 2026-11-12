/*
 * hellodev: a tiny test device for Crossnode's checks.
 *
 * read returns "hello from <hostname>\n"; devctl GET, SET and ADD work on one integer, and GET
 * returns HELLO_INFO_MARK as its info value so the info field's trip across machines is visible.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "hello_dcmd.h"

static iofunc_attr_t attr;
static resmgr_connect_funcs_t cf;
static resmgr_io_funcs_t iof;
static int value;
static char text[160];

static int h_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
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

static int h_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
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
        *data = value;
        nb = sizeof(int);
        rv = HELLO_INFO_MARK;
        break;
    case DCMD_HELLO_SET:
        if (msg->i.nbytes < sizeof(int) || have < sizeof(int))
            return EINVAL;
        value = *data;
        break;
    case DCMD_HELLO_ADD:
        if (msg->i.nbytes < sizeof(int) || have < sizeof(int))
            return EINVAL;
        value += *data;
        *data = value;
        nb = sizeof(int);
        break;
    default:
        return ENOTTY;
    }
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.ret_val = rv;
    msg->o.nbytes = (uint32_t)nb;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + (size_t)nb);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/hello";
    char host[64] = "unknown";
    gethostname(host, sizeof host - 1);
    snprintf(text, sizeof text, "hello from %s\n", host);

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        perror("hellodev: dispatch_create");
        return 1;
    }
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = h_read;
    iof.devctl = h_devctl;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    resmgr_attr_t ra;
    memset(&ra, 0, sizeof ra);
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    if (resmgr_attach(dpp, &ra, path, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        perror("hellodev: resmgr_attach");
        return 1;
    }
    printf("hellodev: %s ready\n", path);
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
