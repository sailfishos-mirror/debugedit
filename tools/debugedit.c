/* Copyright (C) 2001-2003, 2005, 2007, 2009-2011, 2016, 2017 Red Hat, Inc.
   Copyright (C) 2022, 2023, 2024, 2025 Mark J. Wielaard <mark@klomp.org>
   Written by Alexander Larsson <alexl@redhat.com>, 2002
   Based on code by Jakub Jelinek <jakub@redhat.com>, 2001.
   String/Line table rewriting by Mark Wielaard <mjw@redhat.com>, 2017.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <error.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>

#include <gelf.h>
#include <dwarf.h>
#include <libelf.h>

#ifndef MAX
#define MAX(m, n) ((m) < (n) ? (n) : (m))
#endif
#ifndef MIN
#define MIN(m, n) ((m) > (n) ? (n) : (m))
#endif


/* Unfortunately strtab manipulation functions were only officially added
   to elfutils libdw in 0.167.  Before that there were internal unsupported
   ebl variants.  While libebl.h isn't supported we'll try to use it anyway
   if the elfutils we build against is too old.  */
#include <elfutils/version.h>
#if _ELFUTILS_PREREQ (0, 167)
#include <elfutils/libdwelf.h>
typedef Dwelf_Strent		Strent;
typedef Dwelf_Strtab		Strtab;
#define strtab_init		dwelf_strtab_init
#define strtab_add(X,Y)		dwelf_strtab_add(X,Y)
#define strtab_add_len(X,Y,Z)	dwelf_strtab_add_len(X,Y,Z)
#define strtab_free		dwelf_strtab_free
#define strtab_finalize		dwelf_strtab_finalize
#define strent_offset		dwelf_strent_off
#else
#include <elfutils/libebl.h>
typedef struct Ebl_Strent	Strent;
typedef struct Ebl_Strtab	Strtab;
#define strtab_init		ebl_strtabinit
#define strtab_add(X,Y)		ebl_strtabadd(X,Y,0)
#define strtab_add_len(X,Y,Z)	ebl_strtabadd(X,Y,Z)
#define strtab_free		ebl_strtabfree
#define strtab_finalize		ebl_strtabfinalize
#define strent_offset		ebl_strtaboffset
#endif

#include <search.h>

#include "tools/hashtab.h"

#include "xxhash.h"

#define DW_TAG_partial_unit 0x3c
#define DW_FORM_sec_offset 0x17
#define DW_FORM_exprloc 0x18
#define DW_FORM_flag_present 0x19
#define DW_FORM_ref_sig8 0x20

char *base_dir = NULL;
char *dest_dir = NULL;
char *list_file = NULL;
int list_file_fd = -1;
int do_build_id = 0;
int no_recompute_build_id = 0;
bool preserve_dates = false;
char *build_id_seed = NULL;

int show_version = 0;

/* We go over the debug sections in two phases. In phase zero we keep
   track of any needed changes and collect strings, indexes and
   sizes. In phase one we do the actual replacements updating the
   strings, indexes and writing out new debug sections. The following
   keep track of various changes that might be needed. */

/* Whether we need to do any literal string (DW_FORM_string) replacements
   in debug_info. */
static bool need_string_replacement = false;
/* Whether we need to do any updates of the string indexes (DW_FORM_strp)
   in debug_info for string indexes. */
static bool need_strp_update = false;
/* Likewise for DW_FORM_line_strp. */
static bool need_line_strp_update = false;
/* If the debug_line changes size we will need to update the
   DW_AT_stmt_list attributes indexes in the debug_info. */
static bool need_stmt_update = false;

/* If we recompress any debug section we need to write out the ELF
   again. */
static bool recompressed = false;

/* Storage for dynamically allocated strings to put into string
   table. Keep together in memory blocks of 16K. */
#define STRMEMSIZE (16 * 1024)
struct strmemblock
{
  struct strmemblock *next;
  char memory[0];
};

/* We keep track of each index in the original string table and the
   associated entry in the new table so we don't insert identical
   strings into the new string table. If constructed correctly the
   original strtab shouldn't contain duplicate strings anyway. Any
   actual identical strings could be deduplicated, but searching for
   and comparing the indexes is much faster than comparing strings
   (and we don't have to construct replacement strings). */
struct stridxentry
{
  uint32_t idx; /* Original index in the string table. */
  Strent *entry; /* Entry in the new table. */
};

/* Turns out we do need at least one replacement string when there are
   strings that are never used in any of the debug sections, but that
   turn up in the .debug_str_offsets. In that case (which really
   shouldn't occur because it means the DWARF producer added unused
   strings to the string table) we (warn and) replace the entry with
   "<debugedit>".  Call create_dummy_debugedit_stridxentry to add the
   actual string.  */
static struct stridxentry debugedit_stridxentry = { 0, NULL };

/* Storage for new string table entries. Keep together in memory to
   quickly search through them with tsearch. */
#define STRIDXENTRIES ((16 * 1024) / sizeof (struct stridxentry))
struct strentblock
{
  struct strentblock *next;
  struct stridxentry entry[0];
};

/* All data to keep track of the existing and new string table. */
struct strings
{
  Strtab *str_tab;			/* The new string table. */
  Elf_Data orig_data;			/* Original Elf_Data. */
  char *str_buf;			/* New Elf_Data d_buf. */
  struct strmemblock *blocks;		/* The first strmemblock. */
  struct strmemblock *last_block;	/* The currently used strmemblock. */
  size_t stridx;			/* Next free byte in last block. */
  struct strentblock *entries;		/* The first string index block. */
  struct strentblock *last_entries;	/* The currently used strentblock. */
  size_t entryidx;			/* Next free entry in the last block. */
  void *strent_root;			/* strent binary search tree root. */
};

struct line_table
{
  struct CU *cu;      /* (first) CU that references this table.  */

  size_t old_idx;     /* Original offset. */
  size_t new_idx;     /* Offset in new debug_line section. */
  ssize_t size_diff;  /* Difference in (header) size. */
  bool replace_dirs;  /* Whether to replace any dir paths.  */
  bool replace_files; /* Whether to replace any file paths. */

  /* Header fields. */
  uint32_t unit_length;
  uint16_t version;
  uint32_t header_length;
  uint8_t min_instr_len;
  uint8_t max_op_per_instr; /* Only if version >= 4 */
  uint8_t default_is_stmt;
  int8_t line_base;
  uint8_t line_range;
  uint8_t opcode_base;
};

struct debug_lines
{
  struct line_table *table; /* Malloc/Realloced. */
  size_t size;              /* Total number of line_tables.
			       Updated by get_line_table. */
  size_t used;              /* Used number of line_tables.
			       Updated by get_line_table. */
  size_t debug_lines_len;   /* Total size of new debug_line section.
			       updated by edit_dwarf2_line. */
  char *line_buf;           /* New Elf_Data d_buf. */
};

struct CU
{
  int ptr_size;
  int cu_version;
  /* The offset into the .debug_str_offsets section for this CU.  */
  uint32_t str_offsets_base;
  /* The offset into the .debug_macros section for this CU (DW_AT_macros).  */
  uint32_t macros_offs;

  struct CU *next;
};

typedef struct
{
  Elf *elf;
  GElf_Ehdr ehdr;
  Elf_Scn **scn;
  const char *filename;
  int lastscn;
  size_t phnum;
  struct strings debug_str, debug_line_str;
  struct debug_lines lines;
  /* List of CUs that keeps track of version, ptr_size,
     str_offsets_base, etc. so other structures, like macros, can use
     those properties for parsing.  */
  struct CU *cus;
  GElf_Shdr shdr[0];
} DSO;

typedef struct
{
  unsigned char *ptr;
  uint32_t addend;
  int ndx;
} REL;

typedef struct
{
  Elf64_Addr r_offset;
  int ndx;
} LINE_REL;

typedef struct debug_section
  {
    const char *name;
    unsigned char *data;
    Elf_Data *elf_data;
    size_t size;
    int sec, relsec;
    int reltype;
    REL *relbuf;
    REL *relend;
    bool rel_updated;
    uint32_t ch_type;
    /* Only happens for COMDAT .debug_macro and .debug_types.  */
    struct debug_section *next;
  } debug_section;

static debug_section debug_sections[] =
  {
#define DEBUG_INFO	0
#define DEBUG_ABBREV	1
#define DEBUG_LINE	2
#define DEBUG_ARANGES	3
#define DEBUG_PUBNAMES	4
#define DEBUG_PUBTYPES	5
#define DEBUG_MACINFO	6
#define DEBUG_LOC	7
#define DEBUG_STR	8
#define DEBUG_FRAME	9
#define DEBUG_RANGES	10
#define DEBUG_TYPES	11
#define DEBUG_MACRO	12
#define DEBUG_GDB_SCRIPT	13
#define DEBUG_RNGLISTS	14
#define DEBUG_LINE_STR	15
#define DEBUG_ADDR	16
#define DEBUG_STR_OFFSETS	17
#define DEBUG_LOCLISTS	18
    { ".debug_info",		},
    { ".debug_abbrev",		},
    { ".debug_line",		},
    { ".debug_aranges",		},
    { ".debug_pubnames",	},
    { ".debug_pubtypes",	},
    { ".debug_macinfo",		},
    { ".debug_loc",		},
    { ".debug_str",		},
    { ".debug_frame",		},
    { ".debug_ranges",		},
    { ".debug_types",		},
    { ".debug_macro",		},
    { ".debug_gdb_scripts",	},
    { ".debug_rnglists",	},
    { ".debug_line_str",	},
    { ".debug_addr",		},
    { ".debug_str_offsets",	},
    { ".debug_loclists",	},
    { NULL,			}
  };

static void
setup_lines (struct debug_lines *lines)
{
  lines->table = NULL;
  lines->size = 0;
  lines->used = 0;
  lines->debug_lines_len = 0;
  lines->line_buf = NULL;
}

static void
destroy_lines (struct debug_lines *lines)
{
  free (lines->table);
  free (lines->line_buf);
}

static void
destroy_cus (struct CU *cu)
{
  while (cu != NULL)
    {
      struct CU *next_cu = cu->next;
      free (cu);
      cu = next_cu;
    }
}

#define read_uleb128(ptr) ({		\
  unsigned int ret = 0;			\
  unsigned int c;			\
  int shift = 0;			\
  do					\
    {					\
      c = *(ptr)++;			\
      ret |= (c & 0x7f) << shift;	\
      shift += 7;			\
    } while (c & 0x80);			\
					\
  if (shift >= 35)			\
    ret = UINT_MAX;			\
  ret;					\
})

#define write_uleb128(ptr,val) ({	\
  uint32_t valv = (val);		\
  do					\
    {					\
      unsigned char c = valv & 0x7f;	\
      valv >>= 7;			\
      if (valv)				\
	c |= 0x80;			\
      *(ptr)++ = c;			\
    }					\
  while (valv);				\
})

static uint16_t (*do_read_16) (unsigned char *ptr);
static uint32_t (*do_read_24) (unsigned char *ptr);
static uint32_t (*do_read_32) (unsigned char *ptr);
static void (*do_write_16) (unsigned char *ptr, uint16_t val);
static void (*do_write_32) (unsigned char *ptr, uint32_t val);

static inline uint16_t
buf_read_ule16 (unsigned char *data)
{
  return data[0] | (data[1] << 8);
}

static inline uint16_t
buf_read_ube16 (unsigned char *data)
{
  return data[1] | (data[0] << 8);
}

static inline uint32_t
buf_read_ule24 (unsigned char *data)
{
  return data[0] | (data[1] << 8) | (data[2] << 16);
}

static inline uint32_t
buf_read_ube24 (unsigned char *data)
{
  return data[2] | (data[1] << 8) | (data[0] << 16);
}

static inline uint32_t
buf_read_ule32 (unsigned char *data)
{
  return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

static inline uint32_t
buf_read_ube32 (unsigned char *data)
{
  return data[3] | (data[2] << 8) | (data[1] << 16) | (data[0] << 24);
}

static const char *
strptr (DSO *dso, size_t sec, size_t offset)
{
  return elf_strptr (dso->elf, sec, offset);
}


#define read_8(ptr) *(ptr)++

#define read_16(ptr) ({					\
  uint16_t ret = do_read_16 (ptr);			\
  ptr += 2;						\
  ret;							\
})

#define read_32(ptr) ({					\
  uint32_t ret = do_read_32 (ptr);			\
  ptr += 4;						\
  ret;							\
})

/* Used for do_write_32_relocated, which can only be called
   immediately following do_read_32_relocated.  */
REL *last_relptr;
REL *last_relend;
int last_reltype;
struct debug_section *last_sec;

static inline REL *
find_rel_for_ptr (unsigned char *xptr, struct debug_section *sec)
{
  REL *relptr = sec->relbuf;
  REL *relend = sec->relend;
  size_t l = 0, r = relend - relptr;
  while (l < r)
    {
      size_t m = (l + r) / 2;
      if (relptr[m].ptr < xptr)
	l = m + 1;
      else if (relptr[m].ptr > xptr)
	r = m;
      else
	return &relptr[m];
    }
  return relend;
}

#define do_read_32_relocated(xptr, xsec) ({		\
  uint32_t dret = do_read_32 (xptr);			\
  REL *relptr = xsec->relbuf;		\
  REL *relend = xsec->relend;		\
  int reltype = xsec->reltype;		\
  if (relptr)						\
    {							\
      relptr = find_rel_for_ptr (xptr, xsec);		\
      if (relptr < relend && relptr->ptr == (xptr))	\
	{						\
	  if (reltype == SHT_REL)			\
	    dret += relptr->addend;			\
	  else						\
	    dret = relptr->addend;			\
	}						\
    }							\
  last_relptr = relptr;					\
  last_relend = relend;					\
  last_reltype = reltype;				\
  last_sec = xsec;					\
  dret;							\
})

#define read_32_relocated(ptr,sec) ({			\
  uint32_t ret = do_read_32_relocated (ptr,sec);	\
  ptr += 4;						\
  ret;							\
})

static void
dwarf2_write_le16 (unsigned char *p, uint16_t v)
{
  p[0] = v;
  p[1] = v >> 8;
}

static void
dwarf2_write_le32 (unsigned char *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static void
dwarf2_write_be16 (unsigned char *p, uint16_t v)
{
  p[1] = v;
  p[0] = v >> 8;
}

static void
dwarf2_write_be32 (unsigned char *p, uint32_t v)
{
  p[3] = v;
  p[2] = v >> 8;
  p[1] = v >> 16;
  p[0] = v >> 24;
}

#define write_8(ptr,val) ({	\
  *ptr++ = (val);		\
})

#define write_16(ptr,val) ({	\
  do_write_16 (ptr,val);	\
  ptr += 2;			\
})

#define write_32(ptr,val) ({	\
  do_write_32 (ptr,val);	\
  ptr += 4;			\
})

/* relocated writes can only be called immediately after
   do_read_32_relocated.  ptr must be equal to relptr->ptr (or
   relend). Might just update the addend. So relocations need to be
   updated at the end.  */

#define do_write_32_relocated(ptr,val) ({		\
  if (last_relptr && last_relptr < last_relend		\
      && last_relptr->ptr == ptr)			\
    {							\
      if (last_reltype == SHT_REL)			\
	do_write_32 (ptr, val - last_relptr->addend);	\
      else						\
	{						\
	  last_relptr->addend = val;			\
	  last_sec->rel_updated = true;	\
	}						\
    }							\
  else							\
    do_write_32 (ptr,val);				\
})

#define write_32_relocated(ptr,val) ({ \
  do_write_32_relocated (ptr,val);     \
  ptr += 4;			       \
})

static int
rel_cmp (const void *a, const void *b)
{
  REL *rela = (REL *) a, *relb = (REL *) b;

  if (rela->ptr < relb->ptr)
    return -1;

  if (rela->ptr > relb->ptr)
    return 1;

  return 0;
}

/* Returns a malloced REL array, or NULL when there are no relocations
   for this section.  When there are relocations, will setup relend,
   as the last REL, and reltype, as SHT_REL or SHT_RELA.  */
