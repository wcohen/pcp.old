//
// Test QmcSource class
//

#include <QTextStream>
#include <qmc_source.h>

QTextStream cerr(stderr);
QTextStream cout(stdout);

int
main(int argc, char* argv[])
{
    int		sts = 0;
    int		c;
    char	buf[MAXHOSTNAMELEN];
    QString	source;

    pmSetProgname(argv[0]);

    while ((c = getopt(argc, argv, "D:?")) != EOF) {
	switch (c) {
	case 'D':
	    sts = pmSetDebug(optarg);
            if (sts < 0) {
		pmprintf("%s: unrecognized debug options specification (%s)\n",
			 pmGetProgname(), optarg);
                sts = 1;
            }
            break;
	case '?':
	default:
	    sts = 1;
	    break;
	}
    }

    if (sts) {
	pmprintf("Usage: %s\n", pmGetProgname());
	pmflush();
	exit(1);
        /*NOTREACHED*/
    }

    (void)gethostname(buf, MAXHOSTNAMELEN);
    buf[MAXHOSTNAMELEN-1] = '\0';

    fprintf(stderr,"*** Create an archive context ***\n");
    source = "archives/oview-short";
    QmcSource* src1 = QmcSource::getSource(PM_CONTEXT_ARCHIVE, source, false);

    if (src1->status() < 0) {
	pmprintf("%s: Error: Unable to create context to \"oview-short\": %s\n",
		 pmGetProgname(), pmErrStr(src1->status()));
	pmflush();
	sts = 1;
    }

    fprintf(stderr,"\n*** Create an archive context using the host name ***\n");
    source = QString("snort");
    QmcSource* src2 = QmcSource::getSource(PM_CONTEXT_HOST, source, true);

    if (src2->status() < 0) {
	pmprintf("%s: Error: Unable to create context to \"%s\": %s\n",
		pmGetProgname(), (const char *)source.toLatin1(),
		pmErrStr(src2->status()));
	pmflush();
	sts = 1;
    }

    if (src1 != src2) {
	pmprintf("%s: Error: Matching host to an archive failed: src1 = %s, src2 = %s\n",
		 pmGetProgname(), (const char *)src1->desc().toLatin1(),
		(const char *)src2->desc().toLatin1());
	pmflush();
	QmcSource::dumpList(cerr);
	sts = 1;
    }

    fprintf(stderr,"\n*** Create two live contexts to the local host ***\n");
    source = buf;
    QmcSource* src3 = QmcSource::getSource(PM_CONTEXT_HOST, source);
    QmcSource* src4 = QmcSource::getSource(PM_CONTEXT_HOST, source);

    if (src3->status() < 0) {
	pmprintf("%s: Error: Unable to create context to \"%s\": %s\n",
		 pmGetProgname(), buf, pmErrStr(src3->status()));
	pmflush();
    }

    if (src4->status() < 0) {
	pmprintf("%s: Error: Unable to create context to \"%s\": %s\n",
		 pmGetProgname(), buf, pmErrStr(src4->status()));
	pmflush();
    }

    if (src3 != src4) {
	pmprintf("%s: Error: Identical host test failed: src3 = %s, src4 = %s\n",
		 pmGetProgname(), (const char *)src3->desc().toLatin1(),
		(const char *)src4->desc().toLatin1());
	pmflush();
	QmcSource::dumpList(cerr);
	sts = 1;
    }

    fprintf(stderr,"\n*** Create a local context ***\n");
    source = QString::null;
    QmcSource* src5 = QmcSource::getSource(PM_CONTEXT_LOCAL, source);
    
    if (src5->status() < 0) {
	pmprintf("%s: Error: Unable to create context to localhost: %s\n",
		 pmGetProgname(), pmErrStr(src5->status()));
	pmflush();
	sts = 1;
    }

    fprintf(stderr,"\n*** List all known sources ***\n");
    QmcSource::dumpList(cout);

    pmflush();
    return sts;
}
