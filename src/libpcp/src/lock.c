/*
 * Copyright (c) 2013 Red Hat.
 * Copyright (c) 2011 Ken McDonell.  All Rights Reserved.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#include "pmapi.h"
#include "impl.h"
#include "internal.h"
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#ifdef PM_MULTI_THREAD
static pthread_mutex_t	lock_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t		__pmLock_extcall = PTHREAD_MUTEX_INITIALIZER;
#else /* !PM_MULTI_THREAD - symbols exposed at the shlib ABI level */
static void		*lock_lock;
void			__pmLock_extcall;
void *__pmLock_libpcp;
void *__pmLock_extcall;
void __pmInitLocks(void)
{
    static int		done = 0;
    if (!done) {
	SetupDebug();
	done = 1;
    }
}
int __pmMultiThreaded(int scope) { (void)scope; return 0; }
int __pmLock(void *l, const char *f, int n) { (void)l, (void)f, (void)n; return 0; }
int __pmIsLocked(void *l) { (void)l; return 0; }
int __pmUnlock(void *l, const char *f, int n) { (void)l, (void)f, (void)n; return 0; }
#endif /* PM_MULTI_THREAD */

#ifdef PM_MULTI_THREAD
typedef struct {
    void	*lock;
    int		count;
} lockdbg_t;

#define PM_LOCK_OP	1
#define PM_UNLOCK_OP	2

static int		multi_init[PM_SCOPE_MAX+1];
static pthread_t	multi_seen[PM_SCOPE_MAX+1];

#if defined(PM_MULTI_THREAD) && defined(PM_MULTI_THREAD_DEBUG)
/*
 * return true if lock == lock_lock
 */
int
__pmIsLockLock(void *lock)
{
    return lock == (void *)&lock_lock;
}
#endif

/* the big libpcp lock */
#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
pthread_mutex_t	__pmLock_libpcp = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
pthread_mutex_t	__pmLock_libpcp = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef HAVE___THREAD
pthread_key_t 	__pmTPDKey = 0;

static void
__pmTPD__destroy(void *addr)
{
    free(addr);
}
#endif /* HAVE___THREAD */
#endif /* PM_MULTI_THREAD */

static void
SetupDebug(void)
{
    /*
     * if $PCP_DEBUG is set in the environment then use the (decimal)
     * value to set bits in pmDebug via bitwise ORing
     */
    char	*val;
    int		ival;

    PM_LOCK(__pmLock_extcall);
    val = getenv("PCP_DEBUG");		/* THREADSAFE */
    if (val != NULL) {
	char	*end;
	ival = strtol(val, &end, 10);
	if (*end != '\0') {
	    fprintf(stderr, "Error: $PCP_DEBUG=%s is not numeric, ignored\n", val);
	}
	else
	    pmDebug |= ival;
    }
    PM_UNLOCK(__pmLock_extcall);
}

#ifdef PM_MULTI_THREAD_DEBUG
static char
*lockname(void *lock)
{
    static char locknamebuf[20];
    int		ctxid;

    if (__pmIsDeriveLock(lock))
	return "derived_metric";
    else if (__pmIsAuxconnectLock(lock))
	return "auxconnect";
    else if (__pmIsConfigLock(lock))
	return "config";
#ifdef PM_FAULT_INJECTION
    else if (__pmIsFaultLock(lock))
	return "fault";
#endif
    else if (__pmIsPduLock(lock))
	return "pdu";
    else if (__pmIsPdubufLock(lock))
	return "pdubuf";
    else if (__pmIsUtilLock(lock))
	return "util";
    else if (__pmIsContextsLock(lock))
	return "contexts";
    else if (__pmIsIpcLock(lock))
	return "ipc";
    else if (__pmIsOptfetchLock(lock))
	return "optfetch";
    else if (__pmIsErrLock(lock))
	return "err";
    else if (__pmIsLockLock(lock))
	return "lock";
    else if (__pmIsLogutilLock(lock))
	return "logutil";
    else if (__pmIsPmnsLock(lock))
	return "pmns";
    else if (__pmIsAFLock(lock))
	return "AF";
    else if (__pmIsSecureServerLock(lock))
	return "secureserver";
    else if (__pmIsConnectLock(lock))
	return "connect";
    else if (lock == (void *)&__pmLock_extcall)
	return "global_extcall";
    else if ((ctxid = __pmIsContextLock(lock)) != -1) {
	snprintf(locknamebuf, sizeof(locknamebuf), "c_lock[slot %d]", ctxid);
	return locknamebuf;
    }
    else {
	snprintf(locknamebuf, sizeof(locknamebuf), PRINTF_P_PFX "%p", lock);
	return locknamebuf;
    }
}