static void
setup_relbuf (DSO *dso, debug_section *sec)
{
  int ndx, maxndx;
  GElf_Rel rel;
  GElf_Rela rela;
  GElf_Sym sym;
  GElf_Addr base = dso->shdr[sec->sec].sh_addr;
  Elf_Data *symdata = NULL;
  int rtype;
  REL *relbuf;
  REL *relend;
  Elf_Scn *scn;
  Elf_Data *data;
  int i = sec->relsec;

  /* No relocations, or did we do this already? */
  if (i == 0 || sec->relbuf != NULL)
    {
      last_relptr = NULL;
      last_relend = NULL;
      last_reltype = 0;
      last_sec = NULL;
      return;
    }

  scn = dso->scn[i];
  data = elf_getdata (scn, NULL);
  assert (data != NULL && data->d_buf != NULL);
  assert (elf_getdata (scn, data) == NULL);
  assert (data->d_off == 0);
  assert (data->d_size == dso->shdr[i].sh_size);
  maxndx = dso->shdr[i].sh_size / dso->shdr[i].sh_entsize;
  relbuf = malloc (maxndx * sizeof (REL));
  sec->reltype = dso->shdr[i].sh_type;
  if (relbuf == NULL)
    error (1, errno, "%s: Could not allocate memory", dso->filename);

  symdata = elf_getdata (dso->scn[dso->shdr[i].sh_link], NULL);
  assert (symdata != NULL && symdata->d_buf != NULL);
  assert (elf_getdata (dso->scn[dso->shdr[i].sh_link], symdata) == NULL);
  assert (symdata->d_off == 0);
  assert (symdata->d_size == dso->shdr[dso->shdr[i].sh_link].sh_size);

  for (ndx = 0, relend = relbuf; ndx < maxndx; ++ndx)
    {
      if (dso->shdr[i].sh_type == SHT_REL)
	{
	  gelf_getrel (data, ndx, &rel);
	  rela.r_offset = rel.r_offset;
	  rela.r_info = rel.r_info;
	  rela.r_addend = 0;
	}
      else
	gelf_getrela (data, ndx, &rela);
      gelf_getsym (symdata, ELF64_R_SYM (rela.r_info), &sym);
      /* Relocations against section symbols are uninteresting in REL.  */
      if (dso->shdr[i].sh_type == SHT_REL && sym.st_value == 0)
	continue;
      /* Only consider relocations against .debug_str,
	 .debug_str_offsets, .debug_line, .debug_line_str,
	 .debug_macro and .debug_abbrev.  */
      if (sym.st_shndx == 0 ||
	  (sym.st_shndx != debug_sections[DEBUG_STR].sec
	   && sym.st_shndx != debug_sections[DEBUG_STR_OFFSETS].sec
	   && sym.st_shndx != debug_sections[DEBUG_LINE].sec
	   && sym.st_shndx != debug_sections[DEBUG_LINE_STR].sec
	   && sym.st_shndx != debug_sections[DEBUG_MACRO].sec
	   && sym.st_shndx != debug_sections[DEBUG_ABBREV].sec))
	continue;
      rela.r_addend += sym.st_value;
      rtype = ELF64_R_TYPE (rela.r_info);
      switch (dso->ehdr.e_machine)
	{
	case EM_SPARC:
	case EM_SPARC32PLUS:
	case EM_SPARCV9:
	  if (rtype != R_SPARC_32 && rtype != R_SPARC_UA32)
	    goto fail;
	  break;
	case EM_386:
	  if (rtype != R_386_32)
	    goto fail;
	  break;
	case EM_PPC:
	case EM_PPC64:
	  if (rtype != R_PPC_ADDR32 && rtype != R_PPC_UADDR32)
	    goto fail;
	  break;
	case EM_S390:
	  if (rtype != R_390_32)
	    goto fail;
	  break;
	case EM_PARISC:
	  if (rtype != R_PARISC_DIR32)
	    goto fail;
	  break;
	case EM_IA_64:
	  if (rtype != R_IA64_SECREL32LSB)
	    goto fail;
	  break;
	case EM_X86_64:
	  if (rtype != R_X86_64_32)
	    goto fail;
	  break;
	case EM_ALPHA:
	  if (rtype != R_ALPHA_REFLONG)
	    goto fail;
	  break;
#if defined(EM_AARCH64) && defined(R_AARCH64_ABS32)
	case EM_AARCH64:
	  if (rtype != R_AARCH64_ABS32)
	    goto fail;
	  break;
#endif
	case EM_68K:
	  if (rtype != R_68K_32)
	    goto fail;
	  break;
#if defined(EM_RISCV) && defined(R_RISCV_32)
	case EM_RISCV:
	  if (rtype != R_RISCV_32)
	    goto fail;
	  break;
#endif
#if defined(EM_MCST_ELBRUS) && defined(R_E2K_32_ABS)
	case EM_MCST_ELBRUS:
	  if (rtype != R_E2K_32_ABS)
		  goto fail;
	  break;
#endif
#if defined(EM_LOONGARCH) && defined(R_LARCH_32)
  case EM_LOONGARCH:
    if (rtype != R_LARCH_32)
      goto fail;
    break;
#endif
#ifndef EM_AMDGPU
#define EM_AMDGPU 224
#endif
#ifndef R_AMDGPU_ABS32
#define R_AMDGPU_ABS32 6
#endif
	case EM_AMDGPU:
	  if (rtype != R_AMDGPU_ABS32)
	    goto fail;
	  break;
	default:
	fail:
	  error (1, 0, "%s: Unhandled relocation %d at [%d] for %s section",
		 dso->filename, rtype, ndx, sec->name);
	}
      relend->ptr = sec->data
	+ (rela.r_offset - base);
      relend->addend = rela.r_addend;
      relend->ndx = ndx;
      ++(relend);
    }
  if (relbuf == relend)
    {
      free (relbuf);
      relbuf = NULL;
      relend = NULL;
    }
  else
    qsort (relbuf, relend - relbuf, sizeof (REL), rel_cmp);

  sec->relbuf = relbuf;
  sec->relend = relend;
  last_relptr = NULL;
}

/* Updates SHT_RELA section associated with the given section based on
   the relbuf data. The relbuf data is freed at the end.  */
static void
update_rela_data (DSO *dso, struct debug_section *sec)
{
  if (! sec->rel_updated)
    {
      free (sec->relbuf);
      sec->relbuf = NULL;
      return;
    }

  Elf_Data *symdata;
  int relsec_ndx = sec->relsec;
  Elf_Data *data = elf_getdata (dso->scn[relsec_ndx], NULL);
  symdata = elf_getdata (dso->scn[dso->shdr[relsec_ndx].sh_link],
			 NULL);

  REL *relptr = sec->relbuf;
  REL *relend = sec->relend;
  while (relptr < relend)
    {
      GElf_Sym sym;
      GElf_Rela rela;
      int ndx = relptr->ndx;

      if (gelf_getrela (data, ndx, &rela) == NULL)
	error (1, 0, "Couldn't get relocation: %s",
	       elf_errmsg (-1));

      if (gelf_getsym (symdata, GELF_R_SYM (rela.r_info),
		       &sym) == NULL)
	error (1, 0, "Couldn't get symbol: %s", elf_errmsg (-1));

      rela.r_addend = relptr->addend - sym.st_value;

      if (gelf_update_rela (data, ndx, &rela) == 0)
	error (1, 0, "Couldn't update relocations: %s",
	       elf_errmsg (-1));

      ++relptr;
    }
  elf_flagdata (data, ELF_C_SET, ELF_F_DIRTY);

  free (sec->relbuf);
}

static inline uint32_t
do_read_uleb128 (unsigned char *ptr)
{
  unsigned char *uleb_ptr = ptr;
  return read_uleb128 (uleb_ptr);
}

static inline uint32_t
do_read_str_form_relocated (DSO *dso, uint32_t form, unsigned char *ptr,
			    struct debug_section *sec, struct CU *cu)
{
  uint32_t idx;
  switch (form)
    {
    case DW_FORM_strp:
    case DW_FORM_line_strp:
      return do_read_32_relocated (ptr, sec);

    case DW_FORM_strx1:
      idx = *ptr;
      break;
    case DW_FORM_strx2:
      idx = do_read_16 (ptr);
      break;
    case DW_FORM_strx3:
      idx = do_read_24 (ptr);
      break;
    case DW_FORM_strx4:
      idx = do_read_32 (ptr);
      break;
    case DW_FORM_strx:
      idx = do_read_uleb128 (ptr);
      break;
    default:
      error (1, 0, "Unhandled string form DW_FORM_0x%x", form);
      return -1;
    }

  unsigned char *str_off_ptr = debug_sections[DEBUG_STR_OFFSETS].data;
  str_off_ptr += cu->str_offsets_base;
  str_off_ptr += idx * 4;

  struct debug_section *str_offsets_sec = &debug_sections[DEBUG_STR_OFFSETS];
  setup_relbuf(dso, str_offsets_sec);

  uint32_t str_off = do_read_32_relocated (str_off_ptr, str_offsets_sec);

  return str_off;
}

struct abbrev_attr
  {
    unsigned int attr;
    unsigned int form;
  };

struct abbrev_tag
  {
    unsigned int entry;
    unsigned int tag;
    int nattr;
    struct abbrev_attr attr[0];
  };

static hashval_t
abbrev_hash (const void *p)
{
  struct abbrev_tag *t = (struct abbrev_tag *)p;

  return t->entry;
}

static int
abbrev_eq (const void *p, const void *q)
{
  struct abbrev_tag *t1 = (struct abbrev_tag *)p;
  struct abbrev_tag *t2 = (struct abbrev_tag *)q;

  return t1->entry == t2->entry;
}

static void
abbrev_del (void *p)
{
  free (p);
}

static htab_t
read_abbrev (DSO *dso, unsigned char *ptr)
{
  htab_t h = htab_try_create (50, abbrev_hash, abbrev_eq, abbrev_del);
  unsigned int attr, form;
  struct abbrev_tag *t;
  int size;
  void **slot;

  if (h == NULL)
    {
no_memory:
      error (0, ENOMEM, "%s: Could not read .debug_abbrev", dso->filename);
      if (h)
        htab_delete (h);
      return NULL;
    }

  while ((attr = read_uleb128 (ptr)) != 0)
    {
      size = 10;
      t = malloc (sizeof (*t) + size * sizeof (struct abbrev_attr));
      if (t == NULL)
        goto no_memory;
      t->entry = attr;
      t->nattr = 0;
      slot = htab_find_slot (h, t, INSERT);
      if (slot == NULL)
        {
	  free (t);
	  goto no_memory;
        }
      if (*slot != NULL)
	{
	  error (0, 0, "%s: Duplicate DWARF abbreviation %d", dso->filename,
		 t->entry);
	  free (t);
	  htab_delete (h);
	  return NULL;
	}
      t->tag = read_uleb128 (ptr);
      ++ptr; /* skip children flag.  */
      while ((attr = read_uleb128 (ptr)) != 0)
        {
	  if (t->nattr == size)
	    {
	      struct abbrev_tag *orig_t = t;
	      size += 10;
	      t = realloc (t, sizeof (*t) + size * sizeof (struct abbrev_attr));
	      if (t == NULL)
		{
		  free (orig_t);
		  goto no_memory;
		}
	    }
	  form = read_uleb128 (ptr);
	  if (form == 2
	      || (form > DW_FORM_flag_present
		  && !(form == DW_FORM_ref_sig8
		       || form == DW_FORM_data16
		       || form == DW_FORM_line_strp
		       || form == DW_FORM_implicit_const
		       || form == DW_FORM_addrx
		       || form == DW_FORM_loclistx
		       || form == DW_FORM_rnglistx
		       || form == DW_FORM_addrx1
		       || form == DW_FORM_addrx2
		       || form == DW_FORM_addrx3
		       || form == DW_FORM_addrx4
		       || form == DW_FORM_strx
		       || form == DW_FORM_strx1
		       || form == DW_FORM_strx2
		       || form == DW_FORM_strx3
		       || form == DW_FORM_strx4)))
	    {
	      error (0, 0, "%s: Unknown DWARF DW_FORM_0x%x", dso->filename,
		     form);
	      htab_delete (h);
	      return NULL;
	    }
	  if (form == DW_FORM_implicit_const)
	    {
	      /* It is SLEB128 but the value is dropped anyway.  */
	      read_uleb128 (ptr);
	    }

	  t->attr[t->nattr].attr = attr;
	  t->attr[t->nattr++].form = form;
        }
      if (read_uleb128 (ptr) != 0)
        {
	  error (0, 0, "%s: DWARF abbreviation does not end with 2 zeros",
		 dso->filename);
	  htab_delete (h);
	  return NULL;
        }
      *slot = t;
    }

  return h;
}

#define IS_DIR_SEPARATOR(c) ((c)=='/')

static char *
canonicalize_path (const char *s, char *d)
{
  char *rv = d;
  char *droot;

  if (IS_DIR_SEPARATOR (*s))
    {
      *d++ = *s++;
      if (IS_DIR_SEPARATOR (*s) && !IS_DIR_SEPARATOR (s[1]))
	{
	  /* Special case for "//foo" meaning a Posix namespace
	     escape.  */
	  *d++ = *s++;
	}
      while (IS_DIR_SEPARATOR (*s))
	s++;
    }
  droot = d;

  while (*s)
    {
      /* At this point, we're always at the beginning of a path
	 segment.  */

      if (s[0] == '.' && (s[1] == 0 || IS_DIR_SEPARATOR (s[1])))
	{
	  s++;
	  if (*s)
	    while (IS_DIR_SEPARATOR (*s))
	      ++s;
	}

      else if (s[0] == '.' && s[1] == '.'
	       && (s[2] == 0 || IS_DIR_SEPARATOR (s[2])))
	{
	  char *pre = d - 1; /* includes slash */
	  while (droot < pre && IS_DIR_SEPARATOR (*pre))
	    pre--;
	  if (droot <= pre && ! IS_DIR_SEPARATOR (*pre))
	    {
	      while (droot < pre && ! IS_DIR_SEPARATOR (*pre))
		pre--;
	      /* pre now points to the slash */
	      if (droot < pre)
		pre++;
	      if (pre + 3 == d && pre[0] == '.' && pre[1] == '.')
		{
		  *d++ = *s++;
		  *d++ = *s++;
		}
	      else
		{
		  d = pre;
		  s += 2;
		  if (*s)
		    while (IS_DIR_SEPARATOR (*s))
		      s++;
		}
	    }
	  else
	    {
	      *d++ = *s++;
	      *d++ = *s++;
	    }
	}
      else
	{
	  while (*s && ! IS_DIR_SEPARATOR (*s))
	    *d++ = *s++;
	}

      if (IS_DIR_SEPARATOR (*s))
	{
	  *d++ = *s++;
	  while (IS_DIR_SEPARATOR (*s))
	    s++;
	}
    }
  while (droot < d && IS_DIR_SEPARATOR (d[-1]))
    --d;
  if (d == rv)
    *d++ = '.';
  *d = 0;

  return rv;
}

/* Returns the rest of PATH if it starts with DIR_PREFIX, skipping any
   / path separators, or NULL if PATH doesn't start with
   DIR_PREFIX. Might return the empty string if PATH equals DIR_PREFIX
   (modulo trailing slashes). Never returns path starting with '/'.
   Note that DIR_PREFIX itself should NOT end with a '/'.  */
static const char *
skip_dir_prefix (const char *path, const char *dir_prefix)
{
  size_t prefix_len = strlen (dir_prefix);
  if (strncmp (path, dir_prefix, prefix_len) == 0)
    {
      path += prefix_len;
      /* Unless path == dir_prefix there should be at least one '/'
	 in the path (which we will skip).  Otherwise the path has
	 a different (longer) directory prefix.  */
      if (*path != '\0' && !IS_DIR_SEPARATOR (*path))
	return NULL;
      while (IS_DIR_SEPARATOR (path[0]))
	path++;
      return path;
    }

  return NULL;
}

/* Most strings will be in the existing debug string table. But to
   replace the base/dest directory prefix we need some new storage.
   Keep new strings somewhat close together for faster comparison and
   copying.  SIZE should be at least one (and includes space for the
   zero terminator). The returned pointer points to uninitialized
   data.  */
