/*
 * Linux sysfs_kernel cluster
 *
 * Copyright (c) 2009,2014,2016 Red Hat.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include <ctype.h>
#include "linux.h"
#include "sysfs_kernel.h"

int
refresh_sysfs_kernel(sysfs_kernel_t *sk)
{
    char buf[MAXPATHLEN];
    int fd, n;

    pmsprintf(buf, sizeof(buf), "%s/sys/kernel/uevent_seqnum", linux_statspath);
    if ((fd = open(buf, O_RDONLY)) < 0) {
    	sk->valid_uevent_seqnum = 0;
	return -oserror();
    }

    if ((n = read(fd, buf, sizeof(buf))) <= 0)
    	sk->valid_uevent_seqnum = 0;
    else {
	buf[n-1] = '\0';
    	sscanf(buf, "%llu", (long long unsigned int *)&sk->uevent_seqnum);
	sk->valid_uevent_seqnum = 1;
    }
    close(fd);
    return 0;
}
