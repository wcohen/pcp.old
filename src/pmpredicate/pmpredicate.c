/*
 * pmpredicate - sample, simpler PMAPI/fetchgroup client that filters instances
 *
 * Copyright (c) 2013-2015,2017 Red Hat.
 * Copyright (c) 1995-2002 Silicon Graphics, Inc.  All Rights Reserved.
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
#include "impl.h"
#include <assert.h>

#define MAX_METRICS 10

static char *predicate_name = NULL;
static int metric_count=0;
static char *metric_name[MAX_METRICS];

pmLongOptions longopts[] = {
    PMAPI_GENERAL_OPTIONS,
    PMAPI_OPTIONS_HEADER("Reporting options"),
    { "pause", 0, 'P', 0, "pause between updates for archive replay" },
    { "filter", 1, 'f', "PREDICATE", "predicate to filter ony" },
    { "metric", 1, 'm', "METRIC", "metrics to collect" },
    PMAPI_OPTIONS_END
};

pmOptions opts = {
    .flags = PM_OPTFLAG_STDOUT_TZ,
    .short_options = PMAPI_OPTIONS "Pf:m:",
    .long_options = longopts,
};


static pmFG		pmfg;
enum { indom_maxnum = 1024 };
static int		predicate_inst[indom_maxnum];
static pmAtomValue 	predicate[indom_maxnum];
static unsigned		num_predicate;
static int		metric_inst[MAX_METRICS][indom_maxnum];
static pmAtomValue	metric[MAX_METRICS][indom_maxnum];
static unsigned		num_metric[MAX_METRICS];

static int		hotproc_inst[indom_maxnum];
static int		num_hotproc;

static void
init_sample(void)
{
    int			sts;
    int i;

    /* set up predicate fetch if there is a predicate */
    if (predicate_name) {
	/* FIXME be more flexible on the units/conversions */
	if ((sts = pmExtendFetchGroup_indom(pmfg,
					    predicate_name, NULL,
					    predicate_inst, NULL, predicate, PM_TYPE_DOUBLE,
					    NULL, indom_maxnum, &num_predicate, NULL)) < 0) {
	    fprintf(stderr, "%s: Failed %s "
		    "ExtendFetchGroup: %s\n",
		    pmProgname, predicate_name, pmErrStr(sts));
	    exit(1);
	}
    }

    for (i=0; metric_name[i] && i<MAX_METRICS; ++i){
	/* FIXME be more flexible on the units/conversions */
	if ((sts = pmExtendFetchGroup_indom(pmfg,
				metric_name[i], NULL,
				metric_inst[i], NULL, metric[i], PM_TYPE_DOUBLE,
				NULL, indom_maxnum, &num_metric[i], NULL)) < 0) {
	    fprintf(stderr, "%s: Failed kernel.percpu.cpu.sys "
				"ExtendFetchGroup: %s\n",
		    pmProgname, pmErrStr(sts));
	    exit(1);
	}
	/* FIXME check to make sure metric same number of indoms as predicate */
    }
}

