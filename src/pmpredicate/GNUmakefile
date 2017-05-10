#
# Copyright (c) 2013-2017 Red Hat.
# Copyright (c) 2000,2004 Silicon Graphics, Inc.  All Rights Reserved.
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
# 

TOPDIR = ../..
include $(TOPDIR)/src/include/builddefs

CFILES	= pmpredicate.c
CMDTARGET = pmpredicate$(EXECSUFFIX)
TARGETS = $(CMDTARGET)
LLDLIBS	= $(PCPLIB)
LSRCFILES = GNUmakefile.install $(CONFIGS)
DEMODIR	= $(PCP_DEMOS_DIR)/pmpredicate

default:	$(TARGETS)

include $(BUILDRULES)

install:	$(TARGETS)
	$(INSTALL) -m 755 $(CMDTARGET) $(PCP_BIN_DIR)/$(CMDTARGET)
	$(INSTALL) -m 755 -d $(DEMODIR)
	$(INSTALL) -m 644 GNUmakefile.install $(DEMODIR)/Makefile
	$(INSTALL) -m 644 $(CFILES) $(DEMODIR)

pmpredicate$(EXECSUFFIX):  pmpredicate.o
	rm -f $@
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

default_pcp:	default

install_pcp:	install
