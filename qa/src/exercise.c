/*
 * Copyright (c) 1995-2002 Silicon Graphics, Inc.  All Rights Reserved.
 */

/*
 * General exerciser, checks (for licensed/unlicensed PMCDs)
 *	- PMCD availability
 *	- pmDesc availability
 *	- indom availiability
 *	- metric value availability
 *	- memory leaks (when -i used to make iterations > 1)
 */

#include <ctype.h>
#include <string.h>
#include <pcp/pmapi.h>
#include <pcp/impl.h>

extern int	errno;

static int	_metrics;
static int	_indom;
static int	_insitu;
static int	_ptr;

static void
dometric(const char *name)
{
    int		n;
    pmID	pmidlist[] = { PM_ID_NULL };
    pmDesc	desc;
    int		*instlist = NULL;
    char	**instname = NULL;
    pmResult	*result;

    _metrics++;

    /* cast const away as pmLookupName will not modify this string */
    if ((n = pmLookupName(1, (char **)&name, pmidlist)) < 0) {
	printf("%s: pmLookupName: %s\n", name, pmErrStr(n));
	return;
    }
    if ((n = pmLookupDesc(pmidlist[0], &desc)) < 0) {
	printf("%s: pmLookupDesc: %s\n", name, pmErrStr(n));
	return;
    }
    if (desc.indom != PM_INDOM_NULL) {
	_indom++;
	if ((n = pmGetInDom(desc.indom, &instlist, &instname)) < 0) {
	    printf("%s: pmGetInDom: %s\n", name, pmErrStr(n));
	    return;
	}
	if (instlist)
	    free(instlist);
	if (instname)
	    free(instname);
    }
    if ((n = pmFetch(1, pmidlist, &result)) < 0) {
	printf("%s: pmFetch: %s\n", name, pmErrStr(n));
	return;
    }
    if (result->numpmid != 1) {
	printf("%s: pmFetch: numpmid=%d, not 1\n", name, result->numpmid);
    }
    else {
	if (result->vset[0]->numval == 0)
	    printf("%s: pmFetch: no value available\n", name);
	else if (result->vset[0]->numval < 0)
	    printf("%s: pmFetch: %s\n", name, pmErrStr(result->vset[0]->numval));
	else {
	    if (result->vset[0]->valfmt == PM_VAL_INSITU) {
		_insitu++;
		if (pmDebugOptions.appl0)
		    printf("%s: insitu type=%s\n", name, pmTypeStr(desc.type));
	    }
	    else {
		_ptr++;
		if (pmDebugOptions.appl0)
		    printf("%s: ptr size=%d valtype=%d descrtype=%s\n",
			    name,
			    result->vset[0]->vlist[0].value.pval->vlen,
			    result->vset[0]->vlist[0].value.pval->vtype,
			    pmTypeStr(desc.type));
	    }
	}
    }
    pmFreeResult(result);
}

int
main(argc, argv)
int argc;
char *argv[];
{
    int		c;
    int		sts;
    int		i;
    int		errflag = 0;
    char	*host = "localhost";
    char	*namespace = PM_NS_DEFAULT;
    char	*endnum;
    int		iter = 1;
    unsigned long datasize;
    static char	*usage = "[-D debugspec] [-h hostname] [-i iterations] [-n namespace] [-l licenseflag ] [name ...]";

    __pmProcessDataSize(NULL);
    pmSetProgname(pmGetProgname());

    while ((c = getopt(argc, argv, "D:h:i:l:n:")) != EOF) {
	switch (c) {

	case 'D':	/* debug options */
	    sts = pmSetDebug(optarg);
	    if (sts < 0) {
		fprintf(stderr, "%s: unrecognized debug options specification (%s)\n",
		    pmGetProgname(), optarg);
		errflag++;
	    }
	    break;

	case 'h':	/* hostname for PMCD to contact */
	    host = optarg;
	    break;

	case 'i':	/* iteration count */
	    iter = (int)strtol(optarg, &endnum, 10);
	    if (*endnum != '\0') {
		fprintf(stderr, "%s: -i requires numeric argument\n", pmGetProgname());
		errflag++;
	    }
	    break;

	case 'n':	/* alternative name space file */
	    namespace = optarg;
	    break;

	case '?':
	default:
	    errflag++;
	    break;
	}
    }

    if (errflag) {
	fprintf(stderr, "Usage: %s %s\n", pmGetProgname(), usage);
	exit(1);
    }

    sts = pmNewContext(PM_CONTEXT_HOST, host);

    if (sts < 0) {
	printf("%s: Cannot connect to PMCD on host \"%s\": %s\n", pmGetProgname(), host, pmErrStr(sts));
	exit(1);
    }

    if (namespace != PM_NS_DEFAULT) {
	/*
	 * only explicitly load namespace if -n specified
	 */
	if ((sts = pmLoadASCIINameSpace(namespace, 1)) < 0) {
	    printf("%s: Cannot load namespace from \"%s\": %s\n", pmGetProgname(), namespace, pmErrStr(sts));
	    exit(1);
	}
    }

    /* non-flag args are argv[optind] ... argv[argc-1] */

    for (i = 0; i < iter; i++) {
	if (optind >= argc)
	    pmTraversePMNS("", dometric);
	else {
	    int	a;
	    for (a = optind; a < argc; a++) {
		pmTraversePMNS(argv[a], dometric);
	    }
	}
	__pmProcessDataSize(&datasize);
	printf("[%d] %d metrics, %d getindom, %d insitu, %d ptr",
		i, _metrics, _indom, _insitu, _ptr);
	if (datasize)
	    printf(", mem leak: %ld Kbytes", datasize);
	putchar('\n');
	_metrics = _indom = _insitu = _ptr = 0;
    }

    exit(0);
}
