/*
 * NET		Implementation of Socket uinterface
 *
 * File:	socket.c
 *
 * Author:	Alistair Riddoch, <ajr@ecs.soton.ac.uk>
 *
 * 		Based on code from the Linux kernel by
 *		Orest Zborowski <obz@Kodak.COM>
 *		Ross Biro <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen <waltje@uWalt.NL.Mugnet.ORG>
 */

#include <linuxmt/config.h>

#ifdef CONFIG_SOCKET

#include <linuxmt/errno.h>
#include <linuxmt/socket.h>
#include <linuxmt/net.h>
#include <linuxmt/fs.h>
#include <linuxmt/mm.h>
#include <linuxmt/sched.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/stat.h>

#include <arch/segment.h>

#if 0
#define find_protocol_family(_a)	0
#endif

#define MAX_SOCK_ADDR 16	/* Must be much bigger for AF_UNIX */

static struct proto_ops *pops[NPROTO] = { NULL, NULL, NULL };

extern struct net_proto protocols[];	/* Network protocols */

static int find_protocol_family(int family)
{
    int i;

    for (i = 0; i < NPROTO; i++)
	if (pops[i]->family == family)
	    return i;

    return -1;
}

struct socket *socki_lookup(struct inode *inode)
{
    return &inode->u.socket_i;
}

/* FIXME - Could above be replaced by
 *
 * 	#define socki_lookup(_a) (&_a->u.sock_i)
 */

int move_addr_to_kernel(char *uaddr, size_t ulen, char *kaddr)
{
    if (ulen < 0 || ulen > MAX_SOCK_ADDR)
	return -EINVAL;

    if (ulen == 0)
	return 0;

    return verified_memcpy_fromfs(kaddr, uaddr, ulen);
}

int move_addr_to_user(char *kaddr, size_t klen, char *uaddr, register int *ulen)
{
    size_t len;
    int err;

    if ((err = verified_memcpy_fromfs(&len, ulen, sizeof(*ulen))))
	return err;

    if (len > klen)
	len = klen;

    if (len < 0 || len > MAX_SOCK_ADDR)
	return -EINVAL;

    if (len)
	if ((err = verified_memcpy_tofs(uaddr, kaddr, len)))
	    return err;

#if 0
    put_user(len, ulen);	/* This is pointless isn't it */
#endif

    return 0;
}

struct socket *sock_alloc(void)
{
    register struct inode *inode;
    register struct socket *sock;

    inode = get_empty_inode();
    if (!inode)
	return NULL;

    inode->i_mode = S_IFSOCK;
    inode->i_sock = 1;
    inode->i_uid = current->uid;
    inode->i_gid = (__u8) current->gid;

    sock = &inode->u.socket_i;
    sock->state = SS_UNCONNECTED;
    sock->flags = 0;
    sock->ops = NULL;
    sock->data = NULL;

#if defined(CONFIG_INET)
    sock->avail_data = 0;
#endif

    sock->file = NULL;

#if defined(CONFIG_UNIX) || defined(CONFIG_NANO) || defined(CONFIG_INET)
    sock->conn = sock->iconn = sock->next = NULL;
#endif

    sock->wait = &inode->i_wait;
    sock->inode = inode;	/* "backlink" use pointer arithmetic instead */
    sock->fasync_list = NULL;

#if 0

    sockets_in_use++;		/* Maybe we'll find a use for this */

#endif

    return sock;
}

struct socket *sockfd_lookup(int fd, struct file **pfile)
{
    register struct file *file;
    register struct inode *inode;

    if (fd < 0 || fd >= NR_OPEN || !(file = current->files.fd[fd]))
	return NULL;

    inode = file->f_inode;
    if (!inode || !inode->i_sock)
	return NULL;

    if (pfile)
	*pfile = file;

    return socki_lookup(inode);
}

