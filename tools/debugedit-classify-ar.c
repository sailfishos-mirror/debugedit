/* Quick and dirty ELF archive member debug checker.
   Copyright (C) 2025 Mark J. Wielaard <mark@klomp.org>
   This file is part of debugedit.

   This file is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <sys/stat.h>
#include <getopt.h>
#include <libelf.h>
#include <gelf.h>

// smaller than zero is quiet (no output),
// zero is show errors,
// bigger than zero is verbose.
static int verbose = 0;

// Negative is infinite, zero is failure, positive is max number to accept.
static int max_members = -1;

/* Returns -1 on error, 0 if member isn't an Elf objects or if it is
   an Elf object but doesn't contain any .[z]debug sections, 1 if it
   is an Elf object with .debug sections.  */
static int
classify_ar_member (Elf *member, const char *name, const char *file)
{
  /* Not an elf member? OK, no debug.  */
  if (elf_kind (member) != ELF_K_ELF)
    return 0;

  /* No sections? OK, no debug.  */
  size_t nshdrs;
  if (elf_getshdrnum (member, &nshdrs) != 0)
    {
      if (verbose >= 0)
	error (0, 0, "couldn't get section header number: %s: '%s[%s]'",
	       elf_errmsg (-1), file, name);
      return -1;
    }
  if (nshdrs == 0)
    return 0;

  size_t shstrndx;
  if (elf_getshdrstrndx (member, &shstrndx) < 0)
    {
      if (verbose >= 0)
	error (0, 0, "couldn't get section header string table: %s: '%s[%s]'",
	       elf_errmsg (-1), file, name);
      return -1;
    }

  bool found_debug = false;
  Elf_Scn *scn = NULL;
  while ((scn = elf_nextscn (member, scn)) != NULL)
    {
      GElf_Shdr shdr_mem;
      GElf_Shdr *shdr = gelf_getshdr (scn, &shdr_mem);
      if (shdr == NULL)
	{
	  if (verbose >= 0)
	    error (0, 0, "couldn't get section header: %s: '%s[%s]'",
		   elf_errmsg (-1), file, name);
	  return -1;
	}

      const char *sname = elf_strptr (member, shstrndx, shdr->sh_name);
      if (sname == NULL)
	{
	  if (verbose >= 0)
	    error (0, 0, "couldn't get section name: %s: '%s[%s]'",
		   elf_errmsg (-1), file, name);
	  return -1;
	}

      /* Just check the name.  */
      if (strncmp (sname, ".debug_", strlen (".debug_")) == 0
	  || strncmp (sname, ".zdebug_", strlen (".zdebug_")) == 0)
	found_debug = true;
    }

  return found_debug ? 1 : 0;
}

static int
classify_ar_elf (int fd, Elf *ar, const char *file)
{
  int members = 0;
  bool err = false;
  bool found_debug = false;
  int cmd = ELF_C_READ;
  Elf *elf;
  while ((elf = elf_begin (fd, cmd, ar)) != NULL)
    {
      Elf_Arhdr *arhdr = elf_getarhdr (elf);
      if (arhdr == NULL)
	{
	  if (verbose >= 0)
	    error (0, 0, "couldn't get ar header: %s: '%s'",
		   elf_errmsg (-1), file);
	  err = true;
	  break;
	}

      char *name = arhdr->ar_name ?: "<no-name>";
      int res = classify_ar_member (elf, name, file);
      if (res < 0)
	err = true;
      else if (res > 0)
	found_debug = true;

      cmd = elf_next (elf);

      if (elf_end (elf) != 0)
	{
	  if (verbose >= 0)
	    error(0, 0, "closing ar member: %s: '%s[%s]",
		  elf_errmsg (-1), file, name);
	  err = true;
	}

      if (err)
	break;

      members++;
    }

  if (err)
    return -1;

  if (!found_debug)
    {
      if (verbose > 0)
	error (0, 0, "no member with debug sections: %s", file);
      return -1;
    }

  if (max_members > 0 && members > max_members)
    {
      if (verbose > 0)
	error (0, 0, "too many members (%d): %s", members, file);
      return -1;
    }

  if (verbose > 0)
    error (0, 0, "found member(s) with debug sections: %s", file);

  return 0;
}