static char *
new_string_storage (struct strings *strings, size_t size)
{
  assert (size > 0);

  /* If the string is extra long just create a whole block for
     it. Normally strings are much smaller than STRMEMSIZE. */
  if (strings->last_block == NULL
      || size > STRMEMSIZE
      || strings->stridx > STRMEMSIZE
      || (STRMEMSIZE - strings->stridx) < size)
    {
      struct strmemblock *newblock = malloc (sizeof (struct strmemblock)
					     + MAX (STRMEMSIZE, size));
      if (newblock == NULL)
	return NULL;

      newblock->next = NULL;

      if (strings->blocks == NULL)
	strings->blocks = newblock;

      if (strings->last_block != NULL)
	strings->last_block->next = newblock;

      strings->last_block = newblock;
      strings->stridx = 0;
    }

  size_t stridx = strings->stridx;
  strings->stridx += size + 1;
  return &strings->last_block->memory[stridx];
}

static void
create_dummy_debugedit_stridxentry (DSO *dso)
{
  if (debugedit_stridxentry.entry != NULL)
    {
      fprintf (stderr, "debugedit: "
	       "create_dummy_debugedit_stridxentry called more than once.");
      exit (-1);
    }

  const char *dummy_name = "<debugedit>"; /* Kilroy was here  */
  const size_t dummy_size = strlen (dummy_name) + 1;

  Strent *strent;
  char* dummy_str = new_string_storage (&dso->debug_str, dummy_size);
  if (dummy_str == NULL)
    error (1, ENOMEM, "Couldn't allocate new string storage");
  memcpy (dummy_str, dummy_name, dummy_size);
  strent = strtab_add_len (dso->debug_str.str_tab, dummy_str, dummy_size);
  if (strent == NULL)
    error (1, ENOMEM, "Could not create new string table entry");

  debugedit_stridxentry.idx = (uint32_t) -1;
  debugedit_stridxentry.entry = strent;
}

/* Comparison function used for tsearch. */
static int
strent_compare (const void *a, const void *b)
{
  struct stridxentry *entry_a = (struct stridxentry *)a;
  struct stridxentry *entry_b = (struct stridxentry *)b;
  size_t idx_a = entry_a->idx;
  size_t idx_b = entry_b->idx;

  if (idx_a < idx_b)
    return -1;

  if (idx_a > idx_b)
    return 1;

  return 0;
}

/* Allocates and inserts a new entry for the old index if not yet
   seen.  Returns a stridxentry if the given index has not yet been
   seen and needs to be filled in with the associated string (either
   the original string or the replacement string). Returns NULL if the
   idx is already known. Use in phase 0 to add all strings seen. In
   phase 1 use string_find_entry instead to get existing entries. */
static struct stridxentry *
string_find_new_entry (struct strings *strings, size_t old_idx)
{
  /* Use next entry in the pool for lookup so we can use it directly
     if this is a new index. */
  struct stridxentry *entry;

  /* Keep entries close together to make key comparison fast. */
  if (strings->last_entries == NULL || strings->entryidx >= STRIDXENTRIES)
    {
      size_t entriessz = (sizeof (struct strentblock)
			  + (STRIDXENTRIES * sizeof (struct stridxentry)));
      struct strentblock *newentries = malloc (entriessz);
      if (newentries == NULL)
	error (1, errno, "Couldn't allocate new string entries block");
      else
	{
	  if (strings->entries == NULL)
	    strings->entries = newentries;

	  if (strings->last_entries != NULL)
	    strings->last_entries->next = newentries;

	  strings->last_entries = newentries;
	  strings->last_entries->next = NULL;
	  strings->entryidx = 0;
	}
    }

  entry = &strings->last_entries->entry[strings->entryidx];
  entry->idx = old_idx;
  struct stridxentry **tres = tsearch (entry, &strings->strent_root,
				       strent_compare);
  if (tres == NULL)
    error (1, ENOMEM, "Couldn't insert new strtab idx");
  else if (*tres == entry)
    {
      /* idx not yet seen, must add actual str.  */
      strings->entryidx++;
      return entry;
    }

  return NULL; /* We already know about this idx, entry already complete. */
}

static struct stridxentry *
string_find_entry (struct strings *strings, size_t old_idx, bool accept_missing)
{
  struct stridxentry **ret;
  struct stridxentry key;
  key.idx = old_idx;
  ret = tfind (&key, &strings->strent_root, strent_compare);
  if (accept_missing && ret == NULL)
    return &debugedit_stridxentry;
  assert (ret != NULL); /* Can only happen for a bad/non-existing old_idx. */
  return *ret;
}

/* Adds a string_idx_entry given an index into the old/existing string
   table. Should be used in phase 0. Does nothing if the index was
   already registered. Otherwise it checks the string associated with
   the index. If the old string doesn't start with base_dir an entry
   will be recorded for the index with the same string. Otherwise a
   string will be recorded where the base_dir prefix will be replaced
   by dest_dir. Returns true if this is a not yet seen index and there
   a replacement file string has been recorded for it, otherwise
   returns false.  */
static bool
record_file_string_entry_idx (bool line_strp, DSO *dso, size_t old_idx)
{
  struct strings *strings = line_strp ? &dso->debug_line_str : &dso->debug_str;
  bool ret = false;
  struct stridxentry *entry = string_find_new_entry (strings, old_idx);
  if (entry != NULL)
    {
      debug_section *sec = &debug_sections[line_strp
					   ? DEBUG_LINE_STR : DEBUG_STR];
      if (old_idx >= sec->size)
	error (1, 0, "Bad string pointer index %zd (%s)", old_idx, sec->name);

      Strent *strent;
      const char *old_str = (char *)sec->data + old_idx;
      const char *file = skip_dir_prefix (old_str, base_dir);
      if (file == NULL)
	{
	  /* Just record the existing string.  */
	  strent = strtab_add_len (strings->str_tab, old_str,
				   strlen (old_str) + 1);
	}
      else
	{
	  /* Create and record the altered file path. */
	  size_t dest_len = strlen (dest_dir);
	  size_t file_len = strlen (file);
	  size_t nsize = dest_len + 1; /* + '\0' */
	  if (file_len > 0)
	    nsize += 1 + file_len;     /* + '/' */
	  char *nname = new_string_storage (strings, nsize);
	  if (nname == NULL)
	    error (1, ENOMEM, "Couldn't allocate new string storage");
	  memcpy (nname, dest_dir, dest_len);
	  if (file_len > 0)
	    {
	      nname[dest_len] = '/';
	      memcpy (nname + dest_len + 1, file, file_len + 1);
	    }
	  else
	    nname[dest_len] = '\0';

	  strent = strtab_add_len (strings->str_tab, nname, nsize);
	  ret = true;
	}
      if (strent == NULL)
	error (1, ENOMEM, "Could not create new string table entry");
      else
	entry->entry = strent;
    }

  return ret;
}

/* Same as record_new_string_file_string_entry_idx but doesn't replace
   base_dir with dest_dir, just records the existing string associated
   with the index. */
static void
record_existing_string_entry_idx (bool line_strp, DSO *dso, size_t old_idx)
{
  struct strings *strings = line_strp ? &dso->debug_line_str : &dso->debug_str;
  struct stridxentry *entry = string_find_new_entry (strings, old_idx);
  if (entry != NULL)
    {
      debug_section *sec = &debug_sections[line_strp
					   ? DEBUG_LINE_STR : DEBUG_STR];
      if (old_idx >= sec->size)
	error (1, 0, "Bad string pointer index %zd (%s)", old_idx, sec->name);

      const char *str = (char *)sec->data + old_idx;
      Strent *strent = strtab_add_len (strings->str_tab,
				       str, strlen (str) + 1);
      if (strent == NULL)
	error (1, ENOMEM, "Could not create new string table entry");
      else
	entry->entry = strent;
    }
}

static void
setup_strings (struct strings *strings)
{
  strings->str_tab = strtab_init (false);
  /* call update_strings to fill this in.  */
  memset (&strings->orig_data, 0, sizeof (strings->orig_data));
  strings->str_buf = NULL;
  strings->blocks = NULL;
  strings->last_block = NULL;
  strings->entries = NULL;
  strings->last_entries = NULL;
  strings->strent_root = NULL;
}

static void
update_strings (struct strings *strings, struct debug_section *sec)
{
  if (sec->elf_data != NULL)
    strings->orig_data = *sec->elf_data;
}

static const char *
orig_str (struct strings *strings, size_t idx)
{
  if (idx < strings->orig_data.d_size)
    return (const char *) strings->orig_data.d_buf + idx;
  return "<invalid>";
}

/* Noop for tdestroy. */
static void free_node (void *p __attribute__((__unused__))) { }

static void
destroy_strings (struct strings *strings)
{
  struct strmemblock *smb = strings->blocks;
  while (smb != NULL)
    {
      void *old = smb;
      smb = smb->next;
      free (old);
    }

  struct strentblock *emb = strings->entries;
  while (emb != NULL)
    {
      void *old = emb;
      emb = emb->next;
      free (old);
    }

  strtab_free (strings->str_tab);
  tdestroy (strings->strent_root, &free_node);
  free (strings->str_buf);
}

/* The minimum number of line tables we pre-allocate. */
#define MIN_LINE_TABLES 64

/* Gets a line_table at offset. Returns true if not yet know and
   successfully read, false otherwise.  Sets *table to NULL and
   outputs a warning if there was a problem reading the table at the
   given offset.  */
static bool
get_line_table (DSO *dso, size_t off, struct line_table **table, struct CU *cu)
{
  struct debug_lines *lines = &dso->lines;
  /* Assume there aren't that many, just do a linear search.  The
     array is probably already sorted because the stmt_lists are
     probably inserted in order. But we cannot rely on that (maybe we
     should check that to make searching quicker if possible?).  Once
     we have all line tables for phase 1 (rewriting) we do explicitly
     sort the array.*/
  for (int i = 0; i < lines->used; i++)
    if (lines->table[i].old_idx == off)
      {
	*table = &lines->table[i];
	return false;
      }

  if (lines->size == lines->used)
    {
      struct line_table *new_table = realloc (lines->table,
					      (sizeof (struct line_table)
					       * (lines->size
						  + MIN_LINE_TABLES)));
      if (new_table == NULL)
	{
	  error (0, ENOMEM, "Couldn't add more debug_line tables");
	  *table = NULL;
	  return false;
	}
      lines->table = new_table;
      lines->size += MIN_LINE_TABLES;
    }

  struct line_table *t = &lines->table[lines->used];
  *table = NULL;

  t->cu = cu;
  t->old_idx = off;
  t->new_idx = off;
  t->size_diff = 0;
  t->replace_dirs = false;
  t->replace_files = false;

  unsigned char *ptr = debug_sections[DEBUG_LINE].data;
  unsigned char *endsec = ptr + debug_sections[DEBUG_LINE].size;
  if (ptr == NULL)
    {
      error (0, 0, "%s: No .line_table section", dso->filename);
      return false;
    }

  if (off > debug_sections[DEBUG_LINE].size)
    {
      error (0, 0, "%s: Invalid .line_table offset 0x%zx",
	     dso->filename, off);
      return false;
    }
  ptr += off;

  /* unit_length */
  unsigned char *endcu = ptr + 4;
  t->unit_length = read_32 (ptr);
  endcu += t->unit_length;
  if (endcu == ptr + 0xffffffff)
    {
      error (0, 0, "%s: 64-bit DWARF not supported", dso->filename);
      return false;
    }

  if (endcu > endsec)
    {
      error (0, 0, "%s: .debug_line CU does not fit into section",
	     dso->filename);
      return false;
    }

  /* version */
  t->version = read_16 (ptr);
  if (t->version != 2 && t->version != 3 && t->version != 4 && t->version != 5)
    {
      error (0, 0, "%s: DWARF version %d unhandled", dso->filename,
	     t->version);
      return false;
    }

  if (t->version >= 5)
    {
      /* address_size */
      assert (cu->ptr_size != 0);
      if (cu->ptr_size != read_8 (ptr))
	{
	  error (0, 0, "%s: .debug_line address size differs from .debug_info",
		 dso->filename);
	  return false;
	}

      /* segment_selector_size */
      (void) read_8 (ptr);
    }

  /* header_length */
  unsigned char *endprol = ptr + 4;
  t->header_length = read_32 (ptr);
  endprol += t->header_length;
  if (endprol > endcu)
    {
      error (0, 0, "%s: .debug_line CU prologue does not fit into CU",
	     dso->filename);
      return false;
    }

  /* min instr len */
  t->min_instr_len = *ptr++;

  /* max op per instr, if version >= 4 */
  if (t->version >= 4)
    t->max_op_per_instr = *ptr++;

  /* default is stmt */
  t->default_is_stmt = *ptr++;

  /* line base */
  t->line_base = (*(int8_t *)ptr++);

  /* line range */
  t->line_range = *ptr++;

  /* opcode base */
  t->opcode_base = *ptr++;

  if (ptr + t->opcode_base - 1 >= endcu)
    {
      error (0, 0, "%s: .debug_line opcode table does not fit into CU",
	     dso->filename);
      return false;
    }
  lines->used++;
  *table = t;
  return true;
}

static int dirty_elf;
static void
dirty_section (unsigned int sec)
{
  for (struct debug_section *secp = &debug_sections[sec]; secp != NULL;
       secp = secp->next)
    elf_flagdata (secp->elf_data, ELF_C_SET, ELF_F_DIRTY);
  dirty_elf = 1;
}

static int
line_table_cmp (const void *a, const void *b)
{
  struct line_table *ta = (struct line_table *) a;
  struct line_table *tb = (struct line_table *) b;

  if (ta->old_idx < tb->old_idx)
    return -1;

  if (ta->old_idx > tb->old_idx)
    return 1;

  return 0;
}


/* Called after phase zero (which records all adjustments needed for
   the line tables referenced from debug_info) and before phase one
   starts (phase one will adjust the .debug_line section stmt
   references using the updated data structures). */
