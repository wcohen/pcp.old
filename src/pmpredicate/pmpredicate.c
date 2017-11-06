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
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <libgen.h>
#include <time.h>
#include <sys/stat.h>

#define MAX_METRICS 10

static int backup=0;
static int clean_default=0;
static char *directory = NULL;
static char *json_prefix = "prefix";
static char *metadata_json_name = "metadata.json";
static char *data_json_name = "data.json";
static char *data_json_name_tmp = "data.json.tmp";
static char *predicate_name = NULL;
static unsigned int metric_count = 0;
static char *metric_name[MAX_METRICS];
static char *metric_mangled_name[MAX_METRICS];
static int top;

pmLongOptions longopts[] = {
    PMAPI_OPTIONS_HEADER("General options"),
    PMOPT_HELP,
    PMOPT_INTERVAL,
    { "json_prefix", 1, 'j', "PREFIX", "JSON prefix for the filtered metrics (default \"prefix\")" },
    { "filter", 1, 'f', "PREDICATE", "predicate to filter ony" },
    { "rank", 1, 'r', "TOP", "limit results to <TOP> highest matches" },
    { "metric", 1, 'm', "METRIC", "metrics to collect" },
    { "directory", 1, 'd', "DIR", "Where to write metadata.json and data.json files" },
    { "username",	1, 'U', "USER",	"run the command using the named user account" },
    PMOPT_SAMPLES,
    PMAPI_OPTIONS_HEADER("Debugging options"),
    { "backup", 0, 'b', 0, "save backup copies of the data.json files for debugging purposes" },
    PMAPI_OPTIONS_END
};

pmOptions opts = {
    .flags = PM_OPTFLAG_STDOUT_TZ,
    .short_options = "?t:j:f:r:m:d:U:s:b",
    .long_options = longopts,
    .interval = {.tv_sec = 5, .tv_usec = 0}, /*Default: 5 second between samples */
    .samples = -1, /* Default: No limit on the number of samples */
};


static pmFG		pmfg;
enum { indom_maxnum = 1024 };
static int		predicate_inst[indom_maxnum];
static unsigned int	predicate_pmid;
static pmDesc		predicate_desc;
static char		*predicate_inst_name[indom_maxnum];
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
    char *inst_name;
} hotvalues_t;

static hotvalues_t	hotvalues[indom_maxnum];
static int		num_hotvalues;

/* Used by qsort to sort from largest to smallest predicate */
static int compare_predicate(const void *a, const void *b)
{
    return (int) (((hotvalues_t *)b)->predicate - ((hotvalues_t *)a)->predicate);
}

/* Used for qsort to sort from smallest to largest instance */
static int compare_inst(const void *a, const void *b)
{
    return (int) (((hotvalues_t *)a)->inst - ((hotvalues_t *)b)->inst);
}

