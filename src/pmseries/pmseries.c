/*
 * Copyright (c) 2017 Red Hat.
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

#include "series.h"
#include "pmapi.h"

static int
series_split(const char *string, pmSeriesID **series)
{
    pmSeriesID		*result, *rp;
    char		*sp, *split, *saved;
    int			nseries = 0;

    if ((split = saved = strdup(string)) == NULL)
	return -ENOMEM;

    while ((sp = strsep(&split, ", ")) != NULL) {
	if (*sp) {
	    if (strlen(sp) != PMSIDSZ) {
		free(split);
		return -EINVAL;
	    }
	    nseries++;
	}
    }
    strcpy(split = saved, string);

    if (nseries == 0) {
	free(split);
	return 0;
    }
    result = rp = (pmSeriesID *)calloc(nseries, sizeof(pmSeriesID));
    if (result == NULL) {
	free(split);
	return -ENOMEM;
    }
    while ((sp = strsep(&split, ", ")) != NULL) {
	if (*sp) {
	    memcpy(rp->name, sp, PMSIDSZ);
	    rp->name[PMSIDSZ] = '\0';
	    rp++;
	}
    }
    free(split);
    *series = result;
    return nseries;
}

static void
series_free(int nseries, pmSeriesID *series)
{
    if (nseries > 0)
	free(series);
}

static void
on_series_info(pmseries_level level, const char *message, void *arg)
{
    const char	*prefix;

    (void)arg;
    switch (level) {
    case PMSERIES_INFO:
	prefix = "INFO";
	break;
    case PMSERIES_WARNING:
	prefix = "WARN";
	break;
    case PMSERIES_ERROR:
	prefix = "ERR";
	break;
    case PMSERIES_REQUEST:
	prefix = "REQ";
	break;
    case PMSERIES_RESPONSE:
	prefix = "RES";
	break;
    case PMSERIES_CORRUPT:
	prefix = "BAD";
	break;
    default:
	prefix = "?";
	break;
    }
    fprintf(stderr, "[%s] %s\n", prefix, message);
}

/*
 *  pmSeriesLoad call, callbacks and data structure
 */

typedef struct series_load_data {
    int			status;		/* command exit status */
    pmSeriesID		series;		/* current time series */
    unsigned int        nseries;	/* count of series seen */
} series_load_data;

static int
series_load(pmSeriesSettings *settings, const char *query, int flags)
{
    series_load_data	data = {0};

    pmSeriesLoad(settings, query, flags, (void *)&data);
    return data.status;
}

static int
series_load_meta(pmSeriesSettings *settings, const char *query)
{
    return series_load(settings, query, PMSERIES_METADATA);
}

static int
series_load_all(pmSeriesSettings *settings, const char *query)
{
    return series_load(settings, query, 0);
}

/*
 *  pmSeriesQuery call, callbacks and data structure
 */

typedef struct series_query_data {
    int			status;		/* command exit status */
    pmSeriesID		series;		/* current time series */
    unsigned int        nseries;	/* count of series seen */
    unsigned int        nvalues;	/* values observed for current series */
} series_query_data;

static int
on_series_match(pmSeriesID *sid, void *arg)
{
    series_query_data	*dp = (series_query_data *)arg;

    printf("%s\n", sid->name);
    dp->series = *sid;
    dp->nseries++;
    return 0;
}

static int
on_series_value(pmSeriesID *sid, const char *stamp, const char *value, void *arg)
{
    series_query_data	*dp = (series_query_data *)arg;

    if (dp->nvalues == 0 || memcmp(&dp->series, sid, PMSIDSZ) != 0) {
	printf("\n%s\n", sid->name);
	dp->series = *sid;
	dp->nvalues = 0;
    }

    printf("    [%s] %s\n", stamp, value);
    dp->nvalues++;
    return 0;
}

static int
series_query(pmSeriesSettings *settings, const char *query)
{
    series_query_data		data = {0};

    pmSeriesQuery(settings, query, 0, (void *)&data);
    return data.status;
}

static int
series_query_meta(pmSeriesSettings *settings, const char *query)
{
    series_query_data		data = {0};

    pmSeriesQuery(settings, query, PMSERIES_METADATA, (void *)&data);
    return data.status;
}