/* Returns zero if it is an ELF archive with max members of which at
   least one is an ELF object with .[z]debug sections.  Returns -1 if
   the file isn't a regular file, not an ELF archive, an error occurs
   while processing it, no member is an ELF object with debug sections
   or the archive contains more than max members. */
static int
classify_ar_file (const char *file)
{
  /* Don't open symlinks.  */
  int fd = open (file, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      if (errno == ELOOP)
	{
	  if (verbose >= 0)
	    error (0, 0, "cannot open symbolic link '%s'", file);
	}
      else
	{
	  if (verbose >= 0)
	    error (0, errno, "cannot open '%s'", file);
	}
      return -1;
    }

  struct stat st;
  if (fstat (fd, &st) != 0)
    {
      if (verbose >= 0)
	error (0, errno, "cannot fstat '%s'", file);
      close (fd);
      return -1;
    }

  if (S_ISDIR (st.st_mode))
    {
      if (verbose >= 0)
	error (0, 0, "cannot open directory '%s'", file);
      close (fd);
      return -1;
    }

  if (!S_ISREG (st.st_mode))
    {
      if (verbose >= 0)
	error (0, 0, "not a regular file '%s'", file);
      close (fd);
      return -1;
    }

  Elf *elf = elf_begin (fd, ELF_C_READ, NULL);
  if (elf == NULL)
    {
      if (verbose >= 0)
	error (0, 0, "cannot open Elf file: %s: '%s'", elf_errmsg (-1), file);
      close (fd);
      return -1;
    }

  if (elf_kind (elf) != ELF_K_AR)
    {
      if (verbose > 0)
	error (0, 0, "not an ELF archive: %s", file);
      elf_end (elf);
      close (fd);
      return -1;
    }

  int res = classify_ar_elf (fd, elf, file);

  elf_end (elf);
  close (fd);

  return res;
}

static struct option optionsTable[] =
  {
    { "max-members", required_argument, 0, 'm' },
    { "quiet", no_argument, 0, 'q' },
    { "verbose", no_argument, 0, 'v' },
    { "version", no_argument, 0, 'V' },
    { "help", no_argument, 0, '?' },
    { "usage", no_argument, 0, 'u' },
    { NULL, 0, 0, 0 }
  };

static const char *optionsChars = "m:qvV?u";

static const char *helpText =
  "Usage: %s [OPTION...] FILE\n"
  "  -m, --max-members=NUM    Maximum number of archive members to accept\n"
  "  -q, --quiet              Don't show any output (not even errors)\n"
  "  -v, --verbose            Show extra output\n"
  "\n"
  "Help options:\n"
  "  -?, --help               Show this help message\n"
  "  -u, --usage              Display brief usage message\n"
  "  -V, --version            Show program version\n";

static const char *usageText =
  "Usage: %s [-m|--max-members NUM]\n"
  "        [-q|--quiet] [-v|--verbose]\n"
  "        [-?|--help] [-u|--usage]\n"
  "        [-V|--version] FILE\n";

static void
help (const char *progname, bool error)
{
  FILE *f = error ? stderr : stdout;
  fprintf (f, helpText, progname);
  exit (error ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void
usage (const char *progname, bool error)
{
  FILE *f = error ? stderr : stdout;
  fprintf (f, usageText, progname);
  exit (error ? EXIT_FAILURE : EXIT_SUCCESS);
}

int
main (int argc, char **argv)
{
  bool show_version = false;

  /* Process arguments.  */
  while (1)
    {
      int opt_ndx = -1;
      int c = getopt_long (argc, argv, optionsChars, optionsTable, &opt_ndx);

      if (c == -1)
	break;

      switch (c)
	{
	default:
	case '?':
	  help (argv[0], opt_ndx == -1);
	  break;

	case 'u':
	  usage (argv[0], false);
	  break;

	case 'V':
	  show_version = true;
	  break;

	case 'm':
	  max_members = atoi (optarg);
	  if (max_members == 0)
	    help (argv[0], true);
	  break;

	case 'q':
	  verbose--;
	  break;

	case 'v':
	  verbose++;
	  break;
	}
    }

  if (show_version)
    {
      printf("%s %s\n", argv[0], VERSION);
      exit(EXIT_SUCCESS);
    }

  if (optind != argc - 1)
    {
      error (0, 0, "Need one FILE as input");
      usage (argv[0], true);
    }

  elf_version(EV_CURRENT);

  return classify_ar_file (argv[optind]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