void cull()
{
    /* Cull to top matches. */
    if (top>0 && top<num_hotvalues) {
	/* Move values with highest predicate values to array start. */
	qsort(hotvalues, num_hotvalues, sizeof(hotvalues_t), compare_predicate);
	num_hotvalues = top;
	/* Return culled values to instance order. */
	qsort(hotvalues, num_hotvalues,sizeof(hotvalues_t), compare_inst);
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
	return "double";
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
    fprintf(meta_fd, "\t\t\t\"pointer\": \"/%s\",\n", "hotvaluesdata");
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
    int errors = 0;

    /* Set up the predicate fetch if there is a predicate. */
    if (predicate_name) {
	if ((sts = pmLookupName(1, &predicate_name, &predicate_pmid) < 0)) {
	    fprintf(stderr, "%s: Failed to find pmid for the predicate: %s\n",
		    pmGetProgname(), pmErrStr(sts));
	    exit(1);
	}
	if ((sts = pmLookupDesc(predicate_pmid, &predicate_desc))) {
	    fprintf(stderr, "%s: Failed to find pmDesc for %s\n",
		    pmGetProgname(), pmErrStr(sts));
	    exit(1);
	}
	if ((sts = pmExtendFetchGroup_indom(pmfg,
					    predicate_name, NULL,
					    predicate_inst, predicate_inst_name,
					    predicate, PM_TYPE_DOUBLE,
					    NULL, indom_maxnum, &num_predicate, NULL)) < 0) {
	    fprintf(stderr, "%s: Failed %s ExtendFetchGroup: %s\n",
		    pmGetProgname(), predicate_name, pmErrStr(sts));
	    exit(1);
	}
    }

    if ((sts = pmLookupName(metric_count, &(metric_name[0]), metric_pmid) < 0)) {
	    fprintf(stderr, "%s: Failed to find pmid for the metrics: %s\n",
		    pmGetProgname(), pmErrStr(sts));
	    exit(1);
    }

    for (i=0; metric_name[i] && i<metric_count; ++i){
	if ((sts = pmLookupDesc(metric_pmid[i], &metric_desc[i]))) {
	    fprintf(stderr, "%s: Failed to find pmDesc for %s\n",
		    pmGetProgname(), pmErrStr(sts));
	    /* Some metrics are missing descriptions, so just warn about it. */
	}
	/* Need to have the same instance domain for predicate and metric. */
	if (predicate_name && predicate_desc.indom != metric_desc[i].indom) {
	    fprintf(stderr, "%s: predicate %s and metric %s have different instance domains\n",
		    pmGetProgname(), predicate_name, metric_name[i]);
	    errors++;
	}
	if ((sts = pmExtendFetchGroup_indom(pmfg,
				metric_name[i], "instant",
				metric_inst[i], NULL, metric[i], metric_desc[i].type,
				NULL, indom_maxnum, &num_metric[i], NULL)) < 0) {
	    fprintf(stderr, "%s: Failed %s ExtendFetchGroup: %s\n",
		    pmGetProgname(), metric_name[i], pmErrStr(sts));
	    errors++;
	}
	if ((sts = pmLookupText(metric_pmid[i], PM_TEXT_ONELINE, &metric_desc_text[i]))) {
	    fprintf(stderr, "%s: Failed to get Text description for %s\n",
		    pmGetProgname(), pmErrStr(sts));
	    /* Some metrics are missing descriptions, so just warn about it. */
	}
    }

    if (errors)
	exit(1);

    write_metadata();
}

static void
backup_file(void)
{
    time_t rawtime;
    struct tm *loctime;
    char *backup_name=malloc(strlen(data_json_name)+16);

    /* Create a new name for backup. */
    if(backup_name == NULL) {
	printf("unable to allocate space for backup data.json file name\n");
	exit(1);
	return;
    }
    time(&rawtime);
    loctime = localtime(&rawtime);
    sprintf(backup_name, "%s-%02d:%02d:%02d", data_json_name, loctime->tm_hour,
	    loctime->tm_min, loctime->tm_sec);
    link(data_json_name, backup_name);
    free(backup_name);
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
	fprintf(stderr, "%s: pmFetchGroup: %s\n", pmGetProgname(), pmErrStr(sts));
	exit(1);
    }

    /* Get a list of indoms that predicate is true for. */
    num_hotvalues = 0;
    for (i=0; i<num_predicate; ++i){
	if (predicate[i].d>0) {
	    hotvalues[num_hotvalues].inst = predicate_inst[i];
	    hotvalues[num_hotvalues].predicate = predicate[i].d;
	    hotvalues[num_hotvalues].inst_name = predicate_inst_name[i];
	    ++num_hotvalues;
	}
    }
    cull();

    data_fd = fopen(data_json_name_tmp, "w+");
    if (data_fd==NULL) goto fail;

    /* Do predicate filtering on each metric. */
    if (predicate_name) {
	int p[MAX_METRICS];

	/* Write hot values to data.json. */
	fprintf(data_fd, "{\n\t\"%s\": [\n", "hotvaluesdata");

	for(i=0; i<MAX_METRICS;++i) p[i] = 0;
	for (j=0; j<num_hotvalues; j++){
	    /* Write out data for the instance. */
	    fprintf(data_fd, "\t{\n\t\t\"inst\": \"%s\"", hotvalues[j].inst_name);
	    for (i=0; metric_name[i] && i<metric_count; ++i){
		/* Scan for matching instance number.
		   They could be in different positions. */
		while (p[i]<num_metric[i] && metric_inst[i][p[i]]<hotvalues[j].inst)
		    ++p[i];
		if (p[i]<num_metric[i] && metric_inst[i][p[i]]==hotvalues[j].inst){
		    /* The metric has a matching instance, write the data. */
		    fprintf(data_fd, ",\n\t\t\"%s\": %s",
			    metric_name[i], pmAtomStr(&metric[i][p[i]], metric_desc[i].type));
		}
	    }
	    fprintf(data_fd, "\n\t}");
	    if (j < num_hotvalues-1) fprintf(data_fd, ",");
	    fprintf(data_fd, "\n");
	}
	fprintf(data_fd, "\t]\n}\n");
    }

    fclose(data_fd);

    rename(data_json_name_tmp, data_json_name);

    if (backup) {
	backup_file();
    }

    return;

 fail:
    printf("unable to open data.json file\n");
    exit(1);
    return;
}