static int sock_read(struct inode *inode, struct file *file,
		     register char *ubuf, size_t size)
{
    struct socket *sock;
    int err;

    if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_read: can't find socket for inode!\n");
	return -EBADF;
    }

    if (sock->flags & SO_ACCEPTCON)
	return -EINVAL;

    if (size < 0)
	return -EINVAL;

    if (size == 0)
	return 0;

    if ((err = verify_area(VERIFY_WRITE, ubuf, size)) < 0)
	return err;

    return sock->ops->read(sock, ubuf, size, (file->f_flags & O_NONBLOCK));
}

static int sock_write(struct inode *inode, struct file *file,
		      register char *ubuf, size_t size)
{
    struct socket *sock;
    int err;

    if (!(sock = socki_lookup(inode))) {
	printk("NET: sock_write: can't find socket for inode!\n");
	return -EBADF;
    }

    if (sock->flags & SO_ACCEPTCON)
	return -EINVAL;

    if (size < 0)
	return -EINVAL;

    if (size == 0)
	return 0;

    if ((err = verify_area(VERIFY_READ, ubuf, size)) < 0)
	return err;

    return sock->ops->write(sock, ubuf, size, (file->f_flags & O_NONBLOCK));
}

static int sock_select(struct inode *inode,
		       struct file *file, int sel_type, select_table * wait)
{
    register struct socket *sock;

    if (!(sock = socki_lookup(inode)))
	return -EBADF;

    if (sock->ops && sock->ops->select)
	return sock->ops->select(sock, sel_type, wait);

    return 0;
}

int sock_awaitconn(register struct socket *mysock,
		   struct socket *servsock, int flags)
{
    register struct socket *last;

    /*
     *      We must be listening
     */
    if (!(servsock->flags & SO_ACCEPTCON))
	return -EINVAL;

    /*
     *      Put ourselves on the server's incomplete connection queue. 
     */
    mysock->next = NULL;

    clr_irq();
    if (!(last = servsock->iconn))
	servsock->iconn = mysock;
    else {
	while (last->next)
	    last = last->next;
	last->next = mysock;
    }
    mysock->state = SS_CONNECTING;
    mysock->conn = servsock;
    set_irq();

    /*
     * Wake up server, then await connection. server will set state to
     * SS_CONNECTED if we're connected.
     */

    wake_up_interruptible(servsock->wait);

#if 0
    sock_wake_async(servsock, 0);	/* I don't think we need this */
#endif

    if (mysock->state != SS_CONNECTED) {
	if (flags & O_NONBLOCK)
	    return -EINPROGRESS;

	interruptible_sleep_on(mysock->wait);
	if (mysock->state != SS_CONNECTED && mysock->state != SS_DISCONNECTING) {
	    /*
	     * if we're not connected we could have been
	     * 1) interrupted, so we need to remove ourselves
	     *    from the server list
	     * 2) rejected (mysock->conn == NULL), and have
	     *    already been removed from the list
	     */
	    if (mysock->conn == servsock) {
		clr_irq();
		if ((last = servsock->iconn) == mysock)
		    servsock->iconn = mysock->next;
		else {
		    while (last->next != mysock)
			last = last->next;
		    last->next = mysock->next;
		}
		set_irq();
	    }
	    if (mysock->conn)
		return -EINTR;
	    else
		return -EACCES;
	}
    }
    return 0;
}


#if defined(CONFIG_UNIX) || defined(CONFIG_NANO) || defined(CONFIG_INET)
static void sock_release_peer(struct socket *peer)
{
    /* FIXME - some of these are not implemented */
    peer->state = SS_DISCONNECTING;
    wake_up_interruptible(peer->wait);

#if 0
    sock_wake_async(peer, 1);
#endif

}
#endif

