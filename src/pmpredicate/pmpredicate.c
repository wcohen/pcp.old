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
#include <libgen.h>
#include <sys/stat.h>

#define MAX_METRICS 10

#ifdef _WIN32
#define PATH_SEPARATOR   '\\'
#define PATH_SEPARATOR_STRING  "\\"
#else
#define PATH_SEPARATOR   '/'
#define PATH_SEPARATOR_STRING   "/"
#endif

static char *directory = NULL;
static char *json_prefix = "prefix";
static char *metadata_json_name = "metadata.json";
static char *data_json_name = "data.json";
static char *data_json_name_tmp = "data.json.tmp";
static char *predicate_name = NULL;
static int metric_count=0;
static char *metric_name[MAX_METRICS];
static char *metric_mangled_name[MAX_METRICS];
static int top;

pmLongOptions longopts[] = {
    PMAPI_GENERAL_OPTIONS,
    PMAPI_OPTIONS_HEADER("Reporting options"),
    { "pause", 0, 'P', 0, "pause between updates for archive replay" },
    { "json_prefix", 1, 'j', "PREFIX", "JSON prefix for the filtered metrics (default \"prefix\")" },
    { "filter", 1, 'f', "PREDICATE", "predicate to filter ony" },
    { "rank", 1, 'r', "TOP", "limit results to <TOP> highest matches" },
    { "metric", 1, 'm', "METRIC", "metrics to collect" },
    { "directory", 1, 'd', "DIR", "Where to write metadata.json and data.json files" },
    PMAPI_OPTIONS_END
};

