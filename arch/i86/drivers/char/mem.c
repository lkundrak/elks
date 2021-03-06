/*
 * ELKS implmentation of memory devices
 * /dev/null, /dev/zero, /dev/mem, /dev/kmem, etc...
 *
 * Heavily inspired by linux/drivers/char/mem.c
 */

/* for reference
 * /dev/mem refers to physical memory
 * /dev/kmem refers to _virtual_ address space
 * Currently these will be the same, but eventually, once ELKS has
 * EMS, etc, we'll want to change these.
 */

#include <linuxmt/config.h>

#ifdef CONFIG_CHAR_DEV_MEM

#include <linuxmt/kernel.h>
#include <linuxmt/major.h>
#include <linuxmt/fs.h>
#include <linuxmt/errno.h>
#include <linuxmt/mm.h>
#include <linuxmt/debug.h>
#include <linuxmt/mem.h>

#include <arch/segment.h>

#define DEV_MEM_MINOR		1
#define DEV_KMEM_MINOR		2
#define DEV_NULL_MINOR		3
#define DEV_PORT_MINOR		4
#define DEV_ZERO_MINOR		5

#define DEV_FULL_MINOR		7
#define DEV_RANDOM_MINOR	8
#define DEV_URANDOM_MINOR	9

/*
 * generally useful code...
 */
loff_t memory_lseek(struct inode *inode, register struct file *filp,
		    loff_t offset, unsigned int origin)
{
    loff_t tmp = -1;

    debugmem("mem_lseek()\n");
    switch (origin) {
    case 0:
	tmp = offset;
	break;
    case 1:
	tmp = filp->f_pos + offset;
	break;
    default:
	return -EINVAL;
    }
    if (tmp < 0)
	return -EINVAL;
    if (tmp != filp->f_pos) {
	filp->f_pos = tmp;

#ifdef BLOAT_FS
	filp->f_reada = 0;
	filp->f_version = ++event;
#endif

    }
    return 0;
}

/*
 * /dev/null code
 */
loff_t null_lseek(struct inode *inode, struct file *filp,
		  off_t offset, int origin)
{
    debugmem("null_lseek()\n");
    return (filp->f_pos = 0);
}

int null_read(struct inode *inode, struct file *filp, char *data, int len)
{
    debugmem("null_read()\n");
    return 0;
}

int null_write(struct inode *inode, struct file *filp, char *data, int len)
{
    debugmem1("null write: ignoring %d bytes!\n", len);
    return len;
}

/*
 * /dev/full code
 */
int full_read(struct inode *inode, struct file *filp, char *data, int len)
{
    debugmem("full_read()\n");
    filp->f_pos += len;
    return len;
}

int full_write(struct inode *inode, struct file *filp, char *data, int len)
{
    debugmem1("full_write: objecting to %d bytes!\n", len);
    return -ENOSPC;
}

/*
 * /dev/zero code
 */
int zero_read(struct inode *inode, struct file *filp, char *data, int len)
{
    debugmem("zero_read()\n");
    fmemset((__u16) data, current->mm.dseg, 0, (__u16) len);
    filp->f_pos += len;
    return len;
}

static void split_seg_off(unsigned short int *segment,
			  unsigned short int *offset,
			  long int posn)
{
    *segment = (unsigned short int) (((unsigned long int) posn) >> 4);
    *offset  = (unsigned short int) (((unsigned long int) posn) & 0xF);
}

/*
 * /dev/kmem (and currently also mem) code
 */
loff_t kmem_read(struct inode *inode, register struct file *filp,
	      char *data, size_t len)
{
    unsigned short int sseg, soff;

    debugmem("[k]mem_read()\n");
    split_seg_off(&sseg, &soff, filp->f_pos);
    debugmem3("Reading %u %p %p.\n", len, sseg, soff);
    fmemcpy(current->mm.dseg, (__u16) data, sseg, soff, (__u16) len);
    filp->f_pos += len;
    return (loff_t) len;
}

int kmem_write(struct inode *inode, register struct file *filp,
	       char *data, size_t len)
{
    unsigned short int dseg, doff;

    debugmem("[k]mem_write()\n");

    split_seg_off(&dseg, &doff, filp->f_pos);
    debugmem2("Writing to %d:%d\n", dseg, doff);
    fmemcpy(dseg, doff, current->mm.dseg, (__u16) data, (__u16) len);
    filp->f_pos += len;
    return (int) len;
}

