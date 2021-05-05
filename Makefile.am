## Process this file with automake to produce Makefile.in.

## Copyright (C) 2021 Mark J. Wielaard <mark@klomp.org>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Tests have their own directory and Makefile.am everything else is
# defined here.
SUBDIRS = tests

AM_CFLAGS = -Wall

# All our programs
bin_PROGRAMS = debugedit sepdebugcrcfix

# Install find-debuginfo.sh in $(bindir) and distribute it.
bin_SCRIPTS = $(top_srcdir)/scripts/find-debuginfo.sh

debugedit_SOURCES = tools/debugedit.c \
		    tools/hashtab.c \
		    tools/md5.c \
		    tools/sha1.c
debugedit_CFLAGS = @LIBELF_CFLAGS@ @LIBDW_CFLAGS@ $(AM_CFLAGS)
debugedit_LDADD = @LIBELF_LIBS@ @LIBDW_LIBS@

sepdebugcrcfix_SOURCES = tools/sepdebugcrcfix.c
sepdebugcrcfix_CFLAGS = @LIBELF_CFLAGS@ $(AM_CFLAGS)
sepdebugcrcfix_LDADD = @LIBELF_LIBS@

# Manual pages are generated for dist
dist_man_MANS = debugedit.1 sepdebugcrcfix.1 find-debuginfo.sh.1

# Common dependency for man pages. Man pages are only regenerated when
# their main source is newer, or configure.ac (version) is updated.
TOP_CONFIGURE_AC = $(top_srcdir)/configure.ac

debugedit.1: tools/debugedit.c $(TOP_CONFIGURE_AC)
	$(MAKE) $(AM_MAKEFLAGS) debugedit$(EXEEXT)
	$(HELP2MAN) -N --output=$@ \
	  --name='debug source path manipulation tool' \
	  ./debugedit$(EXEEXT)

sepdebugcrcfix.1: tools/sepdebugcrcfix.c $(TOP_CONFIGURE_AC)
	$(MAKE) $(AM_MAKEFLAGS) sepdebugcrcfix$(EXEEXT)
	$(HELP2MAN) -N --output=$@ \
	  --name='fixes CRC for separate .debug files' \
	  ./sepdebugcrcfix$(EXEEXT)

find-debuginfo.sh.1: $(top_srcdir)/scripts/find-debuginfo.sh $(TOP_CONFIGURE_AC)
	$(HELP2MAN) -N --output=$@ \
	  --name='finds debuginfo and processes it' \
	  $(top_srcdir)/scripts/find-debuginfo.sh

noinst_HEADERS= tools/ansidecl.h \
		tools/hashtab.h \
		tools/md5.h \
		tools/sha1.h

EXTRA_DIST = README COPYING COPYING3 COPYING.LIB
