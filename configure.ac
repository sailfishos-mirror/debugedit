#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# Copyright (C) 2021, 2024, 2025 Mark J. Wielaard <mark@klomp.org>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

AC_PREREQ([2.69])
AC_INIT([debugedit], [5.2], [debugedit@sourceware.org])
AC_CONFIG_SRCDIR([tools/debugedit.c])
AC_CONFIG_HEADERS([config.h])

# We are using foreign, not gnu, because we don't provide some
# "standard" files like ChangeLog, AUTHORS, INSTALL, etc.
AM_INIT_AUTOMAKE([foreign silent-rules subdir-objects std-options
                  no-dist-gzip dist-xz tar-pax
                  -Wall -Wno-portability])

# Silence. Override with make V=1.
AM_SILENT_RULES([yes])

# autotest directory
AC_CONFIG_TESTDIR([tests])
AC_CONFIG_FILES([tests/Makefile tests/atlocal])
AM_MISSING_PROG([AUTOM4TE], [autom4te])

# Happy to use GNU (or other) system extensions
AC_USE_SYSTEM_EXTENSIONS

# Checks for programs.
AC_PROG_AWK
AC_PROG_SED
m4_version_prereq([2.70], [AC_PROG_CC], [AC_PROG_CC_C99])
AC_PROG_LN_S
AC_CHECK_TOOL([LD], [ld])
AC_CHECK_TOOL([READELF], [readelf])
AC_CHECK_TOOL([OBJCOPY], [objcopy])
AC_CHECK_TOOL([NM], [nm])
AC_CHECK_TOOL([AR], [ar])
AM_MISSING_PROG(HELP2MAN, help2man)

AC_CHECK_PROG([HAS_CPIO], [cpio], [yes], [no])
if test x$HAS_CPIO = xno; then
  AC_MSG_ERROR([cpio needed by find-debuginfo])
fi

AC_CHECK_PROG([HAS_EU_STRIP], [eu-strip], [yes], [no])
if test x$HAS_EU_STRIP = xno; then
  AC_MSG_ERROR([eu-strip needed by find-debuginfo (strip_to_debug)])
fi

AC_CHECK_PROG([HAS_XZ], [xz], [yes], [no])
if test x$HAS_XZ = xno; then
  AC_MSG_ERROR([xz needed by find-debuginfo (add_minidebug)])
fi

AC_CHECK_PROG([HAS_GDB_ADD_INDEX], [gdb-add-index], [yes], [no])
if test x$HAS_GDB_ADD_INDEX = xno; then
  AC_MSG_ERROR([gdb-add-index needed by find-debuginfo])
fi

# Whether dwz support -j.
# Make sure to compile something with -g.
# Run dwz on it with -j1.
DWZ_J=""
AC_CHECK_PROG([DWZ], [dwz], [dwz])
if test "x$DWZ" = "xdwz"; then
  save_CFLAGS="$CFLAGS"
  CFLAGS="$save_CFLAGS -g"
  AC_CACHE_CHECK([whether the dwz support -j], ac_cv_dwz_j, [dnl
    AC_LINK_IFELSE([AC_LANG_PROGRAM()],[dnl
      ac_cv_dwz_j=yes; ${DWZ} -j1 conftest$EXEEXT 2>/dev/null || ac_cv_dwz_j=no],
      AC_MSG_FAILURE([unexpected compile failure]))])
  if test "x$ac_cv_dwz_j" = "xyes"; then
    DWZ_J="-j"
  fi
  CFLAGS="$save_CFLAGS"
else
  AC_MSG_ERROR([dwz needed by find-debuginfo])
fi
AC_SUBST([DWZ_J])

# Only really an issue on 32bit platforms. Makes sure we'll get large off_t.
AC_SYS_LARGEFILE

# Checks for libraries.
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([LIBELF], [libelf])
PKG_CHECK_MODULES([LIBDW], [libdw])
PKG_CHECK_MODULES([XXHASH], [libxxhash >= 0.8.0])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h inttypes.h limits.h malloc.h stddef.h stdint.h stdlib.h string.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_INT8_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_ALLOCA
AC_FUNC_ERROR_AT_LINE
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_CHECK_FUNCS([memchr memset munmap strchr strdup strerror strrchr])

