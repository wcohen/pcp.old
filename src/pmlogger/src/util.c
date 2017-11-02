/*
 * Copyright (c) 2014 Red Hat.
 * Copyright (c) 1995 Silicon Graphics, Inc.  All Rights Reserved.
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

#include "logger.h"

void
buildinst(int *numinst, int **intlist, char ***extlist, int intid, char *extid)
{
    char	**el;
    char	**tmp_el;
    int		*il;
    int		*tmp_il;
    int		num = *numinst;

    if (num == 0) {
	il = NULL;
	el = NULL;
    }
    else {
	il = *intlist;
	el = *extlist;
    }

    tmp_el = (char **)realloc(el, (num+1)*sizeof(el[0]));
    if (tmp_el == NULL) {
	__pmNoMem("buildinst extlist", (num+1)*sizeof(el[0]), PM_FATAL_ERR);
	/* NOTREACHED */
    }
    el = tmp_el;
    tmp_il = (int *)realloc(il, (num+1)*sizeof(il[0]));
    if (tmp_il == NULL) {
	__pmNoMem("buildinst intlist", (num+1)*sizeof(il[0]), PM_FATAL_ERR);
	/* NOTREACHED */
    }
    il = tmp_il;

    il[num] = intid;

    if (extid == NULL)
	el[num] = NULL;
    else {
	if (*extid == '"') {
	    char	*p;
	    p = ++extid;
	    while (*p && *p != '"') p++;
	    *p = '\0';
	}
	el[num] = strdup(extid);
    }

    *numinst = ++num;
    *intlist = il;
    *extlist = el;
}

void
freeinst(int *numinst, int *intlist, char **extlist)
{
    int		i;

    if (*numinst) {
	free(intlist);
	for (i = 0; i < *numinst; i++)
	    free(extlist[i]);
	free(extlist);

	*numinst = 0;
    }
}

/*
 * Link the new fetch groups back to their task structure
 */
void
linkback(task_t *tp)
{
    fetchctl_t	*fcp;

    for (fcp = tp->t_fetch; fcp != NULL; fcp = fcp->f_next)
	fcp->f_aux = (void *)tp;
}
