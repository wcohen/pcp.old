/*
 * The process PMDA
 * 
 * This code monitors root processes to see if they are still 
 * running.  The processes that are monitored are stored in a config file,
 * and the PMDA returns the number of root processes with that name
 * running.  In this way, you can check to see if the number of processes
 * is in a range, is 1, is 0, or some other combination.
 *
 * Copyright (c) 2001 Alan Bailey (bailey@mcs.anl.gov or abailey@ncsa.uiuc.edu) 
 * for the portions of the code supporting the initial agent functionality.
 * All rights reserved. 
 *
 * Copyright (c) 2001,2003,2004 Silicon Graphics, Inc.  All Rights Reserved.
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

#include "pmapi.h"
#include "pmda.h"
#include "domain.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/*
 * Process PMDA
 *
 * Metrics
 *   process.running   
 *     The number of root processes running for each name
 *     The process names are stored in
 *       PCP_VAR_DIR/pmdas/process/process.conf
 */

static pmdaInstid   *processes;

static pmdaIndom indomtab[] = {
#define PROC_INDOM    0
  { PROC_INDOM, 0, NULL }
};

static pmdaMetric metrictab[] = {
/* process.running */
    { NULL,
      { PMDA_PMID(0,0), PM_TYPE_DOUBLE, PROC_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
};

/* Variables needed for this file */
static int *num_procs;
static struct stat file_change;
static int npidlist;
static int maxpidlist;
static int *pidlist;
static int isDSO = 1;
static char mypath[MAXPATHLEN];

/* Routines in this file */
static void process_clear_config_info(void);
static void process_grab_config_info(void);
static void process_config_file_check(void);
static int process_refresh_pidlist(void);

static void
process_config_file_check(void)
{
    struct stat statbuf;
    static int last_error;
    int sep = pmPathSeparator();

    pmsprintf(mypath, sizeof(mypath), "%s%c" "process" "%c" "process.conf",
		pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
    if (stat(mypath, &statbuf) == -1) {
	if (oserror() != last_error) {
	    last_error = oserror();
	    pmNotifyErr(LOG_WARNING, "stat failed on %s: %s\n", 
			mypath, pmErrStr(-oserror()));
	}
    } else {
	last_error = 0;
#if defined(HAVE_ST_MTIME_WITH_E)
	if (memcmp(&statbuf.st_mtime, &file_change.st_mtime, sizeof(statbuf.st_mtime)) != 0)
#elif defined(HAVE_ST_MTIME_WITH_SPEC)
	if (memcmp(&statbuf.st_mtimespec, &file_change.st_mtimespec, sizeof(statbuf.st_mtimespec)) != 0)
#else
	if (memcmp(&statbuf.st_mtim, &file_change.st_mtim, sizeof(statbuf.st_mtim)) != 0)
#endif
	{
	    process_clear_config_info();
	    process_grab_config_info();
	    file_change = statbuf;
	}
    }
}

/*
 * This function is called when the config file needs to be read in
 * again, so we can define new instances and such.  This frees the
 * previous memory, if any (on startup, there won't be any), and sets
 * the two variables to NULL for fun.
 */
static void
process_clear_config_info(void)
{
    int i;

    /* Free the memory holding the process name */
    for (i = 0; i < indomtab[PROC_INDOM].it_numinst; i++)
	free(processes[i].i_name);

    /* Free the processes structure */
    if (processes)
	free(processes);

    /* Free the process counter structure */
    if (num_procs)
	free(num_procs);
    num_procs = NULL;

    indomtab[PROC_INDOM].it_set = processes = NULL;
    indomtab[PROC_INDOM].it_numinst = 0;
}

/*
 * This routine opens the config file and stores the information in the
 * processes structure.  The processes structure must be reallocated as
 * necessary, and also the num_procs structure needs to be reallocated
 * as we define new processes.  When all of that is done, we fill in the
 * values in the indomtab structure, those being the number of instances
 * and the pointer to the processes structure.
 *
 * A lot of this code was taken from the simple.c code, but in this
 * case, I changed a lot of it to fit my purposes...
 */
static void
process_grab_config_info(void)
{
    FILE *fp;
    char process_name[1024];
    char *q;
    size_t size;
    int process_number = 0;
    int sep = pmPathSeparator();

    pmsprintf(mypath, sizeof(mypath), "%s%c" "process" "%c" "process.conf",
		pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
    if ((fp = fopen(mypath, "r")) == NULL) {
	pmNotifyErr(LOG_ERR, "fopen on %s failed: %s\n",
		mypath, pmErrStr(-oserror()));
	if (processes != NULL) {
	    free(processes);
	    processes = NULL;
	    process_number = 0;
	}
	goto done;
    }

    while (fgets(process_name, sizeof(process_name), fp) != NULL) {
	if (process_name[0] == '#')
	    continue;
	/* Remove the newline */
	if ((q = strchr(process_name, '\n')) != NULL) {
	    *q = '\0';
	} else {
	    /* This means the line was too long */
	    pmNotifyErr(LOG_WARNING, "line %d in config file too long\n",
			process_number + 1);
	}
	size = (process_number + 1) * sizeof(pmdaInstid);
	if ((processes = realloc(processes, size)) == NULL)
	    pmNoMem("process", size, PM_FATAL_ERR);
	processes[process_number].i_name = malloc(strlen(process_name) + 1);
	strcpy(processes[process_number].i_name, process_name);
	processes[process_number].i_inst = process_number;
	process_number++;
    }
    fclose(fp);

done:
    if (processes == NULL)
	pmNotifyErr(LOG_WARNING, "\"process\" instance domain is empty");

    indomtab[PROC_INDOM].it_set = processes;
    indomtab[PROC_INDOM].it_numinst = process_number;

    num_procs = realloc(num_procs, (process_number)*sizeof(int));
}

/*
 * This code refreshes the count of all the processes that we want to
 * monitor.  It does this by calling process_refresh_pidlist, which just
 * grabs a list of all the running processes (as PIDs).  We then loop
 * through each of these processes, and check to see if it is a root
 * process.  If so, we then open up the /proc/<PID>/status file,
 * and see what the process name is.  If it is one of our special
 * processes that we want to track, then we up the count for that
 * process in num_procs.  Then we're done.
 */
static void
process_refresh_pid_checks(void)
{
    int proc;
    char proc_path[30];
    char cmd_line[200];
    FILE *fd;
    int proc_name;
    int item;
    struct stat statbuf;

    process_refresh_pidlist();

    /* Clear the variables */
    for (item = 0; item < indomtab[PROC_INDOM].it_numinst; item++)
	num_procs[item] = 0;

    for (proc = 0; proc < npidlist; proc++) {
	pmsprintf(proc_path, sizeof(proc_path), "/proc/%d/status", pidlist[proc]);
	if (stat(proc_path, &statbuf) == -1) {
	    /* We can't stat it, let's assume the process isn't running anymore */
	    continue;
	} else {
	    /* This checks to make sure the process is a root process */
	    if (statbuf.st_uid != 0) {
		continue;
	    }
	}

	if ((fd = fopen(proc_path, "r")) != NULL) {
	    /*
	     * matching process by name is platform specific ... this code
	     * works for Linux when /proc/<pid>/status is ascii and the
	     * first line is of the form:
	     * Name:   <cmdname>
	     */
	    if (fgets(cmd_line, sizeof(cmd_line), fd) == NULL) {
		/*
		 * not expected, but not a disaster ... we'll just not try
		 * to match this one
		 */
		fclose(fd);
		continue;
	    }
	    fclose(fd);

	    for (proc_name = 0; proc_name < indomtab[PROC_INDOM].it_numinst; proc_name++) {
		if (strstr(cmd_line, (processes[proc_name]).i_name)) {
		    (num_procs[proc_name])++;
		}
	    }
	} 
    }
}

/*
 * Read in the list of all processes running.
 */
int
process_refresh_pidlist()
{
    DIR *dirp = opendir("/proc");
    struct dirent *dp;

    if (dirp == NULL)
	return -oserror();

    npidlist = 0;
    while ((dp = readdir(dirp)) != (struct dirent *)NULL) {
	if (isdigit((int)dp->d_name[0])) {
	    if (npidlist >= maxpidlist) {
		maxpidlist += 16;
		pidlist = (int *)realloc(pidlist, maxpidlist * sizeof(int));
	    }
	    pidlist[npidlist++] = atoi(dp->d_name);
	}
    }
    closedir(dirp);

    if (pmDebugOptions.appl2) {
	fprintf(stderr, "process_refresh_pidlist: found %d PIDs:", npidlist);
	    if (npidlist > 0) {
		fprintf(stderr, " %d", pidlist[0]);
		if (npidlist > 1) {
		    fprintf(stderr, " %d", pidlist[1]);
		    if (npidlist == 3)
			fprintf(stderr, " %d", pidlist[2]);
		    else if (npidlist == 4)
			fprintf(stderr, " %d %d", pidlist[2], pidlist[3]);
		    else if (npidlist > 4)
			fprintf(stderr, " ... %d %d", pidlist[npidlist-2],
				pidlist[npidlist-1]);
		}
	}
	fprintf(stderr, "\n");
    }

    return npidlist;
}

static int
process_fetch(int numpmid, pmID pmidlist[], pmResult **resp, pmdaExt *pmda)
{
    /*
     * This is the wrapper over the pmdaFetch routine, to handle the problem
     * of varying instance domains.  All this does is check to see if the
     * config file has been updated, and update the instance domain if so,
     * and refresh the pid count for the processes.
     */
    process_config_file_check();
    process_refresh_pid_checks();
    return pmdaFetch(numpmid, pmidlist, resp, pmda);
}


/*
 * callback provided to pmdaFetch
 */
static int
process_fetchCallBack(pmdaMetric *mdesc, unsigned int inst, pmAtomValue *atom)
{
    if (pmID_cluster(mdesc->m_desc.pmid) != 0)
	return PM_ERR_PMID;
    if (pmID_item(mdesc->m_desc.pmid) != 0)
	return PM_ERR_PMID;

    /* We have the values stored, simply grab it and return it.  */
    atom->d = num_procs[inst];
    return 0;
}

/*
 * Initialise the agent (both daemon and DSO).
 */
void 
process_init(pmdaInterface *dp)
{
    if (isDSO) {
	int sep = pmPathSeparator();
	pmsprintf(mypath, sizeof(mypath), "%s%c" "process" "%c" "help",
		pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
	pmdaDSO(dp, PMDA_INTERFACE_2, "process DSO", mypath);
    }

    if (dp->status != 0)
	return;

    dp->version.two.fetch = process_fetch;

    /* Let's grab the info right away just to make sure it's there. */
    process_grab_config_info();

    pmdaSetFetchCallBack(dp, process_fetchCallBack);

    pmdaInit(dp, indomtab, sizeof(indomtab)/sizeof(indomtab[0]), 
	     metrictab, sizeof(metrictab)/sizeof(metrictab[0]));
}

pmLongOptions	longopts[] = {
    PMDA_OPTIONS_HEADER("Options"),
    PMOPT_DEBUG,
    PMDAOPT_DOMAIN,
    PMDAOPT_LOGFILE,
    PMOPT_HELP,
    PMDA_OPTIONS_END
};

pmdaOptions	opts = {
    .short_options = "D:d:l:?",
    .long_options = longopts,
};

/*
 * Set up the agent if running as a daemon.
 */
int
main(int argc, char **argv)
{
    int			sep = pmPathSeparator();
    pmdaInterface	desc;

    isDSO = 0;
    pmSetProgname(argv[0]);

    pmsprintf(mypath, sizeof(mypath), "%s%c" "process" "%c" "help",
	pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
    pmdaDaemon(&desc, PMDA_INTERFACE_2, pmGetProgname(), PROCESS,
		"process.log", mypath);

    pmdaGetOptions(argc, argv, &opts, &desc);
    if (opts.errors) {
    	pmdaUsageMessage(&opts);
	exit(1);
    }

    pmdaOpenLog(&desc);
    process_init(&desc);
    pmdaConnect(&desc);
    pmdaMain(&desc);
    exit(0);
}