# Checks for compiler flags.
AC_CACHE_CHECK([whether gcc supports -gdwarf-5], ac_cv_gdwarf_5, [dnl
save_CFLAGS="$CFLAGS"
CFLAGS="-gdwarf-5"
AC_COMPILE_IFELSE([AC_LANG_SOURCE()], ac_cv_gdwarf_5=yes, ac_cv_gdwarf_5=no)
CFLAGS="$save_CFLAGS"
])
GDWARF_5_FLAG=$ac_cv_gdwarf_5
AC_SUBST([GDWARF_5_FLAG])

AC_CACHE_CHECK([whether -gdwarf-5 produced DWARF5 .debug_line], ac_cv_dwarf_5_debugline, [dnl
save_CFLAGS="$CFLAGS"
CFLAGS="-gdwarf-5"
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[int i=0;]],[[/* empty main */]])],
  [if $READELF --debug-dump=line conftest.o 2>&1 | \
   grep "DWARF Version:" 2>&1 | grep "5" > /dev/null 2>&1; \
     then ac_cv_dwarf_5_debugline=yes; \
     else ac_cv_dwarf_5_debugline=no; fi],
  ac_cv_dwarf_5_debugline=no)
CFLAGS="$save_CFLAGS"
])
DWARF_5_DEBUGLINE=$ac_cv_dwarf_5_debugline
AC_SUBST([DWARF_5_DEBUGLINE])

AC_CACHE_CHECK([whether -gdwarf-5 produced DWARF5 .debug_addr], ac_cv_dwarf_5_debugaddr, [dnl
save_CFLAGS="$CFLAGS"
CFLAGS="-gdwarf-5"
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[int i=0;]],[[/* empty main */]])],
  [if $READELF -S conftest.o 2>&1 | grep -F ".debug_addr" > /dev/null 2>&1; \
     then ac_cv_dwarf_5_debugaddr=yes; \
     else ac_cv_dwarf_5_debugaddr=no; fi],
  ac_cv_dwarf_5_debugaddr=no)
CFLAGS="$save_CFLAGS"
])
DWARF_5_DEBUGADDR=$ac_cv_dwarf_5_debugaddr
AC_SUBST([DWARF_5_DEBUGADDR])

AC_CACHE_CHECK([whether gcc supports -gz=none], ac_cv_gz_none, [dnl
save_CFLAGS="$CFLAGS"
CFLAGS="-gz=none"
AC_LINK_IFELSE([AC_LANG_PROGRAM()], ac_cv_gz_none=yes, ac_cv_gz_none=no)
CFLAGS="$save_CFLAGS"
])
if test "$ac_cv_gz_none" = "yes"; then
  GZ_NONE_FLAG="-gz=none"
else
  GZ_NONE_FLAG=""
fi
AC_SUBST([GZ_NONE_FLAG])

AC_CACHE_CHECK([whether gcc supports -gz=zlib], ac_cv_gz_zlib, [dnl
save_CFLAGS="$CFLAGS"
CFLAGS="-gz=zlib"
AC_LINK_IFELSE([AC_LANG_PROGRAM()], ac_cv_gz_zlib=yes, ac_cv_gz_zlib=no)
CFLAGS="$save_CFLAGS"
])
if test "$ac_cv_gz_zlib" = "yes"; then
  GZ_ZLIB_FLAG="-gz=zlib"
else
  GZ_ZLIB_FLAG=""
fi
AC_SUBST([GZ_ZLIB_FLAG])

AC_CACHE_CHECK([whether compiler needs -fdebug-macro], ac_cv_debug_macro, [dnl
save_CFLAGS="$CFLAGS"
CFLAGS="-fdebug-macro"
AC_LINK_IFELSE([AC_LANG_PROGRAM()], ac_cv_debug_macro=yes, ac_cv_debug_macro=no)
CFLAGS="$save_CFLAGS"
])
if test "$ac_cv_debug_macro" = "yes"; then
  DEBUG_MACRO_FLAG="-fdebug-macro"
else
  DEBUG_MACRO_FLAG=""
