/*
 * Linux /proc/sys/fs metrics cluster
 *
 * Copyright (c) 2014,2016 Red Hat.
 * Copyright (c) 2003,2004 Silicon Graphics, Inc.  All Rights Reserved.
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
#include "linux.h"
#include "proc_sys_fs.h"

int
refresh_proc_sys_fs(proc_sys_fs_t *proc_sys_fs)
{
    static int err_reported;
    char buf[MAXPATHLEN];
    FILE *filesp = NULL;
    FILE *inodep = NULL;
    FILE *dentryp = NULL;

    memset(proc_sys_fs, 0, sizeof(proc_sys_fs_t));

    if ((filesp  = linux_statsfile("/proc/sys/fs/file-nr", buf, sizeof(buf))) == NULL ||
	(inodep  = linux_statsfile("/proc/sys/fs/inode-state", buf, sizeof(buf))) == NULL ||
	(dentryp = linux_statsfile("/proc/sys/fs/dentry-state", buf, sizeof(buf))) == NULL) {
	proc_sys_fs->errcode = -oserror();
	if (err_reported == 0)
	    fprintf(stderr, "Warning: vfs metrics are not available : %s\n",
		    osstrerror());
    }
    else {
	proc_sys_fs->errcode = 0;
	if (fscanf(filesp,  "%d %d %d",
			&proc_sys_fs->fs_files_count,
			&proc_sys_fs->fs_files_free,
			&proc_sys_fs->fs_files_max) != 3)
	    proc_sys_fs->errcode = PM_ERR_VALUE;
	if (fscanf(inodep,  "%d %d",
			&proc_sys_fs->fs_inodes_count,
			&proc_sys_fs->fs_inodes_free) != 2)
	    proc_sys_fs->errcode = PM_ERR_VALUE;
	if (fscanf(dentryp, "%d %d",
			&proc_sys_fs->fs_dentry_count,
			&proc_sys_fs->fs_dentry_free) != 2)
	    proc_sys_fs->errcode = PM_ERR_VALUE;
	if (pmDebugOptions.libpmda) {
	    if (proc_sys_fs->errcode == 0)
		fprintf(stderr, "refresh_proc_sys_fs: found vfs metrics\n");
	    else
		fprintf(stderr, "refresh_proc_sys_fs: botch! missing vfs metrics\n");
	}
    }
    if (filesp)
	fclose(filesp);
    if (inodep)
	fclose(inodep);
    if (dentryp)
	fclose(dentryp);

    if (!err_reported)
	err_reported = 1;

    if (proc_sys_fs->errcode == 0)
	return 0;
    return -1;
}