void sock_release(register struct socket *sock)
{
    int oldstate;
    register struct socket *peersock;

#if defined(CONFIG_UNIX) || defined(CONFIG_NANO) || defined(CONFIG_INET)
    struct socket *nextsock;
#endif

    if ((oldstate = sock->state) != SS_UNCONNECTED)
	sock->state = SS_DISCONNECTING;

#if defined(CONFIG_UNIX) || defined(CONFIG_NANO) || defined(CONFIG_INET)
    for (peersock = sock->iconn; peersock; peersock = nextsock) {
	nextsock = peersock->next;
	sock_release_peer(peersock);
    }

    peersock = (oldstate == SS_CONNECTED) ? sock->conn : NULL;
#else
    peersock = NULL;		/* sock-conn is always NULL for an INET socket */
#endif

    if (sock->ops)
	sock->ops->release(sock, peersock);

#if defined(CONFIG_UNIX) || defined(CONFIG_NANO) || defined(CONFIG_INET)
    if (peersock)
	sock_release_peer(peersock);
#endif

#if 0
    sockets_in_use--;		/* maybe we'll find a use for this */
#endif

    sock->file = NULL;
    iput(SOCK_INODE(sock));
}

void sock_close(register struct inode *inode, struct file *filp)
{
    if (!inode)
	return;

#if 0
    sock_fasync(inode, filp, 0);
#endif

    sock_release(socki_lookup(inode));
}

int sys_bind(int fd, struct sockaddr *umyaddr, int addrlen)
{
    register struct socket *sock;
    char address[MAX_SOCK_ADDR];
    int err, i;

#if 0
    /* This is done in sockfd_lookup, so can be scrubbed later */
    if (fd < 0 || fd >= NR_OPEN || (current->files.fd[fd] == NULL))
	return -EBADF;
#endif

    if (!(sock = sockfd_lookup(fd, NULL)))
	return -ENOTSOCK;

    err = move_addr_to_kernel((char *) umyaddr, (size_t) addrlen, address);
    if (err < 0)
	return err;

    if ((i = sock->ops->bind(sock, (struct sockaddr *) address, addrlen)) < 0)
	return i;

    return 0;
}

/*@-type@*/

static struct file_operations socket_file_ops = {
    NULL,			/* lseek */
    sock_read,			/* read */
    sock_write,			/* write */
    NULL,			/* readdir */
    sock_select,		/* select */
    NULL,			/* ioctl */
    NULL,			/* open */
    sock_close			/* close */
#ifdef BLOAT_FS
	,
    NULL,			/* fsync */
    NULL,			/* check_media_change */
    NULL			/* revalidate */
#endif
};

/*@+type@*/

int get_fd(register struct inode *inode)
{
    int fd;

    fd = get_unused_fd();
    if (fd >= 0) {
	struct file *file = get_empty_filp();
	if (!file) {
	    return -ENFILE;
	}

	current->files.fd[fd] = file;
	file->f_op = &socket_file_ops;
	file->f_mode = 3;
	file->f_flags = O_RDWR;
	file->f_count = 1;
	file->f_inode = inode;
	if (inode)
	    inode->i_count++;
	file->f_pos = 0;
    }
    return fd;
}

#ifdef CONFIG_SOCK_CLIENTONLY

int sys_listen(int fd, int backlog)
{
    return -EINVAL;
}

int sys_accept(int fd, struct sockaddr *upeer_sockaddr, int *upeer_addrlen)
{
    return -EINVAL;
}

#else

int sys_listen(int fd, int backlog)
{
    register struct socket *sock;

#if 0

    /* This is done in sockfd_lookup, so can be scrubbed later */
    if (fd < 0 || fd >= NR_OPEN || current->files.fd[fd] == NULL)
	return -EBADF;

#endif

    if (!(sock = sockfd_lookup(fd, NULL)))
	return -ENOTSOCK;

    if (sock->state != SS_UNCONNECTED)
	return -EINVAL;

    if (sock->ops && sock->ops->listen)
	sock->ops->listen(sock, backlog);

    sock->flags |= SO_ACCEPTCON;
    return 0;

}

