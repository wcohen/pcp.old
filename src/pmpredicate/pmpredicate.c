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
#include <stdlib.h>

#define MAX_METRICS 10

static char *predicate_name = NULL;
static int metric_count=0;
static char *metric_name[MAX_METRICS];
static int top;

pmLongOptions longopts[] = {
    PMAPI_GENERAL_OPTIONS,
    PMAPI_OPTIONS_HEADER("Reporting options"),
    { "pause", 0, 'P', 0, "pause between updates for archive replay" },
    { "filter", 1, 'f', "PREDICATE", "predicate to filter ony" },
    { "metric", 1, 'm', "METRIC", "metrics to collect" },
    { "rank", 1, 'r', "TOP", "limit results to <TOP> highest matches" },
    PMAPI_OPTIONS_END
};

pmOptions opts = {
    .flags = PM_OPTFLAG_STDOUT_TZ,
    .short_options = PMAPI_OPTIONS "Pf:m:r:",
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
static unsigned int	metric_pmid[MAX_METRICS];
static pmDesc		metric_desc[MAX_METRICS];

typedef struct {
    int inst;
    double predicate;
} hotproc_t;

static hotproc_t	hotproc[indom_maxnum];
static int		num_hotproc;

/* Used by qsort to sort from largest to smallest predicate */
static int compare_predicate(const void *a, const void *b)
{
    return (int) (((hotproc_t *)b)->predicate - ((hotproc_t *)a)->predicate);
}

/* Used for qsort to sort from smallest to largest instance */
static int compare_inst(const void *a, const void *b)
{
    return (int) (((hotproc_t *)a)->inst - ((hotproc_t *)b)->inst);
}

void cull()
{
    /* Cull to top matches */
    if (top>0 && top<num_hotproc) {
	/* Move values with highest predicate values to array start */
	qsort(hotproc, num_hotproc, sizeof(hotproc_t), compare_predicate);
	num_hotproc = top;
	/* Return culled values to inst order */
	qsort(hotproc, num_hotproc,sizeof(hotproc_t), compare_inst);
    }
}

void write_metadata()
{
    FILE *meta_fd = stdout;
    int i;

    meta_fd = fopen("metadata.json", "w+");
    if (meta_fd==NULL) goto fail;

    fprintf(meta_fd, "{\n\t\"prefix\": \"%s\",\n", "prefix");
    fprintf(meta_fd, "\t\"metrics\": [\n\t\t{\n");
    fprintf(meta_fd, "\t\t\t\"name\": \"%s\",\n", "metrics");
    fprintf(meta_fd, "\t\t\t\"pointer\": \"/%s\",\n", "hotprocdata");
    fprintf(meta_fd, "\t\t\t\"type\": \"array\",\n");
    fprintf(meta_fd, "\t\t\t\"description\": \"%s\",\n", "FIXME");
    fprintf(meta_fd, "\t\t\t\"index\": \"/%s\",\n", "inst");
    fprintf(meta_fd, "\t\t\t\"metrics\": [\n");

    for (i=0; i<metric_count; ++i) {
	fprintf(meta_fd, "\t\t\t{\n");
	fprintf(meta_fd, "\t\t\t\t\"name\": \"%s\",\n", metric_name[i]);
	fprintf(meta_fd, "\t\t\t\t\"pointer\": \"/%s\",\n", metric_name[i]);
	fprintf(meta_fd, "\t\t\t\t\"type\": \"%s\",\n", pmTypeStr(metric_desc[i].type));
	fprintf(meta_fd, "\t\t\t\t\"description\": \"%s\"\n", "FIXME");
	fprintf(meta_fd, "\t\t\t}");
	if (i<metric_count - 1) fprintf(meta_fd, ",");
	fprintf(meta_fd, "\n");
    }
    fprintf(meta_fd, "\t\t\t]\n");
    fprintf(meta_fd, "\t\t}\n\t]\n}\n");

    fclose(meta_fd);

    return;

 fail:
    printf("unable to open metadata.json file\n");
    exit(1);
    return;
}

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
	    fprintf(stderr, "%s: Failed %s ExtendFetchGroup: %s\n",
		    pmProgname, predicate_name, pmErrStr(sts));
	    exit(1);
	}
    }

    if ((sts = pmLookupName(metric_count, &(metric_name[0]), metric_pmid) < 0)) {
	    fprintf(stderr, "%s: Failed to find pmid for the metrics: %s\n",
		    pmProgname, pmErrStr(sts));
	    exit(1);
    }

    for (i=0; metric_name[i] && i<metric_count; ++i){
	if ((sts = pmLookupDesc(metric_pmid[i], &metric_desc[i]))) {
	    fprintf(stderr, "%s: Failed to find pmDesc for %s\n",
		    pmProgname, pmErrStr(sts));
	    exit(1);
	}
	if ((sts = pmExtendFetchGroup_indom(pmfg,
				metric_name[i], NULL,
				metric_inst[i], NULL, metric[i], metric_desc[i].type,
				NULL, indom_maxnum, &num_metric[i], NULL)) < 0) {
	    fprintf(stderr, "%s: Failed %s ExtendFetchGroup: %s\n",
		    pmProgname, metric_name[i], pmErrStr(sts));
	    exit(1);
	}
    }

    write_metadata();
}

static void
get_sample(void)
{
    int			sts;
    int			i, j;
    FILE *data_fd;

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
	    hotproc[num_hotproc].inst = predicate_inst[i];
	    hotproc[num_hotproc].predicate = predicate[i].d;
	    ++num_hotproc;
	}
    }
    cull();

    data_fd = fopen("data.json", "w+");
    if (data_fd==NULL) goto fail;

    /* Do predicate filtering on each metric. */
    if (predicate_name) {
	int p[MAX_METRICS];

	/* FIXME write following to data.json */
	fprintf(data_fd, "{\n\t\"%s\":[\n", "hotprocdata");

	for(i=0; i<MAX_METRICS;++i) p[i] = 0;
	for (j=0; j<num_hotproc; j++){
	    /* FIXME write out instance info */
	    fprintf(data_fd, "\t{\n\t\t\"inst\": %d", hotproc[j].inst);
	    for (i=0; metric_name[i] && i<metric_count; ++i){
		/* Scan for matching instance number, They could be in different positions */
		while (p[i]<num_metric[i] && metric_inst[i][p[i]]<hotproc[j].inst)
		    ++p[i];
		if (p[i]<num_metric[i] && metric_inst[i][p[i]]==hotproc[j].inst){
		    /* FIXME have a match, do whatever */
		    fprintf(data_fd, ",\n\t\t\"%s\": %s",
			    metric_name[i], pmAtomStr(&metric[i][p[i]], metric_desc[i].type));
		}
	    }
	    fprintf(data_fd, "\n\t}");
	    if (j < num_hotproc-1) fprintf(data_fd, ",");
	    fprintf(data_fd, "\n");
	}
	fprintf(data_fd, "\t]\n}\n");
    }

    fclose(data_fd);

    return;

 fail:
    printf("unable to open data.json file\n");
    exit(1);
    return;
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
	case 'r':
	    top = atoi(opts.optarg);
	    if (!(top>0)) {
		fprintf(stderr, "--top option needs a postive value\n");
		exit(1);
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