void
__pmDebugLock(int op, void *lock, const char *file, int line)
{
    int			report = 0;
    int			ctx;
    static __pmHashCtl	hashctl = { 0, 0, NULL };
    __pmHashNode	*hp = NULL;
    lockdbg_t		*ldp;
    int			try;
    int			sts;

    if (lock == (void *)&__pmLock_libpcp) {
	if ((pmDebug & DBG_TRACE_APPL0) | ((pmDebug & (DBG_TRACE_APPL0 | DBG_TRACE_APPL1 | DBG_TRACE_APPL2)) == 0))
	    report = DBG_TRACE_APPL0;
    }
    else if ((ctx = __pmIsContextLock(lock)) >= 0) {
	if ((pmDebug & DBG_TRACE_APPL1) | ((pmDebug & (DBG_TRACE_APPL0 | DBG_TRACE_APPL1 | DBG_TRACE_APPL2)) == 0))
	    report = DBG_TRACE_APPL1;
    }
    else {
	if ((pmDebug & DBG_TRACE_APPL2) | ((pmDebug & (DBG_TRACE_APPL0 | DBG_TRACE_APPL1 | DBG_TRACE_APPL2)) == 0))
	    report = DBG_TRACE_APPL2;
    }

    if (report) {
	__psint_t		key = (__psint_t)lock;
	fprintf(stderr, "%s:%d %s", file, line, op == PM_LOCK_OP ? "lock" : "unlock");
	try = 0;
again:
	hp = __pmHashSearch((unsigned int)key, &hashctl);
	while (hp != NULL) {
	    ldp = (lockdbg_t *)hp->data;
	    if (ldp->lock == lock)
		break;
	    hp = hp->next;
	}
	if (hp == NULL) {
	    char	errmsg[PM_MAXERRMSGLEN];
	    ldp = (lockdbg_t *)malloc(sizeof(lockdbg_t));
	    ldp->lock = lock;
	    ldp->count = 0;
	    sts = __pmHashAdd((unsigned int)key, (void *)ldp, &hashctl);
	    if (sts == 1) {
		try++;
		if (try == 1)
		    goto again;
	    }
	    hp = NULL;
	    fprintf(stderr, " hash control failure: %s\n", pmErrStr_r(-sts, errmsg, sizeof(errmsg)));
	}
    }

    if (report == DBG_TRACE_APPL0) {
	fprintf(stderr, "(global_libpcp)");
    }
    else if (report == DBG_TRACE_APPL1) {
	fprintf(stderr, "(ctx %d)", ctx);
    }
    else if (report == DBG_TRACE_APPL2) {
	fprintf(stderr, "(%s)", lockname(lock));
    }
    if (report) {
	if (hp != NULL) {
	    ldp = (lockdbg_t *)hp->data;
	    if (op == PM_LOCK_OP) {
		ldp->count++;
		if (ldp->count != 1)
		    fprintf(stderr, " [count=%d]", ldp->count);
	    }
	    else {
		ldp->count--;
		if (ldp->count != 0)
		    fprintf(stderr, " [count=%d]", ldp->count);
	    }
	}
	fputc('\n', stderr);
#ifdef HAVE_BACKTRACE
#define MAX_TRACE_DEPTH 32
	if (pmDebug & DBG_TRACE_DESPERATE) {
	    void	*backaddr[MAX_TRACE_DEPTH];
	    sts = backtrace(backaddr, MAX_TRACE_DEPTH);
	    if (sts > 0) {
		char	**symbols;
		symbols = backtrace_symbols(backaddr, MAX_TRACE_DEPTH);
		if (symbols != NULL) {
		    int		i;
		    fprintf(stderr, "backtrace:\n");
		    for (i = 0; i < sts; i++)
			fprintf(stderr, "  %s\n", symbols[i]);
		    free(symbols);
		}
	    }
	}

#endif
    }
}
#else
#define __pmDebugLock(op, lock, file, line) do { } while (0)
#endif