static void
edit_dwarf2_line (DSO *dso)
{
  Elf_Data *linedata = debug_sections[DEBUG_LINE].elf_data;
  unsigned char *old_buf = linedata->d_buf;

  /* A nicer way to do this would be to set the original d_size to
     zero and add a new Elf_Data section to contain the new data.
     Out with the old. In with the new.

  int linendx = debug_sections[DEBUG_LINE].sec;
  Elf_Scn *linescn = dso->scn[linendx];
  linedata->d_size = 0;
  linedata = elf_newdata (linescn);

    But when we then (recompress) the section there is a bug in
    elfutils < 0.192 that causes the compression to fail/create bad
    compressed data. So we just reuse the existing linedata (possibly
    loosing track of the original d_buf, which will be overwritten).  */

  dso->lines.line_buf = malloc (dso->lines.debug_lines_len);
  if (dso->lines.line_buf == NULL)
    error (1, ENOMEM, "No memory for new .debug_line table (0x%zx bytes)",
	   dso->lines.debug_lines_len);

  linedata->d_size = dso->lines.debug_lines_len;
  linedata->d_buf = dso->lines.line_buf;
  debug_sections[DEBUG_LINE].data = linedata->d_buf;
  debug_sections[DEBUG_LINE].size = linedata->d_size;
  debug_sections[DEBUG_LINE].elf_data = linedata;

  /* Make sure the line tables are sorted on the old index. */
  qsort (dso->lines.table, dso->lines.used, sizeof (struct line_table),
	 line_table_cmp);

  unsigned char *ptr = linedata->d_buf;
  for (int ldx = 0; ldx < dso->lines.used; ldx++)
    {
      struct line_table *t = &dso->lines.table[ldx];
      unsigned char *optr = old_buf + t->old_idx;
      t->new_idx = ptr - (unsigned char *) linedata->d_buf;

      /* Just copy the whole table if nothing needs replacing. */
      if (! t->replace_dirs && ! t->replace_files)
	{
	  assert (t->size_diff == 0);
	  memcpy (ptr, optr, t->unit_length + 4);
	  ptr += t->unit_length + 4;
	  continue;
	}

      /* Header fields. */
      write_32 (ptr, t->unit_length + t->size_diff);
      write_16 (ptr, t->version);
      write_32 (ptr, t->header_length + t->size_diff);
      write_8 (ptr, t->min_instr_len);
      if (t->version >= 4)
	write_8 (ptr, t->max_op_per_instr);
      write_8 (ptr, t->default_is_stmt);
      write_8 (ptr, t->line_base);
      write_8 (ptr, t->line_range);
      write_8 (ptr, t->opcode_base);

      optr += (4 /* unit len */
	       + 2 /* version */
	       + 4 /* header len */
	       + 1 /* min instr len */
	       + (t->version >= 4) /* max op per instr, if version >= 4 */
	       + 1 /* default is stmt */
	       + 1 /* line base */
	       + 1 /* line range */
	       + 1); /* opcode base */

      /* opcode len table. */
      memcpy (ptr, optr, t->opcode_base - 1);
      optr += t->opcode_base - 1;
      ptr += t->opcode_base - 1;

      /* directory table. We need to find the end (start of file
	 table) anyway, so loop over all dirs, even if replace_dirs is
	 false. */
      while (*optr != 0)
	{
	  const char *dir = (const char *) optr;
	  const char *file_path = NULL;
	  if (t->replace_dirs)
	    {
	      file_path = skip_dir_prefix (dir, base_dir);
	      if (file_path != NULL)
		{
		  size_t dest_len = strlen (dest_dir);
		  size_t file_len = strlen (file_path);
		  memcpy (ptr, dest_dir, dest_len);
		  ptr += dest_len;
		  if (file_len > 0)
		    {
		      *ptr++ = '/';
		      memcpy (ptr, file_path, file_len);
		      ptr += file_len;
		    }
		  *ptr++ = '\0';
		}
	    }
	  if (file_path == NULL)
	    {
	      size_t dir_len = strlen (dir);
	      memcpy (ptr, dir, dir_len + 1);
	      ptr += dir_len + 1;
	    }

	  optr = (unsigned char *) strchr (dir, 0) + 1;
	}
      optr++;
      *ptr++ = '\0';

      /* file table */
      if (t->replace_files)
	{
	  while (*optr != 0)
	    {
	      const char *file = (const char *) optr;
	      const char *file_path = NULL;
	      if (t->replace_files)
		{
		  file_path = skip_dir_prefix (file, base_dir);
		  if (file_path != NULL)
		    {
		      size_t dest_len = strlen (dest_dir);
		      size_t file_len = strlen (file_path);
		      memcpy (ptr, dest_dir, dest_len);
		      ptr += dest_len;
		      if (file_len > 0)
			{
			  *ptr++ = '/';
			  memcpy (ptr, file_path, file_len);
			  ptr += file_len;
			}
		      *ptr++ = '\0';
		    }
		}
	      if (file_path == NULL)
		{
		  size_t file_len = strlen (file);
		  memcpy (ptr, file, file_len + 1);
		  ptr += file_len + 1;
		}

	      optr = (unsigned char *) strchr (file, 0) + 1;

	      /* dir idx, time, len */
	      uint32_t dir_idx = read_uleb128 (optr);
	      write_uleb128 (ptr, dir_idx);
	      uint32_t time = read_uleb128 (optr);
	      write_uleb128 (ptr, time);
	      uint32_t len = read_uleb128 (optr);
	      write_uleb128 (ptr, len);
	    }
	  optr++;
	  *ptr++ = '\0';
	}

      /* line number program (and file table if not copied above). */
      size_t remaining = (t->unit_length + 4
			  - (optr - (old_buf + t->old_idx)));
      memcpy (ptr, optr, remaining);
      ptr += remaining;
    }
  elf_flagdata (linedata, ELF_C_SET, ELF_F_DIRTY);
}

/* Record or adjust (according to phase) DW_FORM_strp or DW_FORM_line_strp.
   Also handles DW_FORM_strx, but just for recording the (indexed) string.  */
static void
edit_strp (DSO *dso, uint32_t form, unsigned char *ptr, int phase,
	   bool handled_strp, struct debug_section *sec, struct CU *cu)
{
  unsigned char *ptr_orig = ptr;

  /* In the first pass we collect all strings, in the
     second we put the new references back (if there are
     any changes).  */
  if (phase == 0)
    {
      /* handled_strp is set for attributes referring to
	 files. If it is set the string is already
	 recorded. */
      if (! handled_strp)
	{
	  size_t idx = do_read_str_form_relocated (dso, form, ptr, sec, cu);
	  record_existing_string_entry_idx (form == DW_FORM_line_strp,
					    dso, idx);
	}
    }
  else if ((form == DW_FORM_strp
	    || form == DW_FORM_line_strp) /* DW_FORM_strx stays the same.  */
	   && (form == DW_FORM_line_strp
	       ? need_line_strp_update : need_strp_update)) /* && phase == 1 */
    {
      struct stridxentry *entry;
      size_t idx, new_idx;
      struct strings *strings = (form == DW_FORM_line_strp
				 ? &dso->debug_line_str : &dso->debug_str);
      idx = do_read_32_relocated (ptr, sec);
      entry = string_find_entry (strings, idx, false);
      new_idx = strent_offset (entry->entry);
      do_write_32_relocated (ptr, new_idx);
    }

  assert (ptr == ptr_orig);
}

/* Adjust *PTRP after the current *FORMP, update *FORMP for FORM_INDIRECT.  */
static enum { FORM_OK, FORM_ERROR, FORM_INDIRECT }
skip_form (DSO *dso, uint32_t *formp, unsigned char **ptrp, struct CU *cu)
{
  size_t len = 0;

  switch (*formp)
    {
    case DW_FORM_ref_addr:
      if (cu->cu_version == 2)
	*ptrp += cu->ptr_size;
      else
	*ptrp += 4;
      break;
    case DW_FORM_flag_present:
    case DW_FORM_implicit_const:
      break;
    case DW_FORM_addr:
      *ptrp += cu->ptr_size;
      break;
    case DW_FORM_ref1:
    case DW_FORM_flag:
    case DW_FORM_data1:
    case DW_FORM_strx1:
    case DW_FORM_addrx1:
      ++*ptrp;
      break;
    case DW_FORM_ref2:
    case DW_FORM_data2:
    case DW_FORM_strx2:
    case DW_FORM_addrx2:
      *ptrp += 2;
      break;
    case DW_FORM_strx3:
    case DW_FORM_addrx3:
      *ptrp += 3;
      break;
    case DW_FORM_ref4:
    case DW_FORM_data4:
    case DW_FORM_strx4:
    case DW_FORM_addrx4:
    case DW_FORM_sec_offset:
      *ptrp += 4;
      break;
    case DW_FORM_ref8:
    case DW_FORM_data8:
    case DW_FORM_ref_sig8:
      *ptrp += 8;
      break;
    case DW_FORM_data16:
      *ptrp += 16;
      break;
    case DW_FORM_sdata:
    case DW_FORM_ref_udata:
    case DW_FORM_udata:
    case DW_FORM_strx:
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
    case DW_FORM_addrx:
      read_uleb128 (*ptrp);
      break;
    case DW_FORM_strp:
    case DW_FORM_line_strp:
      *ptrp += 4;
      break;
    case DW_FORM_string:
      *ptrp = (unsigned char *) strchr ((char *)*ptrp, '\0') + 1;
      break;
    case DW_FORM_indirect:
      *formp = read_uleb128 (*ptrp);
      return FORM_INDIRECT;
    case DW_FORM_block1:
      len = *(*ptrp)++;
      break;
    case DW_FORM_block2:
      len = read_16 (*ptrp);
      *formp = DW_FORM_block1;
      break;
    case DW_FORM_block4:
      len = read_32 (*ptrp);
      *formp = DW_FORM_block1;
      break;
    case DW_FORM_block:
    case DW_FORM_exprloc:
      len = read_uleb128 (*ptrp);
      *formp = DW_FORM_block1;
      assert (len < UINT_MAX);
      break;
    default:
      error (0, 0, "%s: Unknown DWARF DW_FORM_0x%x", dso->filename, *formp);
      return FORM_ERROR;
    }

  if (*formp == DW_FORM_block1)
    *ptrp += len;

  return FORM_OK;
}

/* Part of read_dwarf2_line processing DWARF-4.  */
static bool
read_dwarf4_line (DSO *dso, unsigned char *ptr, char *comp_dir,
		  struct line_table *table)
{
  unsigned char **dirt;
  uint32_t value, dirt_cnt;
  size_t comp_dir_len = !comp_dir ? 0 : strlen (comp_dir);
  unsigned char *dir = ptr;

  /* dir table: */
  value = 1;
  while (*ptr != 0)
    {
      if (base_dir && dest_dir)
	{
	  /* Do we need to replace any of the dirs? Calculate new size. */
	  const char *file_path = skip_dir_prefix ((const char *)ptr,
						   base_dir);
	  if (file_path != NULL)
	    {
	      size_t old_size = strlen ((const char *)ptr) + 1;
	      size_t file_len = strlen (file_path);
	      size_t new_size = strlen (dest_dir) + 1;
	      if (file_len > 0)
		new_size += 1 + file_len;
	      table->size_diff += (new_size - old_size);
	      table->replace_dirs = true;
	    }
	}

      ptr = (unsigned char *) strchr ((char *)ptr, 0) + 1;
      ++value;
    }

  dirt = (unsigned char **) alloca (value * sizeof (unsigned char *));
  dirt[0] = (unsigned char *) ".";
  dirt_cnt = 1;
  ptr = dir;
  while (*ptr != 0)
    {
      dirt[dirt_cnt++] = ptr;
      ptr = (unsigned char *) strchr ((char *)ptr, 0) + 1;
    }
  ptr++;

  /* file table: */
  while (*ptr != 0)
    {
      char *s, *file;
      size_t file_len, dir_len;

      file = (char *) ptr;
      ptr = (unsigned char *) strchr ((char *)ptr, 0) + 1;
      value = read_uleb128 (ptr);

      if (value >= dirt_cnt)
	{
	  error (0, 0, "%s: Wrong directory table index %u",
		 dso->filename, value);
	  return false;
	}
      file_len = strlen (file);
      if (base_dir && dest_dir)
	{
	  /* Do we need to replace any of the files? Calculate new size. */
	  const char *file_path = skip_dir_prefix (file, base_dir);
	  if (file_path != NULL)
	    {
	      size_t old_size = file_len + 1;
	      size_t file_len = strlen (file_path);
	      size_t new_size = strlen (dest_dir) + 1;
	      if (file_len > 0)
		new_size += 1 + file_len;
	      table->size_diff += (new_size - old_size);
	      table->replace_files = true;
	    }
	}
      dir_len = strlen ((char *)dirt[value]);
      s = malloc (comp_dir_len + 1 + file_len + 1 + dir_len + 1);
      if (s == NULL)
	{
	  error (0, ENOMEM, "%s: Reading file table", dso->filename);
	  return false;
	}
      if (*file == '/')
	{
	  memcpy (s, file, file_len + 1);
	}
      else if (*dirt[value] == '/')
	{
	  memcpy (s, dirt[value], dir_len);
	  s[dir_len] = '/';
	  memcpy (s + dir_len + 1, file, file_len + 1);
	}
      else
	{
	  char *p = s;
	  if (comp_dir_len != 0)
	    {
	      memcpy (s, comp_dir, comp_dir_len);
	      s[comp_dir_len] = '/';
	      p += comp_dir_len + 1;
	    }
	  memcpy (p, dirt[value], dir_len);
	  p[dir_len] = '/';
	  memcpy (p + dir_len + 1, file, file_len + 1);
	}
      canonicalize_path (s, s);
      if (list_file_fd != -1)
	{
	  const char *p = NULL;
	  if (base_dir == NULL)
	    p = s;
	  else
	    {
	      p = skip_dir_prefix (s, base_dir);
	      if (p == NULL && dest_dir != NULL)
		p = skip_dir_prefix (s, dest_dir);
	    }

	  if (p)
	    {
	      size_t size = strlen (p) + 1;
	      while (size > 0)
		{
		  ssize_t ret = write (list_file_fd, p, size);
		  if (ret == -1)
		    error (1, errno, "Could not write to '%s'", list_file);
		  size -= ret;
		  p += ret;
		}
	    }
	}

      free (s);

      read_uleb128 (ptr);
      read_uleb128 (ptr);
    }

  return true;
}

/* Called by read_dwarf5_line first for directories and then file
   names as they both have the same format.  */
static bool
read_dwarf5_line_entries (DSO *dso, unsigned char **ptrp,
			  struct line_table *table, int phase,
			  char ***dirs, int *ndir,
			  const char *entry_name)
{
  /* directory_entry_format_count */
  /* file_name_entry_format_count */
  unsigned format_count = read_8 (*ptrp);

  unsigned char *formats = *ptrp;

  /* directory_entry_format */
  /* file_name_entry_format */
  for (unsigned formati = 0; formati < format_count; ++formati)
    {
      read_uleb128 (*ptrp);
      read_uleb128 (*ptrp);
    }

  /* directories_count */
  /* file_names_count */
  unsigned entry_count = read_uleb128 (*ptrp);

  bool collecting_dirs = phase == 0 && *dirs == NULL;
  bool writing_files = phase == 0 && *dirs != NULL;
  if (collecting_dirs)
    {
      *ndir = entry_count;
      *dirs = malloc (entry_count * sizeof (char *));
      if (*dirs == NULL)
	error (1, errno, "%s: Could not allocate debug_line dirs",
	       dso->filename);
    }

  /* directories */
  /* file_names */
  for (unsigned entryi = 0; entryi < entry_count; ++entryi)
    {
      char *dir = NULL;
      char *file = NULL;;
      unsigned char *format_ptr = formats;
      for (unsigned formati = 0; formati < format_count; ++formati)
	{
	  unsigned lnct = read_uleb128 (format_ptr);
	  unsigned form = read_uleb128 (format_ptr);
	  bool handled_form = false;
	  bool handled_strp = false;
	  bool line_strp = form == DW_FORM_line_strp;
	  if (lnct == DW_LNCT_path)
	    {
	      switch (form)
		{
		case DW_FORM_strp:
		case DW_FORM_line_strp:
		  if (phase == 0)
		    {
		      debug_section *debug_sec = &debug_sections[DEBUG_LINE];
		      size_t idx = do_read_32_relocated (*ptrp, debug_sec);
		      if (dest_dir)
			{
			  if (record_file_string_entry_idx (line_strp, dso,
							    idx))
			    {
			      if (line_strp)
				need_line_strp_update = true;
			      else
				need_strp_update = true;
			    }
			}
		      handled_strp = true;
		      if (collecting_dirs || writing_files)
			{
			  debug_section *sec = &debug_sections[line_strp
                                           ? DEBUG_LINE_STR : DEBUG_STR];
			  if (collecting_dirs)
			    dir = (char *)sec->data + idx;
			  if (writing_files)
			    file = (char *)sec->data + idx;
			}
		    }
		  break;
		default:
		  error (0, 0, "%s: Unsupported "
			 ".debug_line %s %u path DW_FORM_0x%x",
			 dso->filename, entry_name, entryi, form);
		  return false;
		}
	    }
	  if (writing_files && lnct == DW_LNCT_directory_index)
	    {
	      int dirndx;
	      switch (form)
		{
		case DW_FORM_udata:
		  handled_form = true;
		  dirndx = read_uleb128 (*ptrp);
		  break;
		case DW_FORM_data1:
		  dirndx = **ptrp;
		  break;
		case DW_FORM_data2:
		  dirndx = do_read_16 (*ptrp);
		  break;
		case DW_FORM_data4:
		  dirndx = do_read_32 (*ptrp);
		  break;
		default:
		  error (0, 0, "%s: Unsupported "
			 ".debug_line %s %u dirndx DW_FORM_0x%x",
			 dso->filename, entry_name, entryi, form);
		  return false;
		}

	      if (dirndx > *ndir)
		{
		  error (0, 0, "%s: Bad dir number %u in .debug_line %s",
			 dso->filename, entryi, entry_name);
		  return false;
		}
	      dir = (*dirs)[dirndx];
	    }

	  switch (form)
	    {
	    case DW_FORM_strp:
	    case DW_FORM_line_strp:
	    case DW_FORM_strx:
	    case DW_FORM_strx1:
	    case DW_FORM_strx2:
	    case DW_FORM_strx3:
	    case DW_FORM_strx4:
	      edit_strp (dso, form, *ptrp, phase, handled_strp,
			 &debug_sections[DEBUG_LINE], table->cu);
	      break;
	    }

	  if (!handled_form)
	    {
	      switch (skip_form (dso, &form, ptrp, table->cu))
		{
		case FORM_OK:
		  break;
		case FORM_ERROR:
		  return false;
		case FORM_INDIRECT:
		  error (0, 0, "%s: Unsupported "
			 ".debug_line %s %u DW_FORM_indirect",
			 dso->filename, entry_name, entryi);
		  return false;
		}
	    }
	}

      if (collecting_dirs)
	(*dirs)[entryi] = dir;

      if (writing_files)
	{
	  char *comp_dir = (*dirs)[0];
	  size_t comp_dir_len = !comp_dir ? 0 : strlen(comp_dir);
	  size_t file_len = strlen (file);
	  size_t dir_len = strlen (dir);

	  char *s = malloc (comp_dir_len + 1 + file_len + 1 + dir_len + 1);
	  if (s == NULL)
	    {
	      error (0, ENOMEM, "%s: Reading file table", dso->filename);
	      return false;
	    }
	  if (file[0] == '/')
	    {
	      memcpy (s, file, file_len + 1);
	    }
	  else if (dir[0] == '/')
	    {
	      memcpy (s, dir, dir_len);
	      s[dir_len] = '/';
	      memcpy (s + dir_len + 1, file, file_len + 1);
	    }
	  else
	    {
	      char *p = s;
	      if (comp_dir_len != 0)
		{
		  memcpy (s, comp_dir, comp_dir_len);
		  s[comp_dir_len] = '/';
		  p += comp_dir_len + 1;
		}
	      memcpy (p, dir, dir_len);
	      p[dir_len] = '/';
	      memcpy (p + dir_len + 1, file, file_len + 1);
	    }
	  canonicalize_path (s, s);
	  if (list_file_fd != -1)
	    {
	      const char *p = NULL;
	      if (base_dir == NULL)
		p = s;
	      else
		{
		  p = skip_dir_prefix (s, base_dir);
		  if (p == NULL && dest_dir != NULL)
		    p = skip_dir_prefix (s, dest_dir);
		}

	      if (p)
		{
		  size_t size = strlen (p) + 1;
		  while (size > 0)
		    {
		      ssize_t ret = write (list_file_fd, p, size);
		      if (ret == -1)
			error (1, errno, "Could not write to '%s'", list_file);
		      size -= ret;
		      p += ret;
		    }
		}
	    }

	  free (s);
	}
    }

  return true;
}