int kmem_ioctl(struct inode *inode, struct file *file, int cmd, char *arg)
{
    char *i;
    struct mem_usage mu;

#ifdef CONFIG_SWAP
    struct mem_swap_info si;
#endif

    debugmem1("[k]mem_ioctl() %d\n", cmd);
    switch (cmd) {

#ifdef CONFIG_MODULES

    case MEM_GETMODTEXT:
	i = (char *) module_init;
	memcpy_tofs(arg, &i, 2);
	return 0;
    case MEM_GETMODDATA:
	i = (char *) module_data;
	memcpy_tofs(arg, &i, 2);
	return 0;

#endif

    case MEM_GETTASK:
	i = (char *) task;
	memcpy_tofs(arg, &i, 2);

#if 0

	/* Include this code to make ps dump memory info. The dmem()
	 * function must also be included in arch/i86/mm/malloc.c
	 */
	dmem();

#endif

	return 0;
    case MEM_GETCS:
	i = (char *) get_cs();
	memcpy_tofs(arg, &i, 2);
	return 0;
    case MEM_GETDS:
	i = (char *) get_ds();
	memcpy_tofs(arg, &i, 2);
	return 0;
    case MEM_GETUSAGE:
	mu.free_memory = mm_get_usage(MM_MEM, 0);
	mu.used_memory = mm_get_usage(MM_MEM, 1);

#ifdef CONFIG_SWAP
	mu.free_swap = mm_get_usage(MM_SWAP, 0);
	mu.used_swap = mm_get_usage(MM_SWAP, 1);
#else
	mu.free_swap = mu.used_swap = 0;
#endif

	memcpy_tofs(arg, &mu, sizeof(struct mem_usage));
	
	return 0;

#ifdef CONFIG_SWAP
    case MEM_SETSWAP:
    	if(!suser())
    	   return -EPERM;
    	memcpy_fromfs(&si, arg, sizeof(struct mem_swap_info));
    	return mm_swap_on(&si);
#endif
    }
    return -EINVAL;
}

/*@-type@*/

static struct file_operations null_fops = {
    null_lseek,			/* lseek */
    null_read,			/* read */
    null_write,			/* write */
    NULL,			/* readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    NULL,			/* open */
    NULL			/* release */
#ifdef BLOAT_FS
	,
    NULL,			/* fsync */
    NULL,			/* check_media_change */
    NULL			/* revalidate */
#endif
};

static struct file_operations full_fops = {
    memory_lseek,		/* lseek */
    full_read,			/* read */
    full_write,			/* write */
    NULL,			/* readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    NULL,			/* open */
    NULL			/* release */
#ifdef BLOAT_FS
	,
    NULL,			/* fsync */
    NULL,			/* check_media_change */
    NULL			/* revalidate */
#endif
};

static struct file_operations zero_fops = {
    memory_lseek,		/* lseek */
    zero_read,			/* read */
    null_write,			/* write */
    NULL,			/* readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    NULL,			/* open */
    NULL			/* release */
#ifdef BLOAT_FS
	,
    NULL,			/* fsync */
    NULL,			/* check_media_change */
    NULL			/* revalidate */
#endif
};

static struct file_operations kmem_fops = {
    memory_lseek,		/* lseek */
    kmem_read,			/* read */
    kmem_write,			/* write */
    NULL,			/* readdir */
    NULL,			/* select */
    kmem_ioctl,			/* ioctl */
    NULL,			/* open */
    NULL			/* release */
#ifdef BLOAT_FS
	,
    NULL,			/* fsync */
    NULL,			/* check_media_change */
    NULL			/* revalidate */
#endif
};

/*@+type@*/

/*
 * memory device open multiplexor
 */
int memory_open(struct inode *inode, register struct file *filp)
{
    int err=0;

    debugmem1("memory_open: minor = %d; it's /dev/",
	      (int) MINOR(inode->i_rdev));
    switch (MINOR(inode->i_rdev)) {

    case DEV_NULL_MINOR:
	debugmem("null");
	filp->f_op = &null_fops;
	break;

    case DEV_FULL_MINOR:
	debugmem("full");
	filp->f_op = &full_fops;
	break;

    case DEV_ZERO_MINOR:
	debugmem("zero");
	filp->f_op = &zero_fops;
	break;

    /*  The following two entries assume that virtual memory is identical
     *  to physical memory. Is this still true now we have swap?
     */

    case DEV_KMEM_MINOR:
	debugmem("k");
	/* Fallthru */

    case DEV_MEM_MINOR: 
	debugmem("mem");
	filp->f_op = &kmem_fops;
	break;

#ifdef DEBUGMM

    /*  The following entries are all currently subsumed by the default entry.
     *  However, if debugging is enabled, we include them to print out the
     *  correct device name rather than the ??? that would otherwise appear.
     */

    case DEV_PORT_MINOR:
	debugmem("port");
	err = -ENXIO;
	break;

    case DEV_RANDOM_MINOR:
	debugmem("random");
	err = -ENXIO;
	break;

    case DEV_URANDOM_MINOR:
	debugmem("urandom");
	err = -ENXIO;
	break;

#endif

    default:
	debugmem("???");
	err = -ENXIO;
	break;
    }
    debugmem(" !!!\n");
    if (err)
	printk("Device minor %d not supported.\n", MINOR(inode->i_rdev));
    return err;
}

/*@-type@*/

static struct file_operations memory_fops = {
    NULL,			/* lseek */
    NULL,			/* read */
    NULL,			/* write */
    NULL,			/* readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    memory_open,		/* open */
    NULL			/* release */
#ifdef BLOAT_FS
	,
    NULL,			/* fsync */
    NULL,			/* check_media_change */
    NULL			/* revalidate */
#endif
};

/*@+type@*/

void mem_dev_init(void)
{
    if (register_chrdev(MEM_MAJOR, "mem", &memory_fops))
	printk("MEM: Unable to get major %d for memory devices\n", MEM_MAJOR);
}

#endif