int sys_accept(int fd, struct sockaddr *upeer_sockaddr, int *upeer_addrlen)
{
    struct file *file;
    register struct socket *sock;
    register struct socket *newsock;
    char address[MAX_SOCK_ADDR];
    size_t len;
    int i;

#if 0

    /* This is done in sockfd_lookup, so can be scrubbed later */
    if (fd < 0 || fd >= NR_OPEN || current->files.fd[fd] == NULL)
	return -EBADF;

#endif

    if (!(sock = sockfd_lookup(fd, &file)))
	return -ENOTSOCK;

    if (sock->state != SS_UNCONNECTED)
	return -EINVAL;

    if (!(sock->flags & SO_ACCEPTCON))
	return -EINVAL;

    if (!(newsock = sock_alloc())) {
	printk("NET: sock_accept: no more sockets\n");
	return -ENOSR;		/* Was EAGAIN, but we are out of system resources! */
    }

    newsock->type = sock->type;
    newsock->ops = sock->ops;
    if ((i = sock->ops->dup(newsock, sock)) < 0) {
	sock_release(newsock);
	return i;
    }

    i = newsock->ops->accept(sock, newsock, file->f_flags);
    if (i < 0) {
	sock_release(newsock);
	return i;
    }

    if ((fd = get_fd(SOCK_INODE(newsock))) < 0) {
	sock_release(newsock);
	return -EINVAL;
    }

    if (upeer_sockaddr) {
	newsock->ops->getname(newsock, (struct sockaddr *) address, &len, 1);
	move_addr_to_user(address, len, (char *) upeer_sockaddr, upeer_addrlen);
    }

    return fd;
}

#endif

int sys_connect(int fd, struct sockaddr *uservaddr, int addrlen)
{
    register struct socket *sock;
    struct file *file;
    char address[MAX_SOCK_ADDR];
    int err, i;

#if 0
/* All this is done in sockfd_lookup */
    if (fd < 0 || fd >= NR_OPEN || (file = current->files.fd[fd]) == NULL)
	return -EBADF;
#endif

    if (!(sock = sockfd_lookup(fd, &file)))
	return -ENOTSOCK;

    err = move_addr_to_kernel((char *) uservaddr, (size_t) addrlen, address);
    if (err < 0)
	return err;

    switch (sock->state) {
    case SS_UNCONNECTED:
	/* This is ok... continue with connect */
	break;
    case SS_CONNECTED:
	/* Socket is already connected */
	return -EISCONN;
    case SS_CONNECTING:
	/* Not yet connected... we will check this. */

	/*
	 *      FIXME:  For all protocols what happens if you
	 *              start an async connect fork and both
	 *              children connect.
	 *
	 *              Clean this up in the protocols!
	 */

	break;
    default:
	return -EINVAL;
    }
    i = sock->ops->connect(sock, (struct sockaddr *) address, addrlen,
			   file->f_flags);
    if (i < 0)
	return i;

    return 0;
}

int sock_register(int family, struct proto_ops *ops)
{
    int i;

    for (i = 0; i < NPROTO; i++) {
	if (pops[i] != NULL)
	    continue;
	pops[i] = ops;
	pops[i]->family = family;
	return i;
    }
    return -ENOMEM;
}

void proto_init(void)
{
    struct net_proto *pro;

    /* Kick all configured protocols. */
    pro = protocols;
    while (pro->name != NULL) {
	(*pro->init_func) (pro);
	pro++;
    }
    /* We're all done... */
}

void sock_init(void)
{
    printk("ELKS Sockets.\n");

    proto_init();
}

int sys_socket(int family, int type, int protocol)
{
    register struct socket *sock;
    register struct proto_ops *ops;
    int i, fd;

/*	find_protocol_family() is a macro which gives 0 while only
 *	AF_INET sockets are supported
 */
    if ((i = find_protocol_family(family)) < 0) {
	return -EINVAL;
    }

    ops = pops[i];		/* Initially pops is not an array. */

    if (type != SOCK_STREAM)
	return -EINVAL;

    if (!(sock = sock_alloc()))
	return -ENOSR;

    sock->type = (short int) type;
    sock->ops = ops;
    if ((i = sock->ops->create(sock, protocol)) < 0) {
	sock_release(sock);
	return i;
    }

    if ((fd = get_fd(SOCK_INODE(sock))) < 0) {
	sock_release(sock);
	return -EINVAL;
    }

    sock->file = current->files.fd[fd];

    return fd;
}

#endif