/* Part of read_dwarf2_line processing DWARF-5.  */
static bool
read_dwarf5_line (DSO *dso, unsigned char *ptr, struct line_table *table,
		  int phase)
{
  char **dirs = NULL;
  int ndir;
  /* Skip header.  */
  ptr += (4 /* unit len */
          + 2 /* version */
          + (table->version < 5 ? 0 : 0
             + 1 /* address_size */
             + 1 /* segment_selector*/)
          + 4 /* header len */
          + 1 /* min instr len */
          + (table->version >= 4) /* max op per instr, if version >= 4 */
          + 1 /* default is stmt */
          + 1 /* line base */
          + 1 /* line range */
          + 1 /* opcode base */
          + table->opcode_base - 1); /* opcode len table */

  bool retval = (read_dwarf5_line_entries (dso, &ptr, table, phase,
					   &dirs, &ndir, "directory")
		 && read_dwarf5_line_entries (dso, &ptr, table, phase,
					      &dirs, &ndir, "file name"));
  free (dirs);
  return retval;
}

/* Called during phase zero for each debug_line table referenced from
   .debug_info.  Outputs all source files seen and records any
   adjustments needed in the debug_list data structures. Returns true
   if line_table needs to be rewrite either the dir or file paths. */
static bool
read_dwarf2_line (DSO *dso, uint32_t off, char *comp_dir, struct CU *cu)
{
  unsigned char *ptr;
  struct line_table *table;

  if (get_line_table (dso, off, &table, cu) == false
      || table == NULL)
    return false;

  /* Skip to the directory table. The rest of the header has already
     been read and checked by get_line_table. */
  ptr = debug_sections[DEBUG_LINE].data + off;
  ptr += (4 /* unit len */
	  + 2 /* version */
	  + (table->version < 5 ? 0 : 0
	     + 1 /* address_size */
	     + 1 /* segment_selector*/)
	  + 4 /* header len */
	  + 1 /* min instr len */
	  + (table->version >= 4) /* max op per instr, if version >= 4 */
	  + 1 /* default is stmt */
	  + 1 /* line base */
	  + 1 /* line range */
	  + 1 /* opcode base */
	  + table->opcode_base - 1); /* opcode len table */

  /* DWARF version 5 line tables won't change size. But they might need
     [line]strp recording/updates. Handle that part later.  */
  if (table->version < 5)
    {
      if (! read_dwarf4_line (dso, ptr, comp_dir, table))
	return false;
    }

  dso->lines.debug_lines_len += 4 + table->unit_length + table->size_diff;
  return table->replace_dirs || table->replace_files;
}

/* Called during phase one, after the table has been sorted. */
static size_t
find_new_list_offs (struct debug_lines *lines, size_t idx)
{
  struct line_table key;
  key.old_idx = idx;
  struct line_table *table = bsearch (&key, lines->table,
				      lines->used,
				      sizeof (struct line_table),
				      line_table_cmp);
  return table->new_idx;
}

/* Read DW_FORM_strp or DW_FORM_line_strp collecting compilation directory.  */
static void
edit_attributes_str_comp_dir (uint32_t form, DSO *dso, unsigned char **ptrp,
			      int phase, char **comp_dirp, bool *handled_strpp,
			      struct debug_section *debug_sec, struct CU *cu)
{
  const char *dir;
  size_t idx = do_read_str_form_relocated (dso, form, *ptrp, debug_sec, cu);
  bool line_strp = form == DW_FORM_line_strp;
  /* In phase zero we collect the comp_dir.  */
  if (phase == 0)
    {
      debug_section *sec = &debug_sections[line_strp
					   ? DEBUG_LINE_STR : DEBUG_STR];
      if (sec->data == NULL || idx >= sec->size)
	error (1, 0, "%s: Bad string pointer index %zd for comp_dir (%s)",
	       dso->filename, idx, sec->name);
      dir = (char *) sec->data + idx;

      free (*comp_dirp);
      *comp_dirp = strdup (dir);
    }

  if (dest_dir != NULL && phase == 0)
    {
      if (record_file_string_entry_idx (line_strp, dso, idx))
	{
	  if (line_strp)
	    need_line_strp_update = true;
	  else
	    need_strp_update = true;
	}
      *handled_strpp = true;
    }
}

/* This scans the attributes of one DIE described by the given abbrev_tag.
   PTR points to the data in the debug_info. It will be advanced till all
   abbrev data is consumed. In phase zero data is collected, in phase one
   data might be replaced/updated.  */
static unsigned char *
edit_attributes (DSO *dso, unsigned char *ptr, struct abbrev_tag *t, int phase,
		 struct debug_section *debug_sec, struct CU *cu)
{
  int i;
  uint32_t list_offs;
  int found_list_offs;
  char *comp_dir;

  comp_dir = NULL;
  list_offs = 0;
  found_list_offs = 0;
  for (i = 0; i < t->nattr; ++i)
    {
      uint32_t form = t->attr[i].form;
      while (1)
	{
	  /* Whether we already handled a string as file for this
	     attribute.  If we did then we don't need to handle/record
	     it again when handling the DW_FORM_strp later. */
	  bool handled_strp = false;

	  /* A stmt_list points into the .debug_line section.  In
	     phase zero record all offsets. Then in phase one replace
	     them with the new offsets if we rewrote the line
	     tables.  */
	  if (t->attr[i].attr == DW_AT_stmt_list)
	    {
	      if (form == DW_FORM_data4
		  || form == DW_FORM_sec_offset)
		{
		  list_offs = do_read_32_relocated (ptr, debug_sec);
		  if (phase == 0)
		    found_list_offs = 1;
		  else if (need_stmt_update) /* phase one */
		    {
		      size_t idx, new_idx;
		      idx = do_read_32_relocated (ptr, debug_sec);
		      new_idx = find_new_list_offs (&dso->lines, idx);
		      do_write_32_relocated (ptr, new_idx);
		    }
		}
	    }

	  if (t->attr[i].attr == DW_AT_macros)
	    cu->macros_offs = do_read_32_relocated (ptr, debug_sec);

	  /* DW_AT_comp_dir is the current working directory. */
	  if (t->attr[i].attr == DW_AT_comp_dir)
	    {
	      if (form == DW_FORM_string)
		{
		  free (comp_dir);
		  comp_dir = strdup ((char *)ptr);

		  if (dest_dir)
		    {
		      /* In phase zero we are just collecting dir/file
			 names and check whether any need to be
			 adjusted. If so, in phase one we replace
			 those dir/files.  */
		      const char *file = skip_dir_prefix (comp_dir, base_dir);
		      if (file != NULL && phase == 0)
			need_string_replacement = true;
		      else if (file != NULL && phase == 1)
			{
			  size_t orig_len = strlen (comp_dir);
			  size_t dest_len = strlen (dest_dir);
			  size_t file_len = strlen (file);
			  size_t new_len = dest_len;
			  if (file_len > 0)
			    new_len += 1 + file_len; /* + '/' */

			  /* We don't want to rewrite the whole
			     debug_info section, so we only replace
			     the comp_dir with something equal or
			     smaller, possibly adding some slashes
			     at the end of the new compdir.  This
			     normally doesn't happen since most
			     producers will use DW_FORM_strp which is
			     more efficient.  */
			  if (orig_len < new_len)
			    error (0, 0, "Warning, not replacing comp_dir "
				   "'%s' prefix ('%s' -> '%s') encoded as "
				   "DW_FORM_string. "
				   "Replacement too large.",
				   comp_dir, base_dir, dest_dir);
			  else
			    {
			      /* Add zero (if no file part), one or more
				 slashes in between the new dest_dir and the
				 file name to fill up all space (replacement
				 DW_FORM_string must be of the same length).
				 We don't need to copy the old file name (if
				 any) or the zero terminator, because those
				 are already at the end of the string.  */
			      memcpy (ptr, dest_dir, dest_len);
			      memset (ptr + dest_len, '/',
				      orig_len - new_len);
			    }
			}
		    }
		}
	      else if (form == DW_FORM_strp
		       || form == DW_FORM_line_strp
		       || form == DW_FORM_strx
		       || form == DW_FORM_strx1
		       || form == DW_FORM_strx2
		       || form == DW_FORM_strx3
		       || form == DW_FORM_strx4)
		edit_attributes_str_comp_dir (form, dso,
					      &ptr, phase, &comp_dir,
					      &handled_strp, debug_sec, cu);
	    }
	  else if ((t->tag == DW_TAG_compile_unit
		    || t->tag == DW_TAG_partial_unit)
		   && ((form == DW_FORM_strp
			&& debug_sections[DEBUG_STR].data)
		       || (form == DW_FORM_line_strp
			   && debug_sections[DEBUG_LINE_STR].data)
		       || ((form == DW_FORM_strx
			    || form == DW_FORM_strx1
			    || form == DW_FORM_strx2
			    || form == DW_FORM_strx3
			    || form == DW_FORM_strx4)
			   && debug_sections[DEBUG_STR_OFFSETS].data))
		   && t->attr[i].attr == DW_AT_name)
	    {
	      bool line_strp = form == DW_FORM_line_strp;

	      /* DW_AT_name is the primary file for this compile
		 unit. If starting with / it is a full path name.
		 Note that we don't handle DW_FORM_string in this
		 case.  */
	      size_t idx = do_read_str_form_relocated (dso, form, ptr,
						       debug_sec, cu);

	      /* In phase zero we will look for a comp_dir to use.  */
	      if (phase == 0)
		{
		  debug_section *sec = &debug_sections[line_strp
						       ? DEBUG_LINE_STR
						       : DEBUG_STR];
		  if (idx >= sec->size)
		    error (1, 0,
			   "%s: Bad string pointer index %zd for unit name (%s)",
			   dso->filename, idx, sec->name);
		  char *name = (char *) sec->data + idx;
		  if (*name == '/' && comp_dir == NULL)
		    {
		      char *enddir = strrchr (name, '/');

		      if (enddir != name)
			{
			  comp_dir = malloc (enddir - name + 1);
			  memcpy (comp_dir, name, enddir - name);
			  comp_dir [enddir - name] = '\0';
			}
		      else
			comp_dir = strdup ("/");
		    }
		}

	      /* First pass (0) records the new name to be
		 added to the debug string pool, the second
		 pass (1) stores it (the new index). */
	      if (dest_dir && phase == 0)
		{
		  if (record_file_string_entry_idx (line_strp, dso, idx))
		    {
		      if (line_strp)
			need_line_strp_update = true;
		      else
			need_strp_update = true;
		    }
		  handled_strp = true;
		}
	    }

	  switch (form)
	    {
	    case DW_FORM_strp:
	    case DW_FORM_line_strp:
	    case DW_FORM_strx:
	    case DW_FORM_strx1:
	    case DW_FORM_strx2:
	    case DW_FORM_strx3:
	    case DW_FORM_strx4:
	      edit_strp (dso, form, ptr, phase, handled_strp, debug_sec, cu);
	      break;
	    }

	  switch (skip_form (dso, &form, &ptr, cu))
	    {
	    case FORM_OK:
	      break;
	    case FORM_ERROR:
	      return NULL;
	    case FORM_INDIRECT:
	      continue;
	    }

	  break;
	}
    }

  /* Ensure the CU current directory will exist even if only empty.  Source
     filenames possibly located in its parent directories refer relatively to
     it and the debugger (GDB) cannot safely optimize out the missing
     CU current dir subdirectories.  Only do this once in phase one. And
     only do this for dirs under our build/base_dir.  Don't output the
     empty string (in case the comp_dir == base_dir).  */
  if (phase == 0 && base_dir && comp_dir && list_file_fd != -1)
    {
      const char *p = skip_dir_prefix (comp_dir, base_dir);
      if (p != NULL && p[0] != '\0')
        {
	  size_t size = strlen (p);
	  while (size > 0)
	    {
	      ssize_t ret = write (list_file_fd, p, size);
	      if (ret == -1)
		error (1, errno, "Could not write to '%s'", list_file);
	      size -= ret;
	      p += ret;
	    }
	  /* Output trailing dir separator to distinguish them quickly from
	     regular files. */
	  if (size == 0)
	    {
	      ssize_t ret;
	      if (*(p - 1) != '/')
		ret = write (list_file_fd, "/", 2);
	      else
		ret = write (list_file_fd, "", 1);
	      if (ret == -1)
		error (1, errno, "Could not write to '%s'", list_file);
	    }
	}
    }

  /* In phase zero we collect all file names (we need the comp_dir for
     that).  Note that calculating the new size and offsets is done
     separately (at the end of phase zero after all CUs have been
     scanned in dwarf2_edit). */
  if (phase == 0 && found_list_offs
      && read_dwarf2_line (dso, list_offs, comp_dir, cu))
    need_stmt_update = true;

  free (comp_dir);

  return ptr;
}

static int
line_rel_cmp (const void *a, const void *b)
{
  LINE_REL *rela = (LINE_REL *) a, *relb = (LINE_REL *) b;

  if (rela->r_offset < relb->r_offset)
    return -1;

  if (rela->r_offset > relb->r_offset)
    return 1;

  return 0;
}