fi
AC_SUBST([DEBUG_MACRO_FLAG])

# We really need binutils readelf version 2.31 or higher
# Older versions don't cope correctly with compressed debug sections
# https://sourceware.org/bugzilla/show_bug.cgi?id=23919
# It is difficult to do a feature check for this, so just do a version check.
READELF_STRING=$($READELF --version | head -1)
# The double [[ and ]] is because of configure.ac being an m4 script.
READELF_REGEXP='s/GNU readelf[[a-zA-Z0-9(); _-]]*\([[1-9]]\)\.\([[0-9]]*\).*/\1\2/'
READELF_VER=$(echo "$READELF_STRING" | sed -e "$READELF_REGEXP")
AC_CACHE_CHECK([readelf version for compress tests], ac_cv_readelf_ver, [dnl
ac_cv_readelf_ver="$READELF_VER"
])
if test "$READELF_VER" -ge "231"; then
  READELF_VERSION_OK="yes"
else
  AC_MSG_WARN([$READELF_STRING ($READELF_VER) cannot do compression tests])
  READELF_VERSION_OK="no"
fi
AC_SUBST([READELF_VERSION_OK])

# Sadly gdb version >= 15 and dwz < 0.16 are incompatible.
# gdb-add-index will produce .gdb_index version 9 which dwz cannot handle
# (and silently throws away). Making gdb-add-index plus dwz tests fail.
# This is not very accurate, there are dwz versions 0.15 with the gdb_index
# version 9 support backported.
GDB_VERSION_STRING=$(gdb --version | head -1)
# The double [[ and ]] is because of configure.ac being an m4 script.
GDB_VERSION_REGEXP='s/.* ([[0-9]]+)\.([[0-9]]+).*/\1/'
GDB_MAJOR=$(echo "$GDB_VERSION_STRING" | sed -E "$GDB_VERSION_REGEXP")
# Older dwz output --version to stderr.
DWZ_VERSION_STRING=$(dwz --version 2>&1 | head -1)
# The double [[ and ]] is because of configure.ac being an m4 script.
DWZ_VERSION_REGEXP_MAJOR='s/.* ([[0-9]]+)\.([[0-9]]+).*/\1/'
DWZ_VERSION_REGEXP_MINOR='s/.* ([[0-9]]+)\.([[0-9]]+).*/\2/'
DWZ_MAJOR=$(echo "$DWZ_VERSION_STRING" | sed -E "$DWZ_VERSION_REGEXP_MAJOR")
DWZ_MINOR=$(echo "$DWZ_VERSION_STRING" | sed -E "$DWZ_VERSION_REGEXP_MINOR")
AC_CACHE_CHECK([gdb and dwz versions for gdb-add-index tests], ac_cv_gdb_dwz_ver, [dnl
ac_cv_gdb_dwz_ver="$GDB_MAJOR-vs-$DWZ_MAJOR.$DWZ_MINOR"
])
if test "$GDB_MAJOR" -lt "15"; then
  GDB_ADD_INDEX_CHECKS_OK="yes"
elif test "$DWZ_MAJOR" -gt "0"; then
  GDB_ADD_INDEX_CHECKS_OK="yes"
elif test "$DWZ_MINOR" -ge "16"; then
  GDB_ADD_INDEX_CHECKS_OK="yes"
else
  AC_MSG_WARN([$GDB_VERSION_STRING and $DWZ_VERSION_STRING cannot do gdb-add-index tests])
  GDB_ADD_INDEX_CHECKS_OK="no"
fi
AC_SUBST([GDB_ADD_INDEX_CHECKS_OK])

ENABLE_INLINED_XXHASH=yes
AC_ARG_ENABLE(inlined-xxhash,
	AS_HELP_STRING([--disable-inlined-xxhash], [disable the inlined-only version of xxhash library]),
	ENABLE_INLINED_XXHASH=$enableval,
	ENABLE_INLINED_XXHASH=yes)

if test x$ENABLE_INLINED_XXHASH = xyes; then
	XXHASH_CFLAGS="$XXHASH_CFLAGS -DXXH_INLINE_ALL"
	XXHASH_LIBS=""
fi

# And generate the output files.
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
