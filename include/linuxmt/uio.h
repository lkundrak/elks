#ifndef __LINUXMT_UIO_H__
#define __LINUXMT_UIO_H__

struct iovec
{
    void *iov_base;		/* BSD uses caddr_t (same thing in effect) */
    int iov_len;
};

#define UIO_MAXIOV	16

#endif