static int
edit_info (DSO *dso, int phase, struct debug_section *sec)
{
  unsigned char *ptr, *endcu, *endsec;
  uint32_t value;
  htab_t abbrev;
  struct abbrev_tag tag, *t;
  int i;
  bool first;
  struct CU *cu;

  ptr = sec->data;
  if (ptr == NULL)
    return 0;

  setup_relbuf(dso, sec);
  endsec = ptr + sec->size;
  while (ptr < endsec)
    {
      cu = malloc (sizeof (struct CU));
      if (cu == NULL)
	error (1, errno, "%s: Could not allocate memory for next CU",
	       dso->filename);

      cu->next = dso->cus;
      dso->cus = cu;

      unsigned char *cu_start = ptr;

      /* header size, version, unit_type, ptr_size.  */
      if (ptr + 4 + 2 + 1 + 1 > endsec)
	{
	  error (0, 0, "%s: %s CU header too small",
		 dso->filename, sec->name);
	  return 1;
	}

      endcu = ptr + 4;
      endcu += read_32 (ptr);
      if (endcu == ptr + 0xffffffff)
	{
	  error (0, 0, "%s: 64-bit DWARF not supported", dso->filename);
	  return 1;
	}

      if (endcu > endsec)
	{
	  error (0, 0, "%s: %s too small", dso->filename, sec->name);
	  return 1;
	}

      int cu_version = read_16 (ptr);
      if (cu_version != 2 && cu_version != 3 && cu_version != 4
	  && cu_version != 5)
	{
	  error (0, 0, "%s: DWARF version %d unhandled", dso->filename,
		 cu_version);
	  return 1;
	}
      cu->cu_version = cu_version;

      int cu_ptr_size = 0;

      uint8_t unit_type = DW_UT_compile;
      if (cu_version >= 5)
	{
	  unit_type = read_8 (ptr);
	  if (unit_type != DW_UT_compile
	      && unit_type != DW_UT_partial
	      && unit_type != DW_UT_type)
	    {
	      error (0, 0, "%s: Unit type %u unhandled", dso->filename,
		     unit_type);
	      return 1;
	    }

	  cu_ptr_size = read_8 (ptr);
	}

      unsigned char *header_end = (cu_start + 23
				   + (cu_version < 5
				      ? 0
				      : (unit_type != DW_UT_type
					 ? 1 /* unit */
					 : 1 + 8 + 4))); /* unit, id, off */
      if (header_end > endsec)
	{
	  error (0, 0, "%s: %s CU header too small", dso->filename, sec->name);
	  return 1;
	}

      value = read_32_relocated (ptr, sec);
      if (value >= debug_sections[DEBUG_ABBREV].size)
	{
	  if (debug_sections[DEBUG_ABBREV].data == NULL)
	    error (0, 0, "%s: .debug_abbrev not present", dso->filename);
	  else
	    error (0, 0, "%s: DWARF CU abbrev offset too large",
		   dso->filename);
	  return 1;
	}

      if (cu_version < 5)
	cu_ptr_size = read_8 (ptr);

      if (cu_ptr_size != 4 && cu_ptr_size != 8)
	{
	  error (0, 0, "%s: Invalid DWARF pointer size %d",
		 dso->filename, cu_ptr_size);
	  return 1;
	}

      cu->ptr_size = cu_ptr_size;

      if (sec != &debug_sections[DEBUG_INFO] || unit_type == DW_UT_type)
	ptr += 12; /* Skip type_signature and type_offset.  */

      abbrev = read_abbrev (dso,
			    debug_sections[DEBUG_ABBREV].data + value);
      if (abbrev == NULL)
	return 1;

      first = true;
      while (ptr < endcu)
	{
	  tag.entry = read_uleb128 (ptr);
	  if (tag.entry == 0)
	    continue;
	  t = htab_find_with_hash (abbrev, &tag, tag.entry);
	  if (t == NULL)
	    {
	      error (0, 0, "%s: Could not find DWARF abbreviation %d",
		     dso->filename, tag.entry);
	      htab_delete (abbrev);
	      return 1;
	    }

	  /* We need str_offsets_base before processing the CU. */
	  if (first)
	    {
	      first = false;
	      if (cu_version >= 5)
		{
		  uint32_t form;
		  unsigned char *fptr = ptr;
		  for (i = 0; i < t->nattr; ++i)
		    {
		      form = t->attr[i].form;
		      if (t->attr[i].attr == DW_AT_str_offsets_base)
			{
			  cu->str_offsets_base = do_read_32_relocated (fptr,
								       sec);
			  break;
			}
		      skip_form (dso, &form, &fptr, cu);
		    }
		}
	    }
	  ptr = edit_attributes (dso, ptr, t, phase, sec, cu);
	  if (ptr == NULL)
	    break;
	}

      htab_delete (abbrev);
    }

  return 0;
}

/* Rebuild .debug_str.  */
static void
edit_dwarf2_any_str (DSO *dso, struct strings *strings, debug_section *secp)
{
  Strtab *strtab = strings->str_tab;
  Elf_Data *strdata = secp->elf_data;

  /* A nicer way to do this would be to set the original d_size to
     zero and add a new Elf_Data section to contain the new data.
     Out with the old. In with the new.

  int strndx = secp->sec;
  Elf_Scn *strscn = dso->scn[strndx];
  strdata->d_size = 0;
  strdata = elf_newdata (strscn);

    But when we then (recompress) the section there is a bug in
    elfutils < 0.192 that causes the compression to fail/create bad
    compressed data. So we just reuse the existing strdata (possibly
    loosing track of the original d_buf, which will be overwritten).  */

  /* We really should check whether we had enough memory,
     but the old ebl version will just abort on out of
     memory... */
  strtab_finalize (strtab, strdata);
  secp->size = strdata->d_size;
  strings->str_buf = strdata->d_buf;
  elf_flagdata (strdata, ELF_C_SET, ELF_F_DIRTY);
}

/* Rebuild .debug_str_offsets.  */
static void
update_str_offsets (DSO *dso)
{
  struct debug_section *str_off_sec = &debug_sections[DEBUG_STR_OFFSETS];
  unsigned char *ptr = str_off_sec->data;
  unsigned char *endp = ptr + str_off_sec->size;

  while (ptr < endp)
    {
      /* Read header, unit_length, version and padding.  */
      unsigned char *index_start = ptr;
      if (endp - ptr < 3 * 4)
	break;
      uint32_t unit_length = read_32 (ptr);
      if (unit_length == 0xffffffff || endp - ptr < unit_length)
	break;
      unsigned char *endidxp = ptr + unit_length;
      uint32_t version = read_16 (ptr);
      if (version != 5)
	break;
      uint32_t padding = read_16 (ptr);
      if (padding != 0)
	break;
      unsigned char *offstart = ptr;

      while (ptr < endidxp)
	{
	  struct stridxentry *entry;
	  size_t idx, new_idx;
	  idx = do_read_32_relocated (ptr, str_off_sec);
	  entry = string_find_entry (&dso->debug_str, idx, true);
	  if (entry == &debugedit_stridxentry)
	    error (0, 0, "Warning, .debug_str_offsets table at offset %zx "
		   "index [%zd] .debug_str [%zx] entry '%s' unused, "
		   "replacing with '<debugedit>'\n",
		   (index_start - str_off_sec->data),
		   (ptr - offstart) / sizeof (uint32_t), idx,
		   orig_str (&dso->debug_str, idx));
	  new_idx = strent_offset (entry->entry);
	  write_32_relocated (ptr, new_idx);
	}
    }
}

static struct CU *
find_macro_cu (DSO *dso, uint32_t macros_offs)
{
  struct CU *cu = dso->cus;
  while (cu != NULL)
    {
      if (cu->macros_offs == macros_offs)
	return cu;
      cu = cu->next;
    }

  return dso->cus; /* Not found, assume first CU.  */
}