/*
 *  pmSeriesDesc call, callback and data structure
 */

typedef struct series_desc_data {
    int			status;		/* command exit status */
    pmSeriesID		series;		/* current time series */
    unsigned int        nseries;	/* count of series seen */
} series_desc_data;

static char *
pmid2hex(const char *pmid, char *buf, size_t buflen)
{
    unsigned int	domain, cluster, item;
    char		*c, *i;

    strncpy(buf, pmid, buflen);
    buf[buflen-1] = '\0';
    if ((c = index(buf, '.')) == NULL ||
	((i = index(c+1, '.')) == NULL)) {
	pmsprintf(buf, buflen, "0xffffffff");
    } else {
	*c = *i = '\0';
	domain = atoi(buf);
	cluster = atoi(c+1);
	item = atoi(i+1);
	pmsprintf(buf, buflen, "0x%x", pmID_build(domain, cluster, item));
    }
    return buf;
}

static char *
indom2hex(const char *indom, char *buf, size_t buflen)
{
    unsigned int	domain, serial;
    char		*p;

    strncpy(buf, indom, buflen);
    buf[buflen-1] = '\0';
    if ((p = index(buf, '.')) == NULL) {
	pmsprintf(buf, buflen, "0xffffffff");
    } else {
	*p = '\0';
	domain = atoi(buf);
	serial = atoi(p+1);
	pmsprintf(buf, buflen, "0x%x", pmInDom_build(domain, serial));
    }
    return buf;
}

static char *
typestr(const char *type, char *buf, size_t buflen)
{
    if (strcmp(type, "s32") == 0)
	pmsprintf(buf, buflen, "32-bit int");
    else if (strcmp(type, "u32") == 0)
	pmsprintf(buf, buflen, "32-bit int");
    else if (strcmp(type, "s64") == 0)
	pmsprintf(buf, buflen, "64-bit int");
    else if (strcmp(type, "u64") == 0)
        pmsprintf(buf, buflen, "64-bit unsigned int");
    else
	pmsprintf(buf, buflen, "%s", type);
    return buf;
}

static int
on_series_desc(pmSeriesID *series, const char *pmid, const char *indom,
	const char *semantics, const char *type, const char *units, void *arg)
{
    series_desc_data	*dp = (series_desc_data *)arg;
    char		inbuf[64], phexbuf[32], ihexbuf[32], typebuf[32];

    typestr(type, typebuf, sizeof(typebuf));
    pmid2hex(pmid, phexbuf, sizeof(phexbuf));
    indom2hex(indom, ihexbuf, sizeof(ihexbuf));

    pmsprintf(inbuf, sizeof(inbuf), "%s %s",
		    indom[0] == '\0' ? "PM_INDOM_NULL" : indom, ihexbuf);

    if (dp->series.name[0] == '\0' || memcmp(&dp->series, series, PMSIDSZ))
	printf("\n%s\n", series->name);
    printf("    PMID: %s %s\n"
	   "    Data Type: %s  InDom: %s\n"
	   "    Semantics: %s  Units: %s\n",
	    pmid, phexbuf, typebuf, inbuf, semantics, units);

    dp->series = *series;
    dp->nseries++;
    return 0;
}

static int
series_descriptor(pmSeriesSettings *settings, const char *query)
{
    int			nseries, sts;
    pmSeriesID		*series = NULL;
    series_desc_data	data;

    if ((nseries = sts = series_split(query, &series)) < 0) {
	fprintf(stderr, "%s: no series identifiers in string '%s': %s\n",
		pmGetProgname(), query, pmErrStr(sts));
	return 2;
    }
    pmSeriesDesc(settings, nseries, series, (void *)&data);
    series_free(nseries, series);
    return data.status;
}

/*
 *  pmSeriesInstance call, callback and data structure
 */

typedef struct series_inst_data {
    int			status;		/* command exit status */
    pmSeriesID		series;		/* current time series */
    unsigned int        nseries;	/* count of series seen */
} series_inst_data;