/*
 * Initialize a single mutex to our preferred flavour ...
 * PTHREAD_MUTEX_ERRORCHECK today.
 */
void
__pmInitMutex(pthread_mutex_t *lock)
{
    int			sts;
    char		errmsg[PM_MAXERRMSGLEN];
    pthread_mutexattr_t	attr;

    if ((sts = pthread_mutexattr_init(&attr)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "__pmInitMutex(");
#ifdef PM_MULTI_THREAD_DEBUG
	fprintf(stderr, "%s", lockname(lock));
#else
	fprintf(stderr, "%p", lock);
#endif
	fprintf(stderr, ": pthread_mutexattr_init failed: %s\n", errmsg);
	exit(4);
    }
    if ((sts = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "__pmInitMutex(");
#ifdef PM_MULTI_THREAD_DEBUG
	fprintf(stderr, "%s", lockname(lock));
#else
	fprintf(stderr, "%p", lock);
#endif
	fprintf(stderr, ": pthread_mutexattr_settype failed: %s\n", errmsg);
	exit(4);
    }
    if ((sts = pthread_mutex_init(lock, &attr)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "__pmInitMutex(");
#ifdef PM_MULTI_THREAD_DEBUG
	fprintf(stderr, "%s", lockname(lock));
#else
	fprintf(stderr, "%p", lock);
#endif
	fprintf(stderr, ": pthread_mutex_init failed: %s\n", errmsg);
	exit(4);
    }
    pthread_mutexattr_destroy(&attr);
}

/*
 * Do one-trip mutex initializations ... PM_INIT_LOCKS() comes here
 */
void
__pmInitLocks(void)
{
    static pthread_mutex_t	init = PTHREAD_MUTEX_INITIALIZER;
    static int			done = 0;
    int				psts;
    char			errmsg[PM_MAXERRMSGLEN];
    if ((psts = pthread_mutex_lock(&init)) != 0) {
	pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	fprintf(stderr, "__pmInitLocks: pthread_mutex_lock failed: %s", errmsg);
	exit(4);
    }
    if (!done) {
	SetupDebug();
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
	/*
	 * Unable to initialize at compile time, need to do it here in
	 * a one trip for all threads run-time initialization.
	 */
	pthread_mutexattr_t	attr;

	if ((psts = pthread_mutexattr_init(&attr)) != 0) {
	    pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	    fprintf(stderr, "__pmInitLocks: pthread_mutexattr_init failed: %s", errmsg);
	    exit(4);
	}
	if ((psts = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) != 0) {
	    pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	    fprintf(stderr, "__pmInitLocks: pthread_mutexattr_settype failed: %s", errmsg);
	    exit(4);
	}
	if ((psts = pthread_mutex_init(&__pmLock_libpcp, &attr)) != 0) {
	    pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	    fprintf(stderr, "__pmInitLocks: pthread_mutex_init failed: %s", errmsg);
	    exit(4);
	}
	pthread_mutexattr_destroy(&attr);
#endif
#ifndef HAVE___THREAD
	/* first thread here creates the thread private data key */
	if ((psts = pthread_key_create(&__pmTPDKey, __pmTPD__destroy)) != 0) {
	    pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	    fprintf(stderr, "__pmInitLocks: pthread_key_create failed: %s", errmsg);
	    exit(4);
	}
#endif
	/*
	 * Now initialize the local mutexes.
	 */
	init_pmns_lock();
	init_AF_lock();
	init_secureserver_lock();
	init_connect_lock();

	done = 1;
    }
    if ((psts = pthread_mutex_unlock(&init)) != 0) {
	pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	fprintf(stderr, "__pmInitLocks: pthread_mutex_unlock failed: %s", errmsg);
	exit(4);
    }
#ifndef HAVE___THREAD
    if (pthread_getspecific(__pmTPDKey) == NULL) {
	__pmTPD	*tpd = (__pmTPD *)malloc(sizeof(__pmTPD));
	if (tpd == NULL) {
	    __pmNoMem("__pmInitLocks: __pmTPD", sizeof(__pmTPD), PM_FATAL_ERR);
	    /*NOTREACHED*/
	}
	if ((psts = pthread_setspecific(__pmTPDKey, tpd)) != 0) {
	    pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	    fprintf(stderr, "__pmInitLocks: pthread_setspecific failed: %s", errmsg);
	    exit(4);
	}
	memset((void *)tpd, 0, sizeof(*tpd));
	tpd->curr_handle = PM_CONTEXT_UNDEF;
    }
#endif
}