static int
edit_dwarf2 (DSO *dso)
{
  Elf_Data *data;
  Elf_Scn *scn;
  int i, j;

  for (i = 0; debug_sections[i].name; ++i)
    {
      debug_sections[i].data = NULL;
      debug_sections[i].size = 0;
      debug_sections[i].sec = 0;
      debug_sections[i].relsec = 0;
    }

  for (i = 1; i < dso->ehdr.e_shnum; ++i)
    if (! (dso->shdr[i].sh_flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR))
	&& dso->shdr[i].sh_size)
      {
        const char *name = strptr (dso, dso->ehdr.e_shstrndx,
				   dso->shdr[i].sh_name);

	if (name != NULL
	    && strncmp (name, ".debug_", sizeof (".debug_") - 1) == 0)
	  {
	    for (j = 0; debug_sections[j].name; ++j)
	      if (strcmp (name, debug_sections[j].name) == 0)
	 	{
		  struct debug_section *debug_sec = &debug_sections[j];
		  if (debug_sections[j].data)
		    {
		      if (j != DEBUG_MACRO && j != DEBUG_TYPES)
			{
			  error (0, 0, "%s: Found two copies of %s section",
				 dso->filename, name);
			  return 1;
			}
		      else
			{
			  /* In relocatable files .debug_macro and .debug_types
			     might appear multiple times as COMDAT section.  */
			  struct debug_section *sec;
			  sec = calloc (sizeof (struct debug_section), 1);
			  if (sec == NULL)
			    error (1, errno,
				   "%s: Could not allocate more %s sections",
				   dso->filename, name);
			  sec->name = name;

			  struct debug_section *multi_sec = debug_sec;
			  while (multi_sec->next != NULL)
			    multi_sec = multi_sec->next;

			  multi_sec->next = sec;
			  debug_sec = sec;
			}
		    }

		  scn = dso->scn[i];

		  /* Check for compressed DWARF headers. Records
		     ch_type so we can recompress headers after we
		     processed the data.  */
		  if (dso->shdr[i].sh_flags & SHF_COMPRESSED)
		    {
		      GElf_Chdr chdr;
		      if (gelf_getchdr(dso->scn[i], &chdr) == NULL)
			error (1, 0, "Couldn't get compressed header: %s",
			       elf_errmsg (-1));
		      debug_sec->ch_type = chdr.ch_type;
		      if (elf_compress (scn, 0, 0) < 0)
			error (1, 0, "Failed decompression");
		      gelf_getshdr (scn, &dso->shdr[i]);
		    }

		  data = elf_getdata (scn, NULL);
		  assert (data != NULL && data->d_buf != NULL);
		  assert (elf_getdata (scn, data) == NULL);
		  assert (data->d_off == 0);
		  assert (data->d_size == dso->shdr[i].sh_size);
		  debug_sec->data = data->d_buf;
		  debug_sec->elf_data = data;
		  debug_sec->size = data->d_size;
		  debug_sec->sec = i;
		  break;
		}

	    if (debug_sections[j].name == NULL)
	      {
		error (0, 0, "%s: Unknown debugging section %s",
		       dso->filename, name);
	      }
	  }
	else if (dso->ehdr.e_type == ET_REL
		 && ((dso->shdr[i].sh_type == SHT_REL
		      && name != NULL
		      && strncmp (name, ".rel.debug_",
				  sizeof (".rel.debug_") - 1) == 0)
		     || (dso->shdr[i].sh_type == SHT_RELA
			 && name != NULL
			 && strncmp (name, ".rela.debug_",
				     sizeof (".rela.debug_") - 1) == 0)))
	  {
	    for (j = 0; debug_sections[j].name; ++j)
	      if (strcmp (name + sizeof (".rel") - 1
			  + (dso->shdr[i].sh_type == SHT_RELA),
			  debug_sections[j].name) == 0)
	 	{
		  if (j == DEBUG_MACRO || j == DEBUG_TYPES)
		    {
		      /* Pick the correct one.  */
		      int rel_target = dso->shdr[i].sh_info;
		      struct debug_section *multi_sec = &debug_sections[j];
		      while (multi_sec != NULL)
			{
			  if (multi_sec->sec == rel_target)
			    {
			      multi_sec->relsec = i;
			      break;
			    }
			  multi_sec = multi_sec->next;
			}
		      if (multi_sec == NULL)
			error (0, 1, "No %s reloc section: %s",
			       debug_sections[j].name, dso->filename);
		    }
		  else
		    debug_sections[j].relsec = i;
		  break;
		}
	  }
      }

  update_strings (&dso->debug_str, &debug_sections[DEBUG_STR]);
  update_strings (&dso->debug_line_str, &debug_sections[DEBUG_LINE_STR]);

  if (dso->ehdr.e_ident[EI_DATA] == ELFDATA2LSB)
    {
      do_read_16 = buf_read_ule16;
      do_read_24 = buf_read_ule24;
      do_read_32 = buf_read_ule32;
      do_write_16 = dwarf2_write_le16;
      do_write_32 = dwarf2_write_le32;
    }
  else if (dso->ehdr.e_ident[EI_DATA] == ELFDATA2MSB)
    {
      do_read_16 = buf_read_ube16;
      do_read_24 = buf_read_ube24;
      do_read_32 = buf_read_ube32;
      do_write_16 = dwarf2_write_be16;
      do_write_32 = dwarf2_write_be32;
    }
  else
    {
      error (0, 0, "%s: Wrong ELF data enconding", dso->filename);
      return 1;
    }

  if (debug_sections[DEBUG_INFO].data == NULL)
    return 0;

  unsigned char *ptr, *endsec;
  int phase;
  for (phase = 0; phase < 2; phase++)
    {
      /* If we don't need to update anyhing, skip phase 1. */
      if (phase == 1
	  && !need_strp_update
	  && !need_line_strp_update
	  && !need_string_replacement
	  && !need_stmt_update)
	break;

      if (edit_info (dso, phase, &debug_sections[DEBUG_INFO]))
	return 1;

      struct debug_section *types_sec = &debug_sections[DEBUG_TYPES];
      while (types_sec != NULL)
	{
	  if (edit_info (dso, phase, types_sec))
	    return 1;
	  types_sec = types_sec->next;
	}

      /* We might have to recalculate/rewrite the debug_line
	 section.  We need to do that before going into phase one
	 so we have all new offsets.  We do this separately from
	 scanning the dirs/file names because the DW_AT_stmt_lists
	 might not be in order or skip some padding we might have
	 to (re)move. */
      if (phase == 0 && need_stmt_update)
	{
	  edit_dwarf2_line (dso);

	  /* The line table programs will be moved
	     forward/backwards a bit in the new data. Update the
	     debug_line relocations to the new offsets. */
	  int rndx = debug_sections[DEBUG_LINE].relsec;
	  if (rndx != 0)
	    {
	      LINE_REL *rbuf;
	      size_t rels;
	      Elf_Data *rdata = elf_getdata (dso->scn[rndx], NULL);
	      int rtype = dso->shdr[rndx].sh_type;
	      rels = dso->shdr[rndx].sh_size / dso->shdr[rndx].sh_entsize;
	      rbuf = malloc (rels * sizeof (LINE_REL));
	      if (rbuf == NULL)
		error (1, errno, "%s: Could not allocate line relocations",
		       dso->filename);

	      /* Sort them by offset into section. */
	      for (size_t i = 0; i < rels; i++)
		{
		  if (rtype == SHT_RELA)
		    {
		      GElf_Rela rela;
		      if (gelf_getrela (rdata, i, &rela) == NULL)
			error (1, 0, "Couldn't get relocation: %s",
			       elf_errmsg (-1));
		      rbuf[i].r_offset = rela.r_offset;
		      rbuf[i].ndx = i;
		    }
		  else
		    {
		      GElf_Rel rel;
		      if (gelf_getrel (rdata, i, &rel) == NULL)
			error (1, 0, "Couldn't get relocation: %s",
			       elf_errmsg (-1));
		      rbuf[i].r_offset = rel.r_offset;
		      rbuf[i].ndx = i;
		    }
		}
	      qsort (rbuf, rels, sizeof (LINE_REL), line_rel_cmp);

	      size_t lndx = 0;
	      for (size_t i = 0; i < rels; i++)
		{
		  /* These relocations only happen in ET_REL files
		     and are section offsets. */
		  GElf_Addr r_offset;
		  size_t ndx = rbuf[i].ndx;

		  GElf_Rel rel;
		  GElf_Rela rela;
		  if (rtype == SHT_RELA)
		    {
		      if (gelf_getrela (rdata, ndx, &rela) == NULL)
			error (1, 0, "Couldn't get relocation: %s",
			       elf_errmsg (-1));
		      r_offset = rela.r_offset;
		    }
		  else
		    {
		      if (gelf_getrel (rdata, ndx, &rel) == NULL)
			error (1, 0, "Couldn't get relocation: %s",
			       elf_errmsg (-1));
		      r_offset = rel.r_offset;
		    }

		  while (lndx < dso->lines.used
			 && r_offset > (dso->lines.table[lndx].old_idx
					+ 4
					+ dso->lines.table[lndx].unit_length))
		    lndx++;

		  if (lndx >= dso->lines.used)
		    error (1, 0,
			   ".debug_line relocation offset out of range");

		  /* Offset (pointing into the line program) moves
		     from old to new index including the header
		     size diff. */
		  r_offset += (ssize_t)((dso->lines.table[lndx].new_idx
					 - dso->lines.table[lndx].old_idx)
					+ dso->lines.table[lndx].size_diff);

		  if (rtype == SHT_RELA)
		    {
		      rela.r_offset = r_offset;
		      if (gelf_update_rela (rdata, ndx, &rela) == 0)
			error (1, 0, "Couldn't update relocation: %s",
			       elf_errmsg (-1));
		    }
		  else
		    {
		      rel.r_offset = r_offset;
		      if (gelf_update_rel (rdata, ndx, &rel) == 0)
			error (1, 0, "Couldn't update relocation: %s",
			       elf_errmsg (-1));
		    }
		}

	      elf_flagdata (rdata, ELF_C_SET, ELF_F_DIRTY);
	      free (rbuf);
	    }
	}

      /* The .debug_macro section also contains offsets into the
	 .debug_str section and references to the .debug_line
	 tables, so we need to update those as well if we update
	 the strings or the stmts.  */
      if ((need_strp_update || need_stmt_update)
	  && debug_sections[DEBUG_MACRO].data)
	{
	  /* There might be multiple (COMDAT) .debug_macro sections.  */
	  struct debug_section *macro_sec = &debug_sections[DEBUG_MACRO];
	  while (macro_sec != NULL)
	    {
	      setup_relbuf(dso, macro_sec);

	      ptr = macro_sec->data;
	      endsec = ptr + macro_sec->size;
	      int op = 0, macro_version, macro_flags;
	      int offset_len = 4, line_offset = 0;
	      struct CU *cu = NULL;

	      while (ptr < endsec)
		{
		  if (!op)
		    {
		      cu = find_macro_cu (dso, ptr - macro_sec->data);
		      macro_version = read_16 (ptr);
		      macro_flags = read_8 (ptr);
		      if (macro_version < 4 || macro_version > 5)
			error (1, 0, "unhandled .debug_macro version: %d",
			       macro_version);
		      if ((macro_flags & ~2) != 0)
			error (1, 0, "unhandled .debug_macro flags: 0x%x",
			       macro_flags);

		      offset_len = (macro_flags & 0x01) ? 8 : 4;
		      line_offset = (macro_flags & 0x02) ? 1 : 0;

		      if (offset_len != 4)
			error (0, 1,
			       "Cannot handle 8 byte macro offsets: %s",
			       dso->filename);

		      /* Update the line_offset if it is there.  */
		      if (line_offset)
			{
			  if (phase == 0)
			    ptr += offset_len;
			  else
			    {
			      size_t idx, new_idx;
			      idx = do_read_32_relocated (ptr, macro_sec);
			      new_idx = find_new_list_offs (&dso->lines,
							    idx);
			      write_32_relocated (ptr, new_idx);
			    }
			}
		    }

		  op = read_8 (ptr);
		  if (!op)
		    continue;
		  switch(op)
		    {
		    case DW_MACRO_GNU_define:
		    case DW_MACRO_GNU_undef:
		      read_uleb128 (ptr);
		      ptr = ((unsigned char *) strchr ((char *) ptr, '\0')
			     + 1);
		      break;
		    case DW_MACRO_GNU_start_file:
		      read_uleb128 (ptr);
		      read_uleb128 (ptr);
		      break;
		    case DW_MACRO_GNU_end_file:
		      break;
		    case DW_MACRO_GNU_define_indirect:
		    case DW_MACRO_GNU_undef_indirect:
		      read_uleb128 (ptr);
		      if (phase == 0)
			{
			  size_t idx = read_32_relocated (ptr, macro_sec);
			  record_existing_string_entry_idx (false, dso, idx);
			}
		      else
			{
			  struct stridxentry *entry;
			  size_t idx, new_idx;
			  idx = do_read_32_relocated (ptr, macro_sec);
			  entry = string_find_entry (&dso->debug_str, idx,
						     false);
			  new_idx = strent_offset (entry->entry);
			  write_32_relocated (ptr, new_idx);
			}
		      break;
		    case DW_MACRO_GNU_transparent_include:
		      ptr += offset_len;
		      break;
		    case DW_MACRO_define_strx:
		    case DW_MACRO_undef_strx:
		      read_uleb128 (ptr);
		      if (phase == 0)
			{
			  size_t idx;
			  idx = do_read_str_form_relocated (dso, DW_FORM_strx,
							    ptr, macro_sec,
							    cu);
			  record_existing_string_entry_idx (false, dso, idx);
			}
		      read_uleb128 (ptr);
		      break;
		    default:
		      error (1, 0, "Unhandled DW_MACRO op 0x%x", op);
		      break;
		    }
		}

	      macro_sec = macro_sec->next;
	    }
	}


      /* Now handle all the DWARF5 line tables, they contain strp
	 and/or line_strp entries that need to be registered/rewritten.  */
      setup_relbuf(dso, &debug_sections[DEBUG_LINE]);

      /* edit_dwarf2_line will have set this up, unless there are no
	 moved/resized (DWARF4) lines. In which case we can just use
	 the original section data. new_idx will have been setup
	 correctly, even if it is the same as old_idx.  */
      unsigned char *line_buf = (unsigned char *)dso->lines.line_buf;
      if (line_buf == NULL)
	line_buf = debug_sections[DEBUG_LINE].data;
      for (int ldx = 0; ldx < dso->lines.used; ldx++)
	{
	  struct line_table *t = &dso->lines.table[ldx];
	  if (t->version >= 5)
	    read_dwarf5_line (dso, line_buf + t->new_idx, t, phase);
	}

      /* Same for the debug_str and debug_line_str sections.
	 Make sure everything is in place for phase 1 updating of debug_info
	 references. */
      if (phase == 0 && need_strp_update)
	{
	  /* We might need a dummy .debug_str entry for
	     .debug_str_offsets entries of unused strings. We have to
	     add it unconditionally when there is a .debug_str_offsets
	     section because we don't know if there are any such
	     entries.  */
	  if (debug_sections[DEBUG_STR_OFFSETS].data != NULL)
	    create_dummy_debugedit_stridxentry (dso);
	  edit_dwarf2_any_str (dso, &dso->debug_str,
			       &debug_sections[DEBUG_STR]);
	}
      if (phase == 0 && need_line_strp_update)
	edit_dwarf2_any_str (dso, &dso->debug_line_str,
			     &debug_sections[DEBUG_LINE_STR]);
    }

  /* After phase 1 we might have rewritten the debug_info with
     new strp, strings and/or linep offsets.  */
  if (need_strp_update || need_line_strp_update
      || need_string_replacement || need_stmt_update) {
    dirty_section (DEBUG_INFO);
    if (debug_sections[DEBUG_TYPES].data != NULL)
      dirty_section (DEBUG_TYPES);
  }
  if (need_strp_update || need_stmt_update)
    dirty_section (DEBUG_MACRO);
  if (need_stmt_update || need_line_strp_update)
    dirty_section (DEBUG_LINE);
  if (need_strp_update && debug_sections[DEBUG_STR_OFFSETS].data != NULL)
    {
      setup_relbuf(dso, &debug_sections[DEBUG_STR_OFFSETS]);
      update_str_offsets (dso);
      dirty_section (DEBUG_STR_OFFSETS);
      update_rela_data (dso, &debug_sections[DEBUG_STR_OFFSETS]);
    }

  /* Update any relocations addends we might have touched. */
  update_rela_data (dso, &debug_sections[DEBUG_INFO]);

  struct debug_section *types_sec = &debug_sections[DEBUG_TYPES];
  while (types_sec != NULL)
    {
      update_rela_data (dso, types_sec);
      types_sec = types_sec->next;
    }

  struct debug_section *macro_sec = &debug_sections[DEBUG_MACRO];
  while (macro_sec != NULL)
    {
      update_rela_data (dso, macro_sec);
      macro_sec = macro_sec->next;
    }

  update_rela_data (dso, &debug_sections[DEBUG_LINE]);

  return 0;
}

static struct option optionsTable[] =
  {
    { "base-dir", required_argument, 0, 'b' },
    { "dest-dir", required_argument, 0, 'd' },
    { "list-file", required_argument, 0, 'l' },
    { "build-id", no_argument, 0, 'i' },
    { "build-id-seed", required_argument, 0, 's' },
    { "no-recompute-build-id", no_argument, 0, 'n' },
    { "preserve-dates", no_argument, 0, 'p' },
    { "version", no_argument, 0, 'V' },
    { "help", no_argument, 0, '?' },
    { "usage", no_argument, 0, 'u' },
    { NULL, 0, 0, 0 }
  };

static const char *optionsChars = "b:d:l:is:npV?u";

static const char *helpText =
  "Usage: %s [OPTION...] FILE\n"
  "  -b, --base-dir=STRING           base build directory of objects\n"
  "  -d, --dest-dir=STRING           directory to rewrite base-dir into\n"
  "  -l, --list-file=STRING          file where to put list of source and \n"
  "                                  header file names\n"
  "  -i, --build-id                  recompute build ID note and print ID on\n"
  "                                  stdout\n"
  "  -s, --build-id-seed=STRING      if recomputing the build ID note use\n"
  "                                  this string as hash seed\n"
  "  -n, --no-recompute-build-id     do not recompute build ID note even\n"
  "                                  when -i or -s are given\n"
  "  -p, --preserve-dates            Preserve modified/access timestamps\n"
  "\n"
  "Help options:\n"
  "  -?, --help                      Show this help message\n"
  "  -u, --usage                     Display brief usage message\n"
  "  -V, --version                   Show debugedit version\n";

static const char *usageText =
  "Usage: %s [-in?] [-b|--base-dir STRING] [-d|--dest-dir STRING]\n"
  "        [-l|--list-file STRING] [-i|--build-id] \n"
  "        [-s|--build-id-seed STRING]\n"
  "        [-n|--no-recompute-build-id]\n"
  "        [-p|--preserve-dates]\n"
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

static DSO *
fdopen_dso (int fd, const char *name)
{
  Elf *elf = NULL;
  GElf_Ehdr ehdr;
  int i;
  DSO *dso = NULL;
  size_t phnum;

  if (dest_dir == NULL && (!do_build_id || no_recompute_build_id))
    elf = elf_begin (fd, ELF_C_READ, NULL);
  else
    elf = elf_begin (fd, ELF_C_RDWR, NULL);
  if (elf == NULL)
    {
      error (0, 0, "cannot open ELF file: %s", elf_errmsg (-1));
      goto error_out;
    }

  if (elf_kind (elf) != ELF_K_ELF)
    {
      error (0, 0, "\"%s\" is not an ELF file", name);
      goto error_out;
    }

  if (gelf_getehdr (elf, &ehdr) == NULL)
    {
      error (0, 0, "cannot get the ELF header: %s",
	     elf_errmsg (-1));
      goto error_out;
    }

  if (ehdr.e_type != ET_DYN && ehdr.e_type != ET_EXEC && ehdr.e_type != ET_REL)
    {
      error (0, 0, "\"%s\" is not a shared library", name);
      goto error_out;
    }

  /* Allocate DSO structure. Leave place for additional 20 new section
     headers.  */
  dso = (DSO *)
	malloc (sizeof(DSO) + (ehdr.e_shnum + 20) * sizeof(GElf_Shdr)
	        + (ehdr.e_shnum + 20) * sizeof(Elf_Scn *));
  if (!dso)
    {
      error (0, ENOMEM, "Could not open DSO");
      goto error_out;
    }

  if (elf_getphdrnum (elf, &phnum) != 0)
    {
      error (0, 0, "Couldn't get number of phdrs: %s", elf_errmsg (-1));
      goto error_out;
    }

  /* If there are phdrs we want to maintain the layout of the
     allocated sections in the file.  */
  if (phnum != 0)
    elf_flagelf (elf, ELF_C_SET, ELF_F_LAYOUT);

  memset (dso, 0, sizeof(DSO));
  dso->elf = elf;
  dso->phnum = phnum;
  dso->ehdr = ehdr;
  dso->scn = (Elf_Scn **) &dso->shdr[ehdr.e_shnum + 20];

  for (i = 0; i < ehdr.e_shnum; ++i)
    {
      dso->scn[i] = elf_getscn (elf, i);
      gelf_getshdr (dso->scn[i], dso->shdr + i);
    }

  dso->filename = (const char *) strdup (name);
  setup_strings (&dso->debug_str);
  setup_strings (&dso->debug_line_str);
  setup_lines (&dso->lines);
  return dso;

error_out:
  if (dso)
    {
      free ((char *) dso->filename);
      destroy_strings (&dso->debug_str);
      destroy_strings (&dso->debug_line_str);
      destroy_lines (&dso->lines);
      destroy_cus (dso->cus);
      free (dso);
    }
  if (elf)
    elf_end (elf);
  if (fd != -1)
    close (fd);
  return NULL;
}

/* Compute a fresh build ID bit-string from the editted file contents.  */
static void
handle_build_id (DSO *dso, Elf_Data *build_id,
		 size_t build_id_offset, size_t build_id_size)
{
  /* Accept any build_id_size > 0.  Hashes will be truncated or padded
     to the incoming note size, as debugedit cannot change their
     size. */
  if (build_id_size <= 0)
    {
      error (1, 0, "Cannot handle %zu-byte build ID", build_id_size);
    }

  int i = -1;
  if (no_recompute_build_id
      || (! dirty_elf && build_id_seed == NULL))
    goto print;

  /* Clear the bits about to be recomputed, so they do not affect the
     new hash.  Extra bits left over from wider-than-128-bit hash are
     preserved for extra entropy.  This computation should be
     idempotent, so repeated rehashes (with the same seed) should
     result in the same hash. */
  XXH128_canonical_t result_canon;
  memset ((char *) build_id->d_buf + build_id_offset, 0,
          MIN (build_id_size, sizeof(result_canon)));

  XXH3_state_t* state = XXH3_createState();
  if (!state)
    error (1, errno, "Failed to create xxhash state");
  XXH3_128bits_reset (state);

  /* If a seed string was given use it to prime the hash.  */
  if (build_id_seed != NULL)
    /* Another choice is XXH3_generateSecret. */
    XXH3_128bits_update (state, build_id_seed, strlen (build_id_seed));

  /* Slurp the relevant header bits and section contents and feed them
     into the hash function.  The only bits we ignore are the offset
     fields in ehdr and shdrs, since the semantically identical ELF file
     could be written differently if it doesn't change the phdr layout.
     We always use the GElf (i.e. Elf64) formats for the bits to hash
     since it is convenient.  It doesn't matter whether this is an Elf32
     or Elf64 object, only that we are consistent in what bits feed the
     hash so it comes out the same for the same file contents.  */
  {
    union
    {
      GElf_Ehdr ehdr;
      GElf_Phdr phdr;
      GElf_Shdr shdr;
    } u;
    Elf_Data x = { .d_version = EV_CURRENT, .d_buf = &u };

    x.d_type = ELF_T_EHDR;
    x.d_size = sizeof u.ehdr;
    u.ehdr = dso->ehdr;
    u.ehdr.e_phoff = u.ehdr.e_shoff = 0;
    if (elf64_xlatetom (&x, &x, dso->ehdr.e_ident[EI_DATA]) == NULL)
      {
      bad:
	error (1, 0, "Failed to compute header checksum: %s",
	       elf_errmsg (elf_errno ()));
      }

    x.d_type = ELF_T_PHDR;
    x.d_size = sizeof u.phdr;
    for (i = 0; i < dso->ehdr.e_phnum; ++i)
      {
	if (gelf_getphdr (dso->elf, i, &u.phdr) == NULL)
	  goto bad;
	if (elf64_xlatetom (&x, &x, dso->ehdr.e_ident[EI_DATA]) == NULL)
	  goto bad;

        XXH3_128bits_update (state, x.d_buf, x.d_size);
      }

    x.d_type = ELF_T_SHDR;
    x.d_size = sizeof u.shdr;
    for (i = 0; i < dso->ehdr.e_shnum; ++i)
      if (dso->scn[i] != NULL)
	{
	  u.shdr = dso->shdr[i];
	  u.shdr.sh_offset = 0;
	  if (elf64_xlatetom (&x, &x, dso->ehdr.e_ident[EI_DATA]) == NULL)
	    goto bad;

          XXH3_128bits_update (state, x.d_buf, x.d_size);

	  if (dso->shdr[i].sh_type != SHT_NOBITS)
	    {
	      Elf_Data *d = elf_getdata (dso->scn[i], NULL);
	      if (d == NULL)
		goto bad;

              XXH3_128bits_update (state, d->d_buf, d->d_size);
	    }
	}
  }

  XXH128_hash_t result = XXH3_128bits_digest (state);
  XXH3_freeState (state);
  /* Use canonical-endianness output. */
  XXH128_canonicalFromHash (&result_canon, result);
  memcpy((unsigned char *)build_id->d_buf + build_id_offset, &result_canon,
         MIN (build_id_size, sizeof(result_canon)));

  elf_flagdata (build_id, ELF_C_SET, ELF_F_DIRTY);

 print:
  /* Now format the build ID bits in hex to print out.  */
  {
    const uint8_t * id = (uint8_t *)build_id->d_buf + build_id_offset;
    size_t blen = build_id_size;
    static char const hex[] = "0123456789abcdef";
    while (blen-- > 0)
      {
	size_t i = *id++;
	printf ("%c%c", hex[(i >> 4) & 0xf], hex[(i) & 0xf]);
      }
    printf ("\n");
  }
}