static int
on_series_instance(pmSeriesID *series, int inst, const char *name, void *arg)
{
    series_inst_data	*ip = (series_inst_data *)arg;

    if (inst == PM_IN_NULL)
	return 0;

    if (ip->series.name[0] == '\0')
	printf("\n%s\n", series->name);
    printf("    Instance: [%d or \"%s\"]\n", inst, name);

    ip->series = *series;
    ip->nseries++;
    return 0;
}

static int
series_instance(pmSeriesSettings *settings, const char *query)
{
    int			nseries, sts;
    pmSeriesID		*series = NULL;
    series_inst_data	data = {0};

    if ((nseries = sts = series_split(query, &series)) < 0) {
	fprintf(stderr, "%s: cannot find series identifiers in '%s': %s\n",
		pmGetProgname(), query, pmErrStr(sts));
	return 1;
    }
    pmSeriesInstance(settings, nseries, series, (void *)&data);
    series_free(nseries, series);
    return data.status;
}

/*
 *  pmSeriesLabel call, callback and data structure
 */

typedef struct series_labelset {
    int			status;		/* command exit status */
    pmSeriesID		series;		/* current time series */
    unsigned int        nseries;	/* count of series seen */
} series_labelset;

static int
on_series_labels(pmSeriesID *series, const char *label, void *arg)
{
    series_labelset	*lp = (series_labelset *)arg;

    if (lp->series.name[0] == '\0')
	printf("\n%s\n", series->name);
    printf("    Labels: %s\n", label);

    lp->series = *series;
    lp->nseries++;
    return 0;
}

static int
series_labels(pmSeriesSettings *settings, const char *query)
{
    int			nseries, sts;
    pmSeriesID		*series = NULL;
    series_labelset	data = {0};

    if ((nseries = sts = series_split(query, &series)) < 0) {
	fprintf(stderr, "%s: cannot find series identifiers in '%s': %s\n",
		pmGetProgname(), query, pmErrStr(sts));
	return 2;
    }
    pmSeriesLabel(settings, nseries, series, (void *)&data);
    series_free(nseries, series);
    return data.status;
}

/*
 *  pmSeriesMetric call, callback and data structure
 */

typedef struct series_metrics {
    int			status;		/* command exit status */
    pmSeriesID		series;		/* current time series */
    unsigned int        nseries;	/* count of series seen */
    unsigned int        nnames;		/* count of metric names */
} series_metrics;

static int
on_series_metric(pmSeriesID *series, const char *name, void *arg)
{
    series_metrics	*mp = (series_metrics *)arg;

    if (mp->nnames && memcmp(&mp->series, series, PMSIDSZ) == 0) {
	printf(", %s", name);
    } else {
	if (mp->series.name[0] == '\0' || memcmp(&mp->series, series, PMSIDSZ))
	    printf("\n%s\n", series->name);
	printf("    Metrics: %s\n", name);
	mp->series = *series;
	mp->nseries++;
    }
    mp->nnames++;
    return 0;
}

static int
series_metric(pmSeriesSettings *settings, const char *query)
{
    int			nseries, sts;
    pmSeriesID		*series = NULL;
    series_metrics	data = {0};

    if ((nseries = sts = series_split(query, &series)) < 0) {
	fprintf(stderr, "%s: cannot find series identifiers in '%s': %s\n",
		pmGetProgname(), query, pmErrStr(sts));
	return 2;
    }
    pmSeriesMetric(settings, nseries, series, (void *)&data);
    series_free(nseries, series);
    return data.status;
}

void
on_series_done(int sts, void *arg)
{
    int			*exitsts = (int *)arg;

    if (sts < 0)
	fprintf(stderr, "%s: %s\n", pmGetProgname(), pmErrStr(sts));
    *exitsts = (sts < 0) ? 1 : 0;
}