int
__pmMultiThreaded(int scope)
{
    int		sts = 0;

    PM_LOCK(lock_lock);
    if (!multi_init[scope]) {
	multi_init[scope] = 1;
	multi_seen[scope] = pthread_self();
    }
    else {
	if (!pthread_equal(multi_seen[scope], pthread_self()))
	    sts = 1;
    }
    PM_UNLOCK(lock_lock);
    return sts;
}

int
__pmLock(void *lock, const char *file, int line)
{
    int		sts;

    if ((sts = pthread_mutex_lock(lock)) != 0) {
	sts = -sts;
	if (pmDebug & DBG_TRACE_DESPERATE)
	    fprintf(stderr, "%s:%d: lock failed: %s\n", file, line, pmErrStr(sts));
#ifdef BUILD_WITH_LOCK_ASSERTS
	assert(sts == 0);
#endif
    }

    if (pmDebug & DBG_TRACE_LOCK)
	__pmDebugLock(PM_LOCK_OP, lock, file, line);

    return sts;
}

int
__pmIsLocked(void *lock)
{
    int		sts;

    if ((sts = pthread_mutex_trylock(lock)) != 0) {
	if (sts == EBUSY)
	    return 1;
	sts = -sts;
	if (pmDebug & DBG_TRACE_DESPERATE)
	    fprintf(stderr, "islocked: trylock %p failed: %s\n", lock, pmErrStr(sts));
	return 0;
    }
    if ((sts = pthread_mutex_unlock(lock)) != 0) {
	sts = -sts;
	if (pmDebug & DBG_TRACE_DESPERATE)
	    fprintf(stderr, "islocked: unlock %p failed: %s\n", lock, pmErrStr(sts));
    }
    return 0;
}

#ifdef BUILD_WITH_LOCK_ASSERTS
/*
 * Assumes lock is a pthread mutex and not recursive.
 */
void
__pmCheckIsUnlocked(void *lock, char *file, int line)
{
    pthread_mutex_t	*lockp = (pthread_mutex_t *)lock;
    int	sts;

    if ((sts = pthread_mutex_trylock(lockp)) != 0) {
	if (sts == EBUSY) {
#ifdef __GLIBC__
	   fprintf(stderr, "__pmCheckIsUnlocked(%s): [%s:%d] __lock=%d __count=%d\n", lockname(lockp), file, line, lockp->__data.__lock, lockp->__data.__count);
#else
	   fprintf(stderr, "__pmCheckIsUnlocked(%s): [%s:%d] locked\n", lockname(lockp), file, line);
#endif
	}
	else
	    fprintf(stderr, "__pmCheckIsUnlocked: trylock(%s) failed: %s\n", lockname(lockp), pmErrStr(-sts));
	return;
    }
    else {
	if ((sts = pthread_mutex_unlock(lockp)) != 0) {
	    fprintf(stderr, "__pmCheckIsUnlocked: unlock(%s) failed: %s\n", lockname(lockp), pmErrStr(-sts));
	}
    }
    return;
}
#endif /* BUILD_WITH_LOCK_ASSERTS */

int
__pmUnlock(void *lock, const char *file, int line)
{
    int		sts; 

    if (pmDebug & DBG_TRACE_LOCK)
	__pmDebugLock(PM_UNLOCK_OP, lock, file, line);

    if ((sts = pthread_mutex_unlock(lock)) != 0) {
	sts = -sts;
	if (pmDebug & DBG_TRACE_DESPERATE)
	    fprintf(stderr, "%s:%d: unlock failed: %s\n", file, line, pmErrStr(sts));
#ifdef BUILD_WITH_LOCK_ASSERTS
	assert(sts == 0);
#endif
    }

    return sts;
}