static void
get_sample(void)
{
    int			sts;
    int			i, j, k;

    /*
     * Fetch the current metrics; fill many info.* fields.  Since we
     * passed NULLs to most fetchgroup status int*'s, we'll get
     * PM_TYPE_DOUBLE fetch/conversion errors represented by NaN's.
     */
    sts = pmFetchGroup(pmfg);
    if (sts < 0) {
	fprintf(stderr, "%s: pmFetchGroup: %s\n", pmProgname, pmErrStr(sts));
	exit(1);
    }

    /* Get a list of indoms that predicate true on */
    num_hotproc = 0;
    for (i=0; i<num_predicate; ++i){
	if (predicate[i].d>0) {
	    hotproc_inst[num_hotproc] = predicate_inst[i];
	    ++num_hotproc;
	}
    }

    /* FIXME The following is only for diagnostic purposes. */
    printf("\n\npredicate: %s ", predicate_name);
    for(i=0; i<num_predicate; ++i) {
	printf("%f(%d) ", predicate[i].d, predicate_inst[i]);
    }
    printf("\n");

    /* Do predicate filtering on each metric. */
    if (predicate_name) {
	for (i=0; metric_name[i] && i<MAX_METRICS; ++i){
	    printf("\nmetric[%2d]: %s ", i, metric_name[i]);
	    k = 0;
	    for (j=0; k<num_metric[i] && j<num_hotproc ; ++j){
		/* Scan for matching instance number, They could be in different positions */
		while (k<num_metric[i] && metric_inst[i][k]<hotproc_inst[j])
		    ++k;
		if (k<num_metric[i] && metric_inst[i][k]==hotproc_inst[j]){
		    /* FIXME have a match, do whatever */
		    printf("%f(%d) ", metric[i][k].d, metric_inst[i][k]);
		}
	    }
	    printf("\n");
	}
    }

}

int
main(int argc, char **argv)
{
    int			c;
    int			sts;
    int			samples;
    int			pauseFlag = 0;
    char		*source;

    setlinebuf(stdout);

    while ((c = pmGetOptions(argc, argv, &opts)) != EOF) {
	switch (c) {
	case 'P':
	    pauseFlag++;
	    break;
	case 'f':
	    predicate_name = opts.optarg;
	    break;
	case 'm':
	    if (metric_count<MAX_METRICS) {
		metric_name[metric_count] = opts.optarg;
		++metric_count;
	    } else {
		opts.errors++;
	    }
	    break;
	default:
	    opts.errors++;
	    break;
	}
    }

    if (pauseFlag && opts.context != PM_CONTEXT_ARCHIVE) {
	pmprintf("%s: pause can only be used with archives\n", pmProgname);
	opts.errors++;
    }

    if (opts.errors || opts.optind < argc - 1) {
	pmUsageMessage(&opts);
	exit(1);
    }

    if (opts.context == PM_CONTEXT_ARCHIVE) {
	source = opts.archives[0];
    } else if (opts.context == PM_CONTEXT_HOST) {
	source = opts.hosts[0];
    } else {
	opts.context = PM_CONTEXT_HOST;
	source = "local:";
    }

    sts = pmCreateFetchGroup(& pmfg, opts.context, source);
    if (sts < 0) {
	if (opts.context == PM_CONTEXT_HOST)
	    fprintf(stderr, "%s: Cannot connect to PMCD on host \"%s\": %s\n",
		    pmProgname, source, pmErrStr(sts));
	else
	    fprintf(stderr, "%s: Cannot open archive \"%s\": %s\n",
		    pmProgname, source, pmErrStr(sts));
	exit(1);
    }
    c = pmGetFetchGroupContext(pmfg);
    
    /* complete TZ and time window option (origin) setup */
    if (pmGetContextOptions(c, &opts)) {
	pmflush();
	exit(1);
    }

    if ((opts.context == PM_CONTEXT_ARCHIVE) &&
	(opts.start.tv_sec != 0 || opts.start.tv_usec != 0)) {
	if ((sts = pmSetMode(PM_MODE_FORW, &opts.start, 0)) < 0) {
	    fprintf(stderr, "%s: pmSetMode failed: %s\n",
		    pmProgname, pmErrStr(sts));
	    exit(1);
	}
    }

    init_sample();

    /* set a default sampling interval if none has been requested */
    if (opts.interval.tv_sec == 0 && opts.interval.tv_usec == 0)
	opts.interval.tv_sec = 5;

    /* set sampling loop termination via the command line options */
    samples = opts.samples ? opts.samples : -1;

    get_sample();

    while (samples == -1 || samples-- > 0) {
	if (opts.context != PM_CONTEXT_ARCHIVE || pauseFlag)
	    __pmtimevalSleep(opts.interval);
	get_sample();
    }
    exit(0);
}