static int
series_meta_all(pmSeriesSettings *settings, const char *query)
{
    int			nseries, sts, i;
    pmSeriesID		*series = NULL;

    if ((nseries = sts = series_split(query, &series)) < 0) {
	fprintf(stderr, "%s: cannot find series identifiers in '%s': %s\n",
		pmGetProgname(), query, pmErrStr(sts));
	return 1;
    }
    for (i = sts = 0; i < nseries; i++) {
	series_desc_data	desc_data = {.series = series[i]};
	series_inst_data	inst_data = {.series = series[i]};
	series_metrics		metrics_data = {.series = series[i]};
	series_labelset		labelset_data = {.series = series[i]};

	printf("\n%s\n", series[i].name);
	pmSeriesMetric(settings, 1, &series[i], (void *)&metrics_data);
	pmSeriesLabel(settings, 1, &series[i], (void *)&labelset_data);
	pmSeriesInstance(settings, 1, &series[i], (void *)&inst_data);
	pmSeriesDesc(settings, 1, &series[i], (void *)&desc_data);
	sts |= desc_data.status | inst_data.status |
	       metrics_data.status | labelset_data.status;
    }
    series_free(nseries, series);
    if (sts)
	return 1;
    return 0;
}

static int
pmseries_overrides(int opt, pmOptions *opts)
{
    return (opt == 'a' || opt == 'L');
}

static pmSeriesSettings settings = {
    .on_match = on_series_match,
    .on_value = on_series_value,
    .on_desc = on_series_desc,
    .on_instance = on_series_instance,
    .on_metric = on_series_metric,
    .on_labels = on_series_labels,
    .on_info = on_series_info,
    .on_done = on_series_done,
};

static pmLongOptions longopts[] = {
    PMAPI_OPTIONS_HEADER("Options"),
    PMOPT_DEBUG,
    { "all", 0, 'a', 0, "report all metadata (-dilm) for time series" },
    { "desc", 0, 'd', 0, "metric descriptor for time series" },
    { "instance", 0, 'i', 0, "instance identifiers for time series" },
    { "labels", 0, 'l', 0, "list all labels for time series" },
    { "load", 0, 'L', 0, "load time series values and metadata" },
    { "loadmeta", 0, 'M', 0, "load time series metadata only" },
    { "metrics", 0, 'm', 0, "metric names for time series" },
    { "query", 0, 'q', 0, "perform a time series query" },
    PMOPT_VERSION,
    PMOPT_HELP,
    PMAPI_OPTIONS_END
};

static pmOptions opts = {
    .flags = PM_OPTFLAG_BOUNDARIES,
    .short_options = "adD:ilLmMqV?",
    .long_options = longopts,
    .short_usage = "[query ... | series ...]",
    .override = pmseries_overrides,
};

typedef int (*series_command)(pmSeriesSettings *, const char *);

int
main(int argc, char *argv[])
{
    int			c, sts, len;
    unsigned char	query[BUFSIZ] = {0};
    series_command	command = series_query;

    while ((c = pmGetOptions(argc, argv, &opts)) != EOF) {
	switch (c) {

	case 'a':	/* command line contains series identifiers */
	    command = series_meta_all;
	    break;

	case 'd':	/* command line contains series identifiers */
	    command = series_descriptor;
	    break;

	case 'i':	/* command line contains series identifiers */
	    command = series_instance;
	    break;

	case 'l':	/* command line contains series identifiers */
	    command = series_labels;
	    break;

	case 'L':	/* command line contains load string */
	    command = series_load_all;
	    break;

	case 'm':	/* command line contains series identifiers */
	    command = series_metric;
	    break;

	case 'M':	/* command line contains load string */
	    command = series_load_meta;
	    break;

	case 'q':	/* command line contains query string */
	    command = series_query;
	    break;

	case 'Q':	/* command line contains query string */
	    command = series_query_meta;
	    break;

	default:
	    opts.errors++;
	    break;
	}
    }

    if (!command)
	opts.errors++;

    if (opts.errors || (opts.flags & PM_OPTFLAG_EXIT)) {
	sts = !(opts.flags & PM_OPTFLAG_EXIT);
	pmUsageMessage(&opts);
	exit(sts);
    }

    for (c = opts.optind, len = 0; c < argc; c++) {
	char *p = (char *)query + len;
	len += pmsprintf(p, sizeof(query) - len - 2, "%s ", argv[c]);
    }
    if (len)	/* cull the final space */
	query[len - 1] = '\0';

    return command(&settings, (const char *)query);
}
