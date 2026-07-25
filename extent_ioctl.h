#ifndef _EXTENT_IOCTL_H
#define _EXTENT_IOCTL_H

#include <linux/ioctl.h>

/* Custom IOCTL command to ask kernel to print the extent list */
#define OUICHEFS_IOC_GET_EXTENTS _IO('O', 1)
#define OUICHEFS_IOC_DEFRAG_FILE _IO('O', 2)

#endif