/* PCP does not allow '.' in metric names. Covert them to '_' */
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
    char separator[2];
    char *empty = "";

    result[0] = '\0';
    separator[0] = '\0';
    separator[1] = '\0';
    if (dir) {
	/* Ensure there is a path separator at end of dir. */
	if (rindex(dir, __pmPathSeparator()) != (dir + strlen(dir)-1)){
	    separator[0] = __pmPathSeparator();
	}
    }
    if (dir == NULL)
	dir = empty;
    if (file == NULL)
	file = empty;
    pmsprintf(result, MAXPATHLEN, "%s%s%s", dir, separator, file);
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

/* Avoid cluttering up default location with files no longer being updated.
   This clean up allows pmdajson to keep track of which metrics are actually
   available. */
static void clean_files()
{
    if (clean_default) {
	remove(metadata_json_name);
	remove(data_json_name);
	remove(data_json_name_tmp);
	remove(directory);
    }
}

static void sig_handler(int signo)
{
    if (signo == SIGINT)
	clean_files();
    exit(0);
}

static void setup_cleanup()
{
    if (signal(SIGINT, sig_handler) == SIG_ERR)
	fprintf(stderr, "unable to setup handling of SIGING\n");
}

int
main(int argc, char **argv)
{
    int			c;
    int			sts;
    int			forever;
    char		*source;

    setlinebuf(stdout);

    while ((c = pmGetOptions(argc, argv, &opts)) != EOF) {
	switch (c) {
	case 'b':
	    backup++;
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
	    } else {
		fprintf(stderr, "--directory option can only used once\n");
		opts.errors++;
	    }
	    break;
	case 'U':
	    __pmSetProcessIdentity(opts.optarg);
	    break;
	default:
	    opts.errors++;
	    break;
	}
    }

    /* By default put in $PCP_TMP_DIR/json/pid. */
    if (directory == NULL) {
	clean_default = 1;
	char *json_dir = dir_plus_file(pmGetConfig("PCP_TMP_DIR"), "json");
	char pid_str[32];
	snprintf(pid_str, 32, "%d", getpid());
	directory = dir_plus_file(json_dir, pid_str);
	free(json_dir);
    }
    mkdir_recurse(directory, 0755);
    metadata_json_name = dir_plus_file(directory, metadata_json_name);
    data_json_name = dir_plus_file(directory, data_json_name);
    data_json_name_tmp = dir_plus_file(directory, data_json_name_tmp);

    setup_cleanup();

    if (predicate_name == NULL) {
	pmprintf("%s: need a filtering predicate\n", pmGetProgname());
	opts.errors++;
    }

    if (metric_count == 0) {
	pmprintf("%s: need to select metrics\n", pmGetProgname());
	opts.errors++;
    }

    if (opts.errors || opts.optind < argc - 1) {
	pmUsageMessage(&opts);
	exit(1);
    }

    opts.context = PM_CONTEXT_HOST;
    source = "local:";

    sts = pmCreateFetchGroup(& pmfg, opts.context, source);
    if (sts < 0) {
	fprintf(stderr, "%s: Cannot connect to PMCD on host \"%s\": %s\n",
		pmGetProgname(), source, pmErrStr(sts));
	exit(1);
    }
    c = pmGetFetchGroupContext(pmfg);
    
    /* set sampling loop termination via the command line options */
    forever = (opts.samples == -1);

    init_sample();
    get_sample();

    while (forever || opts.samples-- > 0) {
	__pmtimevalSleep(opts.interval);
	get_sample();
    }
    exit(0);
}