pmOptions opts = {
    .flags = PM_OPTFLAG_STDOUT_TZ,
    .short_options = PMAPI_OPTIONS "Pj:f:r:m:d:",
    .long_options = longopts,
    .interval = {.tv_sec = 5, .tv_usec = 0}, /*Default: 5 second  between samples */
    .samples = -1, /* Default: No limit on the number of samples */
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
static char		*metric_desc_text[MAX_METRICS];

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

void cull()
{
    /* Cull to top matches */
    if (top>0 && top<num_hotproc) {
	/* Move values with highest predicate values to array start */
	qsort(hotproc, num_hotproc, sizeof(hotproc_t), compare_predicate);
	num_hotproc = top;
    }
}

const char *json_type(int type)
{
    switch(type){
    case PM_TYPE_32:
    case PM_TYPE_U32:
    case PM_TYPE_64:
    case PM_TYPE_U64:
	return "integer";
    case PM_TYPE_FLOAT:
    case PM_TYPE_DOUBLE:
	return "float";
    case PM_TYPE_STRING:
	return "string";
    case PM_TYPE_AGGREGATE:
    case PM_TYPE_AGGREGATE_STATIC:
    case PM_TYPE_EVENT:
    case PM_TYPE_HIGHRES_EVENT:
    case PM_TYPE_UNKNOWN:
    default:
	return "unknown";
    }
}

void write_metadata()
{
    FILE *meta_fd = stdout;
    int i;

    meta_fd = fopen(metadata_json_name, "w+");
    if (meta_fd==NULL) goto fail;

    fprintf(meta_fd, "{\n\t\"prefix\": \"%s\",\n", json_prefix);
    fprintf(meta_fd, "\t\"metrics\": [\n\t\t{\n");
    fprintf(meta_fd, "\t\t\t\"name\": \"%s\",\n", "metrics");
    fprintf(meta_fd, "\t\t\t\"pointer\": \"/%s\",\n", "hotprocdata");
    fprintf(meta_fd, "\t\t\t\"type\": \"array\",\n");
    fprintf(meta_fd, "\t\t\t\"description\": \"%s\",\n", "FIXME");
    fprintf(meta_fd, "\t\t\t\"index\": \"/%s\",\n", "inst");
    fprintf(meta_fd, "\t\t\t\"metrics\": [\n");

    for (i=0; i<metric_count; ++i) {
	fprintf(meta_fd, "\t\t\t{\n");
	fprintf(meta_fd, "\t\t\t\t\"name\": \"%s\",\n", metric_mangled_name[i]);
	fprintf(meta_fd, "\t\t\t\t\"pointer\": \"/%s\",\n", metric_name[i]);
	if (strlen(pmSemStr(metric_desc[i].sem)))
	    fprintf(meta_fd, "\t\t\t\t\"semantics\": \"%s\",\n", pmSemStr(metric_desc[i].sem));
	if (strlen(pmUnitsStr(&metric_desc[i].units)))
	    fprintf(meta_fd, "\t\t\t\t\"units\": \"%s\",\n", pmUnitsStr(&metric_desc[i].units));
	if (metric_desc_text[i])
	    fprintf(meta_fd, "\t\t\t\t\"description\": \"%s\",\n", metric_desc_text[i]);
	fprintf(meta_fd, "\t\t\t\t\"type\": \"%s\"\n", json_type(metric_desc[i].type));
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
				metric_name[i], "instant",
				metric_inst[i], NULL, metric[i], metric_desc[i].type,
				NULL, indom_maxnum, &num_metric[i], NULL)) < 0) {
	    fprintf(stderr, "%s: Failed %s ExtendFetchGroup: %s\n",
		    pmProgname, metric_name[i], pmErrStr(sts));
	    exit(1);
	}
	if ((sts = pmLookupText(metric_pmid[i], PM_TEXT_ONELINE, &metric_desc_text[i]))) {
	    fprintf(stderr, "%s: Failed to get Text description for %s\n",
		    pmProgname, pmErrStr(sts));
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

    data_fd = fopen(data_json_name, "w+");
    if (data_fd==NULL) goto fail;

    /* Do predicate filtering on each metric. */
    if (predicate_name) {
	int p[MAX_METRICS];

	/* FIXME write following to data.json */
	fprintf(data_fd, "{\n\t\"%s\": [\n", "hotprocdata");

	for(i=0; i<MAX_METRICS;++i) p[i] = 0;
	for (j=0; j<num_hotproc; j++){
	    /* FIXME write out instance info */
	    fprintf(data_fd, "\t{\n\t\t\"inst\": \"%d\"", hotproc[j].inst);
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

    rename(data_json_name_tmp, data_json_name);

    return;

 fail:
    printf("unable to open data.json file\n");
    exit(1);
    return;
}

/* PCP does not allow '.' in metric names. Covert them to '.' */
static char *mangle(char *name)
{
    char *s = strdup(name);
    char *p = s;

    while(*p) {
	if (*p == '.') *p = '_';
	++p;
    }
    return s;
}

static char *dir_plus_file(char *dir, char *file)
{
    char result[MAXPATHLEN];

    result[0] = '\0';
    if (dir) {
	strncat(result, dir, MAXPATHLEN-1);
	/* ensure there is a path separator at end */
	if (rindex(dir, PATH_SEPARATOR) != (dir + strlen(dir)-1)){
	    strncat(result, PATH_SEPARATOR_STRING, MAXPATHLEN-1);
	}
    }
    if (file) {
	strncat(result, file, MAXPATHLEN-1);
    }
    return strdup(result);
}

static int mkdir_recurse(char *path, mode_t mode)
{
    char *path_dup = strdup(path);
    char *new_dir = dirname(path_dup);
    int retval = 0;

    if (strlen(new_dir) > 1) {
	retval = mkdir_recurse(new_dir, mode);
    }
    if (retval>=0 || errno==EEXIST){
        retval = mkdir(path, mode);
    }
    free(path_dup);
    return retval;
}

int
main(int argc, char **argv)
{
    int			c;
    int			sts;
    int			forever;
    int			pauseFlag = 0;
    char		*source;

    setlinebuf(stdout);

    while ((c = pmGetOptions(argc, argv, &opts)) != EOF) {
	switch (c) {
	case 'P':
	    pauseFlag++;
	    break;
	case 'j':
	    json_prefix = opts.optarg;
	    break;
	case 'f':
	    predicate_name = opts.optarg;
	    break;
	case 'm':
	    if (metric_count<MAX_METRICS) {
		metric_name[metric_count] = opts.optarg;
		metric_mangled_name[metric_count] = mangle(opts.optarg);
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
	case 'd':
	    if (directory == NULL) {
		directory = opts.optarg;
		mkdir_recurse(directory, 0755);
		metadata_json_name = dir_plus_file(directory, metadata_json_name);
		data_json_name = dir_plus_file(directory, data_json_name);
	    } else {
		fprintf(stderr, "--directory option can only used once\n");
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

    /* set sampling loop termination via the command line options */
    forever = (opts.samples == -1);

    init_sample();
    get_sample();

    while (forever || opts.samples-- > 0) {
	if (opts.context != PM_CONTEXT_ARCHIVE || pauseFlag)
	    __pmtimevalSleep(opts.interval);
	get_sample();
    }
    exit(0);
}