int
main (int argc, char *argv[])
{
  DSO *dso;
  int fd, i;
  const char *file;
  struct stat stat_buf;
  Elf_Data *build_id = NULL;
  size_t build_id_offset = 0, build_id_size = 0;

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

	case 'b':
	  base_dir = optarg;
	  break;

	case 'd':
	  dest_dir = optarg;
	  break;

	case 'l':
	  list_file = optarg;
	  break;

	case 'i':
	  do_build_id = 1;
	  break;

	case 's':
	  build_id_seed = optarg;
	  break;

	case 'n':
	  no_recompute_build_id = 1;
	  break;

	case 'p':
	  preserve_dates = true;
	  break;

	case 'V':
	  show_version = 1;
	  break;
	}
    }

  if (show_version)
    {
      printf("debugedit %s\n", VERSION);
      exit(EXIT_SUCCESS);
    }

  if (optind != argc - 1)
    {
      error (0, 0, "Need one FILE as input");
      usage (argv[0], true);
    }

  if (dest_dir != NULL)
    {
      if (base_dir == NULL)
	{
	  error (1, 0, "You must specify a base dir if you specify a dest dir");
	}
    }

  if (build_id_seed != NULL && do_build_id == 0)
    {
      error (1, 0, "--build-id-seed (-s) needs --build-id (-i)");
    }

  if (build_id_seed != NULL && strlen (build_id_seed) < 1)
    {
      error (1, 0, "--build-id-seed (-s) string should be at least 1 char");
    }

  /* Ensure clean paths, users can muck with these. Also removes any
     trailing '/' from the paths. */
  if (base_dir)
    canonicalize_path(base_dir, base_dir);
  if (dest_dir)
    canonicalize_path(dest_dir, dest_dir);

  if (list_file != NULL)
    {
      list_file_fd = open (list_file, O_WRONLY|O_CREAT|O_APPEND, 0644);
    }

  file = argv[optind];

  if (elf_version(EV_CURRENT) == EV_NONE)
    {
      error (1, 0, "library out of date");
    }

  if (stat(file, &stat_buf) < 0)
    {
      error (1, errno, "Failed to open input file '%s'", file);
    }

  /* Make sure we can read and write */
  if (chmod (file, stat_buf.st_mode | S_IRUSR | S_IWUSR) != 0)
    error (0, errno, "Failed to chmod input file '%s' to make sure we can read and write", file);

  if (dest_dir == NULL && (!do_build_id || no_recompute_build_id))
    fd = open (file, O_RDONLY);
  else
    fd = open (file, O_RDWR);
  if (fd < 0)
    {
      error (1, errno, "Failed to open input file '%s'", file);
    }

  dso = fdopen_dso (fd, file);
  if (dso == NULL)
    exit (1);

  for (i = 1; i < dso->ehdr.e_shnum; i++)
    {
      const char *name;

      switch (dso->shdr[i].sh_type)
	{
	case SHT_MIPS_DWARF:
	  /* According to the specification, all MIPS .debug_* sections are
	     marked with ELF type SHT_MIPS_DWARF. As SHT_MIPS_DWARF is from
	     processor-specific range, we have to check that we're actually
	     dealing with MIPS ELF file before handling such sections.  */
	  if (dso->ehdr.e_machine != EM_MIPS
	      && dso->ehdr.e_machine != EM_MIPS_RS3_LE) {
	    break;
	  }
	  /*@fallthrough@*/
	case SHT_PROGBITS:
	  name = strptr (dso, dso->ehdr.e_shstrndx, dso->shdr[i].sh_name);
	  /* TODO: Handle stabs */
	  if (name != NULL && strcmp (name, ".stab") == 0)
	    {
	      error (0, 0, "Stabs debuginfo not supported: %s", file);
	      break;
	    }
	  /* We only have to go over the DIE tree if we are rewriting paths
	     or listing sources.  */
	  if ((base_dir != NULL || dest_dir != NULL || list_file_fd != -1)
	      && name != NULL && strcmp (name, ".debug_info") == 0)
	    edit_dwarf2 (dso);

	  break;
	case SHT_NOTE:
	  if (do_build_id
	      && build_id == 0 && (dso->shdr[i].sh_flags & SHF_ALLOC))
	    {
	      /* Look for a build-ID note here.  */
	      size_t off = 0;
	      GElf_Nhdr nhdr;
	      size_t name_off;
	      size_t desc_off;
	      Elf_Data *data = elf_getdata (elf_getscn (dso->elf, i), NULL);
	      while ((off = gelf_getnote (data, off,
					  &nhdr, &name_off, &desc_off)) > 0)
		if (nhdr.n_type == NT_GNU_BUILD_ID
		    && nhdr.n_namesz == sizeof "GNU"
		    && (memcmp ((char *)data->d_buf + name_off, "GNU",
				sizeof "GNU") == 0))
		  {
		    build_id = data;
		    build_id_offset = desc_off;
		    build_id_size = nhdr.n_descsz;
		  }
	    }
	  break;
	default:
	  break;
	}
    }

  /* Recompress any debug sections that might have been uncompressed.  */
  if (dirty_elf)
    for (int s = 0; debug_sections[s].name; s++)
      {
	for (struct debug_section *secp = &debug_sections[s]; secp != NULL;
	     secp = secp->next)
	  {
	    if (secp->ch_type != 0)
	      {
		int sec = secp->sec;
		Elf_Scn *scn = dso->scn[sec];
		GElf_Shdr shdr = dso->shdr[sec];
		Elf_Data *data;
		data = elf_getdata (scn, NULL);
		if (elf_compress (scn, secp->ch_type, 0) < 0)
		  error (1, 0, "Failed recompression");
		gelf_getshdr (scn, &shdr);
		dso->shdr[secp->sec] = shdr;
		data = elf_getdata (scn, NULL);
		secp->elf_data = data;
		secp->data = data->d_buf;
		secp->size = data->d_size;
		elf_flagshdr (scn, ELF_C_SET, ELF_F_DIRTY);
		elf_flagdata (data, ELF_C_SET, ELF_F_DIRTY);
		recompressed = 1;
	      }
	  }
      }

  /* Normally we only need to explicitly update the section headers
     and data when any section data changed size. But because of a bug
     in elfutils before 0.169 we will have to update and write out all
     section data if any data has changed (when ELF_F_LAYOUT was
     set). https://sourceware.org/bugzilla/show_bug.cgi?id=21199 */
  bool need_update = (need_strp_update
		      || need_line_strp_update
		      || need_stmt_update
		      || recompressed);

#if !_ELFUTILS_PREREQ (0, 169)
  /* string replacements or build_id updates don't change section size. */
  need_update = (need_update
		 || need_string_replacement
		 || (do_build_id && build_id != NULL));
#endif

  /* We might have changed the size of some debug sections. If so make
     sure the section headers are updated and the data offsets are
     correct. We set ELF_F_LAYOUT above because we don't want libelf
     to move any allocated sections around itself if there are any
     phdrs. Which means we are responsible for setting the section size
     and offset fields. Plus the shdr offsets. We don't want to change
     anything for the phdrs allocated sections. Keep the offset of
     allocated sections so they are at the same place in the file. Add
     unallocated ones after the allocated ones. */
  if (dso->phnum != 0 && need_update)
    {
      Elf *elf = dso->elf;
      GElf_Off last_offset;
      /* We position everything after the phdrs (which normally would
	 be at the start of the ELF file after the ELF header. */
      last_offset = (dso->ehdr.e_phoff + gelf_fsize (elf, ELF_T_PHDR,
						     dso->phnum, EV_CURRENT));

      /* First find the last allocated section.  */
      Elf_Scn *scn = NULL;
      while ((scn = elf_nextscn (elf, scn)) != NULL)
	{
	  GElf_Shdr shdr_mem;
	  GElf_Shdr *shdr = gelf_getshdr (scn, &shdr_mem);
	  if (shdr == NULL)
	    error (1, 0, "Couldn't get shdr: %s", elf_errmsg (-1));

	  /* Any sections we have changed aren't allocated sections,
	     so we don't need to lookup any changed section sizes. */
	  if ((shdr->sh_flags & SHF_ALLOC) != 0)
	    {
	      GElf_Off off = shdr->sh_offset + (shdr->sh_type != SHT_NOBITS
						? shdr->sh_size : 0);
	      if (last_offset < off)
		last_offset = off;
	    }
	}

      /* Now adjust any sizes and offsets for the unallocated sections. */
      scn = NULL;
      while ((scn = elf_nextscn (elf, scn)) != NULL)
	{
	  GElf_Shdr shdr_mem;
	  GElf_Shdr *shdr = gelf_getshdr (scn, &shdr_mem);
	  if (shdr == NULL)
	    error (1, 0, "Couldn't get shdr: %s", elf_errmsg (-1));

	  /* A bug in elfutils before 0.169 means we have to write out
	     all section data, even when nothing changed.
	     https://sourceware.org/bugzilla/show_bug.cgi?id=21199 */
#if !_ELFUTILS_PREREQ (0, 169)
	  if (shdr->sh_type != SHT_NOBITS)
	    {
	      Elf_Data *d = elf_getdata (scn, NULL);
	      elf_flagdata (d, ELF_C_SET, ELF_F_DIRTY);
	    }
#endif
	  if ((shdr->sh_flags & SHF_ALLOC) == 0)
	    {
	      GElf_Off sec_offset = shdr->sh_offset;
	      GElf_Xword sec_size = shdr->sh_size;

	      /* We might have changed the size (and content) of the
		 debug_str, debug_line_str or debug_line section. */
	      size_t secnum = elf_ndxscn (scn);
	      if (secnum == debug_sections[DEBUG_STR].sec)
		sec_size = debug_sections[DEBUG_STR].size;
	      if (secnum == debug_sections[DEBUG_LINE_STR].sec)
		sec_size = debug_sections[DEBUG_LINE_STR].size;
	      if (secnum == debug_sections[DEBUG_LINE].sec)
		sec_size = debug_sections[DEBUG_LINE].size;

	      /* Zero means one.  No alignment constraints.  */
	      size_t addralign = shdr->sh_addralign ?: 1;
	      last_offset = (last_offset + addralign - 1) & ~(addralign - 1);
	      sec_offset = last_offset;
	      if (shdr->sh_type != SHT_NOBITS)
		last_offset += sec_size;

	      if (shdr->sh_size != sec_size
		  || shdr->sh_offset != sec_offset)
		{
		  /* Make sure unchanged section data is written out
		     at the new location. */
		  if (shdr->sh_offset != sec_offset
		      && shdr->sh_type != SHT_NOBITS)
		    {
		      Elf_Data *d = elf_getdata (scn, NULL);
		      elf_flagdata (d, ELF_C_SET, ELF_F_DIRTY);
		    }

		  shdr->sh_size = sec_size;
		  shdr->sh_offset = sec_offset;
		  if (gelf_update_shdr (scn, shdr) == 0)
		    error (1, 0, "Couldn't update shdr: %s",
			   elf_errmsg (-1));
		}
	    }
	}

      /* Position the shdrs after the last (unallocated) section.  */
      const size_t offsize = gelf_fsize (elf, ELF_T_OFF, 1, EV_CURRENT);
      GElf_Off new_offset = ((last_offset + offsize - 1)
			     & ~((GElf_Off) (offsize - 1)));
      if (dso->ehdr.e_shoff != new_offset)
	{
	  dso->ehdr.e_shoff = new_offset;
	  if (gelf_update_ehdr (elf, &dso->ehdr) == 0)
	    error (1, 0, "Couldn't update ehdr: %s", elf_errmsg (-1));
	}
    }

  if (elf_update (dso->elf, ELF_C_NULL) < 0)
    {
      error (1, 0, "Failed to update file: %s", elf_errmsg (elf_errno ()));
    }

  if (do_build_id && build_id != NULL)
    handle_build_id (dso, build_id, build_id_offset, build_id_size);

  /* If we have done any string replacement or rewrote any section
     data or did a build_id rewrite we need to write out the new ELF
     image.  */
  if ((need_string_replacement
       || need_strp_update
       || need_line_strp_update
       || need_stmt_update
       || dirty_elf
       || (build_id && !no_recompute_build_id)
       || recompressed)
      && elf_update (dso->elf, ELF_C_WRITE) < 0)
    {
      error (1, 0, "Failed to write file: %s", elf_errmsg (elf_errno()));
    }
  if (elf_end (dso->elf) < 0)
    {
      error (1, 0, "elf_end failed: %s", elf_errmsg (elf_errno()));
    }
  close (fd);

  /* Restore old access rights */
  if (chmod (file, stat_buf.st_mode) != 0)
    error (0, errno, "Failed to chmod input file '%s' to restore old access rights", file);

  /* Preserve timestamps.  */
  if (preserve_dates)
    {
      struct timespec tv[2];
      tv[0] = stat_buf.st_atim;
      tv[1] = stat_buf.st_mtim;
      if (utimensat (AT_FDCWD, file, tv, 0) != 0)
	error (0, errno, "Failed to preserve timestamps on '%s'", file);
    }

  free ((char *) dso->filename);
  destroy_strings (&dso->debug_str);
  destroy_strings (&dso->debug_line_str);
  destroy_lines (&dso->lines);
  destroy_cus (dso->cus);
  free (dso);

  /* In case there were multiple (COMDAT) .debug_macro sections,
     free them.  */
  struct debug_section *macro_sec = &debug_sections[DEBUG_MACRO];
  macro_sec = macro_sec->next;
  while (macro_sec != NULL)
    {
      struct debug_section *next = macro_sec->next;
      free (macro_sec);
      macro_sec = next;
    }

  /* In case there were multiple (COMDAT) .debug_types sections,
     free them.  */
  struct debug_section *types_sec = &debug_sections[DEBUG_TYPES];
  types_sec = types_sec->next;
  while (types_sec != NULL)
    {
      struct debug_section *next = types_sec->next;
      free (types_sec);
      types_sec = next;
    }

  return 0;
}
