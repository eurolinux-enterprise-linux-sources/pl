/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2008, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*#define O_DEBUG 1*/
#include "pl-incl.h"
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

static char *	getString(IOSTREAM *, unsigned *len);
static int64_t	getInt64(IOSTREAM *);
static int	getInt32(IOSTREAM *s);
static long	getLong(IOSTREAM *);
static int	getInt(IOSTREAM *);
static double	getFloat(IOSTREAM *);
static bool	loadWicFd(IOSTREAM *);
static bool	loadPredicate(IOSTREAM *, int skip ARG_LD);
static bool	loadImport(IOSTREAM *, int skip ARG_LD);
static void	saveXRBlobType(PL_blob_t *type, IOSTREAM *fd);
static void	putString(const char *, size_t len, IOSTREAM *);
static void	putNum(int64_t, IOSTREAM *);
static void	putFloat(double, IOSTREAM *);
static void	saveWicClause(Clause, IOSTREAM *);
static void	closeProcedureWic(IOSTREAM *);
static word	loadXRc(int c, IOSTREAM *fd ARG_LD);
static atom_t   getBlob(IOSTREAM *fd ARG_LD);
static bool	loadStatement(int c, IOSTREAM *fd, int skip ARG_LD);
static bool	loadPart(IOSTREAM *fd, Module *module, int skip ARG_LD);
static bool	loadInModule(IOSTREAM *fd, int skip ARG_LD);
static int	qlfVersion(IOSTREAM *s);
static atom_t	qlfFixSourcePath(const char *raw);
static int	pushPathTranslation(IOSTREAM *fd, const char *loadname,
				    int flags);
static void	popPathTranslation(void);

#define Qgetc(s) Snpgetc(s)		/* ignore position recording */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
SWI-Prolog can compile Prolog source files into intermediate code files,
which can be loaded very  fast.   They  can  be  saved  as  stand  alone
executables using Unix #! magic number.

A wic file consists of the magic code and a version check code.  This is
followed by the command line option   defaults.  Then an optional series
of `include' statements follow.  Finally   the predicates and directives
are  described.   Predicates  are  described    close  to  the  internal
representation.  Directives are stored as  binary terms representing the
query.

The default options and include statements are written incrementally  in
each  wic  file.   In  the  normal  boot  cycle  first  the boot file is
determined.  Then the option structure is filled with the default option
found in this boot file.  Next the command line arguments are scanned to
obtain all options.  Then stacks, built  in's,  etc.   are  initialised.
The  the  boot  file is read again, but now only scanning for directives
and predicates.

IF YOU CHANGE ANYTHING TO THIS FILE, SO THAT OLD WIC-FILES CAN NO LONGER
BE READ, PLEASE DO NOT FORGET TO INCREMENT THE VERSION NUMBER!

Below is an informal description of the format of a `.qlf' file:

<wic-file>	::=	<magic code>
			<version number>
			<bits-per-word>
			<home>				% a <string>
			{<statement>}
			'T'
----------------------------------------------------------------
<qlf-file>	::=	<qlf-magic>
			<version-number>
			<bits-per-word>
			'F' <string>			% path of qlf file
			'Q' <qlf-part>
<qlf-magic>	::=	<string>
<qlf-module>	::=	<qlf-header>
			<size>				% size in bytes
			{<statement>}
			'X'
<qlf-header>	::=	'M' <XR/modulename>		% module name
			<source>			% file + time
			{<qlf-export>}
			'X'
		      | <source>			% not a module
			<time>
<qlf-export>	::=	'E' <XR/functor>
<source>	::=	'F' <string> <time> <system>
		      | '-'
----------------------------------------------------------------
<magic code>	::=	<string>			% normally #!<path>
<version number>::=	<num>
<statement>	::=	'W' <string>			% include wic file
		      | 'P' <XR/functor> 		% predicate
			    <flags>
			    {<clause>} <pattern>
		      |	'O' <XR/modulename>		% pred out of module
			    <XR/functor>
			    <flags>
			    {<clause>} <pattern>
		      | 'D'
		        <lineno>			% source line number
			<term>				% directive
		      | 'E' <XR/functor>		% export predicate
		      | 'I' <XR/procedure>		% import predicate
		      | 'Q' <qlf-module>		% include module
		      | 'M' <XR/modulename>		% load-in-module
		            {<statement>}
			    'X'
<flags>		::=	<num>				% Bitwise or of PRED_*
<clause>	::=	'C' <#codes>
			    <line_no> <# var>
			    <#n subclause> <codes>
		      | 'X' 				% end of list
<XR>		::=	XR_REF     <num>		% XR id from table
			XR_ATOM    <len><chars>		% atom
			XR_BLOB	   <blob><private>	% typed atom (blob)
			XR_INT     <num>		% number
			XR_FLOAT   <word>*		% float (double)
			XR_STRING  <string>		% string
			XR_STRING_UTF8  <utf-8 string>	% wide string
			XR_FUNCTOR <XR/name> <num>	% functor
			XR_PRED    <XR/fdef> <XR/module>% predicate
			XR_MODULE  <XR/name>		% module
			XR_FILE	   's'|'u' <XR/atom> <time>
				   '-'
			XR_BLOB_TYPE <len><chars>	% blob type-name
<term>		::=	<num>				% # variables in term
			<theterm>
<theterm>	::=	<XR/atomic>			% atomic data
		      | 'v' <num>			% variable
		      | 't' <XR/functor> {<theterm>}	% compound
<system>	::=	's'				% system source file
		      | 'u'				% user source file
<time>		::=	<word>				% time file was loaded
<pattern>	::=	<num>				% indexing pattern
<codes>		::=	<num> {<code>}
<string>	::=	{<non-zero byte>} <0>
<word>		::=	<4 byte entity>

Numbers are stored in  a  packed  format  to  reduce  the  size  of  the
intermediate  code  file  as  99%  of  them  is  normally  small, but in
principle not limited (virtual  machine  codes,  arities,  table  sizes,
etc).   The  upper  two  bits  of  the  first byte contain the number of
additional bytes.  the bytes represent the number `most-significant part
first'.  See the functions putNum() and getNum()  for  details.   Before
you  don't  agree  to  this  schema,  you  should  remember it makes the
intermediate code files about 30% smaller  and  avoids  the  differences
between  16  and  32  bits  machines (arities on 16 bits machines are 16
bits) as well as machines with different byte order.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define LOADVERSION 57			/* load all versions later >= X */
#define VERSION 57			/* save version number */
#define QLFMAGICNUM 0x716c7374		/* "qlst" on little-endian machine */

#define XR_REF     0			/* reference to previous */
#define XR_ATOM	   1			/* atom */
#define XR_FUNCTOR 2			/* functor */
#define XR_PRED	   3			/* procedure */
#define XR_INT     4			/* int */
#define XR_FLOAT   5			/* float */
#define XR_STRING  6			/* string */
#define XR_FILE	   7			/* source file */
#define XR_MODULE  8			/* a module */
#define XR_BLOB	   9			/* a typed atom (blob) */
#define XR_BLOB_TYPE 10			/* name of atom-type declaration */
#define XR_STRING_UTF8 11		/* Wide character string */

#define PRED_SYSTEM	 0x01		/* system predicate */
#define PRED_HIDE_CHILDS 0x02		/* hide my childs */

static char saveMagic[] = "SWI-Prolog (c) 1990 Jan Wielemaker\n";
static char qlfMagic[]  = "SWI-Prolog .qlf file\n";
static char *wicFile;			/* name of output file */
static char *mkWicFile;			/* Wic file under construction */
static IOSTREAM *wicFd;			/* file descriptor of wic file */
static Procedure currentProc;		/* current procedure */
static SourceFile currentSource;	/* current source file */


#undef LD
#define LD LOCAL_LD

		 /*******************************
		 *     LOADED XR ID HANDLING	*
		 *******************************/

typedef struct xr_table *XrTable;

struct xr_table
{ int		id;			/* next id to give out */
  Word	       *table;			/* main table */
  int   	tablesize;		/* # sub-arrays */
  XrTable	previous;		/* stack */
};

static XrTable loadedXrs;		/* head pointer */

#define loadedXRTableId		(loadedXrs->id)

#define SUBENTRIES ((ALLOCSIZE)/sizeof(word))

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
XR reference handling during loading.  This   is arranged as an array-of
arrays.  These arrays are of size ALLOCSIZE,   so they will be reused on
perfect-fit basis the pl-alloc.c.  With ALLOCSIZE   = 64K, this requires
minimal 128K memory.   Maximum  allowed  references   is  16K^2  or  32M
references.  That will normally overflow other system limits first.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
pushXrIdTable(ARG1_LD)
{ XrTable t = allocHeap(sizeof(struct xr_table));

  if ( !(t->table = malloc(ALLOCSIZE)) )
    outOfCore();
  SECURE(memset(t->table, 0, ALLOCSIZE));
  t->tablesize = 0;
  t->id = 0;

  t->previous = loadedXrs;
  loadedXrs = t;
}


static void
popXrIdTable(ARG1_LD)
{ int i;
  XrTable t = loadedXrs;

  loadedXrs = t->previous;		/* pop the stack */

  for(i=0; i<t->tablesize; i++)		/* destroy obsolete table */
    free(t->table[i]);

  free(t->table);
  freeHeap(t, sizeof(*t));
}


static word
lookupXrId(intptr_t id)
{ XrTable t = loadedXrs;
  Word array = t->table[id/SUBENTRIES];
  word value;

  SECURE(assert(array));
  value = array[id%SUBENTRIES];

  return value;
}


static void
storeXrId(long id, word value)
{ XrTable t = loadedXrs;
  long i = id/SUBENTRIES;

  while ( i >= t->tablesize )
  { Word a = malloc(ALLOCSIZE);

    if ( !a )
      outOfCore();

    SECURE(memset(a, 0, ALLOCSIZE));
    t->table[t->tablesize++] = a;
  }

  t->table[i][id%SUBENTRIES] = value;
}


		 /*******************************
		 *	 PRIMITIVE LOADING	*
		 *******************************/

typedef struct qlf_state
{ int	has_moved;			/* Paths must be translated */
  char *save_dir;			/* Directory saved */
  char *load_dir;			/* Directory loading */
  int   saved_version;			/* Version saved */
  struct qlf_state *previous;		/* previous saved state (reentrance) */
} qlf_state;

static qlf_state *load_state;		/* current load-state */

#define PATH_ISDIR	0x1		/* pushPathTranslation() flags */

static bool
qlfLoadError(IOSTREAM *fd, char *ctx)
{ fatalError("%s: QLF format error at index = %ld", ctx, Stell(fd));

  fail;
}


static char *getstr_buffer = NULL;
static int  getstr_buffer_size = 0;

static char *
getString(IOSTREAM *fd, unsigned *length)
{ char *s;
  int len = getInt(fd);
  int i;

  if ( getstr_buffer_size < len+1 )
  { int size = ((len+1+1023)/1024)*1024;

    if ( getstr_buffer )
      getstr_buffer = realloc(getstr_buffer, size);
    else
      getstr_buffer = malloc(size);

    if ( getstr_buffer )
      getstr_buffer_size = size;
    else
      outOfCore();
  }

  for( i=0, s = getstr_buffer; i<len; i++ )
  { int c = Sgetc(fd);

    if ( c == EOF )
      fatalError("Unexpected EOF on intermediate code file at offset %d",
		 Stell(fd));

    *s++ = c;
  }
  *s = EOS;

  if ( length )
    *length = (unsigned) len;

  return getstr_buffer;
}


pl_wchar_t *
wicGetStringUTF8(IOSTREAM *fd, size_t *length,
		 pl_wchar_t *buf, size_t bufsize)
{ size_t i, len = (size_t)wicGetNum(fd);
  IOENC oenc = fd->encoding;
  pl_wchar_t *tmp, *o;

  if ( length )
    *length = len;

  if ( len < bufsize )
    tmp = buf;
  else
    tmp = PL_malloc(len*sizeof(pl_wchar_t));

  fd->encoding = ENC_UTF8;
  for(i=0, o=tmp; i<len; i++)
  { int c = Sgetcode(fd);

    if ( c < 0 )
      fatalError("Unexpected EOF in UCS atom");
    *o++ = c;
  }
  fd->encoding = oenc;

  return tmp;
}



static atom_t
getAtom(IOSTREAM *fd, PL_blob_t *type ARG_LD)
{ char buf[1024];
  char *tmp, *s;
  size_t len = getInt(fd);
  size_t i;
  atom_t a;

  if ( len < sizeof(buf) )
    tmp = buf;
  else
    tmp = allocHeap(len);

  for(s=tmp, i=0; i<len; i++)
  { int c = Sgetc(fd);

    if ( c == EOF )
      fatalError("Unexpected EOF on intermediate code file at offset %d",
		 Stell(fd));
    *s++ = c;
  }
  if ( type )
  { int new;

    a = lookupBlob(tmp, len, type, &new);
  } else
  { a = lookupAtom(tmp, len);
  }

  if ( tmp != buf )
    freeHeap(tmp, len);

  return a;
}


static PL_blob_t *
getBlobType(IOSTREAM *fd)
{ const char *name = getString(fd, NULL);

  return PL_find_blob_type(name);
}


static char *
getMagicString(IOSTREAM *fd, char *buf, int maxlen)
{ char *s;
  int c;

  for( s = buf; --maxlen >= 0 && (*s = (c = Sgetc(fd))); s++ )
    if ( c == EOF )
      return NULL;

  if ( maxlen > 0 )
    return buf;

  return NULL;
}


static int64_t
getInt64(IOSTREAM *fd)
{ int64_t first;
  int bytes, shift, b;

  DEBUG(4, Sdprintf("getInt64() from %ld --> \n", Stell(fd)));

  first = Sgetc(fd);
  if ( !(first & 0xc0) )		/* 99% of them: speed up a bit */
  { first <<= (INT64BITSIZE-6);
    first >>= (INT64BITSIZE-6);

    DEBUG(4, Sdprintf(INT64_FORMAT "\n", first));
    return first;
  }

  bytes = (int) ((first >> 6) & 0x3);
  first &= 0x3f;

  if ( bytes <= 2 )
  { for( b = 0; b < bytes; b++ )
    { first <<= 8;
      first |= Sgetc(fd) & 0xff;
    }

    shift = (sizeof(first)-1-bytes)*8 + 2;
  } else
  { int m;

    bytes = (int)first;
    first = (int64_t)0;

    for(m=0; m<bytes; m++)
    { first <<= 8;
      first |= Sgetc(fd) & 0xff;
    }
    shift = (sizeof(first)-bytes)*8;
  }

  first <<= shift;
  first >>= shift;

  DEBUG(4, Sdprintf(INT64_FORMAT "\n", first));
  return first;
}


static long
getLong(IOSTREAM *fd)
{ int64_t val = getInt64(fd);

  return (long)val;
}


static int
getInt(IOSTREAM *fd)
{ int64_t val = getInt64(fd);

  return (int)val;
}


#ifdef WORDS_BIGENDIAN
static const int double_byte_order[] = { 7,6,5,4,3,2,1,0 };
#else
static const int double_byte_order[] = { 0,1,2,3,4,5,6,7 };
#endif

#define BYTES_PER_DOUBLE (sizeof(double_byte_order)/sizeof(int))

static double
getFloat(IOSTREAM *fd)
{ double f;
  unsigned char *cl = (unsigned char *)&f;
  unsigned int i;

  for(i=0; i<BYTES_PER_DOUBLE; i++)
  { int c = Sgetc(fd);

    if ( c == -1 )
      fatalError("Unexpected end-of-file in QLT file");
    cl[double_byte_order[i]] = c;
  }

  DEBUG(3, Sdprintf("getFloat() --> %f\n", f));

  return f;
}


static int
getInt32(IOSTREAM *s)
{ int v;

  v  = (Sgetc(s) & 0xff) << 24;
  v |= (Sgetc(s) & 0xff) << 16;
  v |= (Sgetc(s) & 0xff) << 8;
  v |= (Sgetc(s) & 0xff);

  return v;
}


static inline word
loadXR__LD(IOSTREAM *fd ARG_LD)
{ return loadXRc(Qgetc(fd), fd PASS_LD);
}
#define loadXR(s) loadXR__LD(s PASS_LD)


static word
loadXRc(int c, IOSTREAM *fd ARG_LD)
{ word xr;
  int id = 0;				/* make gcc happy! */

  switch( c )
  { case XR_REF:
    { intptr_t xr  = getLong(fd);
      word val = lookupXrId(xr);

      return val;
    }
    case XR_ATOM:
    { id = ++loadedXRTableId;
      xr = getAtom(fd, NULL PASS_LD);
      DEBUG(3, Sdprintf("XR(%d) = '%s'\n", id, stringAtom(xr)));
      break;
    }
    case XR_BLOB:
    { id = ++loadedXRTableId;
      xr = getBlob(fd PASS_LD);
      DEBUG(3, Sdprintf("XR(%d) = <blob>\n", id));
      break;
    }
    case XR_BLOB_TYPE:
    { id = ++loadedXRTableId;
      xr = (word)getBlobType(fd);
      DEBUG(3, Sdprintf("XR(%d) = <blob-type>%s", id, ((PL_blob_t*)xr)->name));
      break;
    }
    case XR_FUNCTOR:
    { atom_t name;
      int arity;

      id = ++loadedXRTableId;
      name = loadXR(fd);
      arity = getInt(fd);
      xr = (word) lookupFunctorDef(name, arity);
      DEBUG(3, Sdprintf("XR(%d) = %s/%d\n", id, stringAtom(name), arity));
      break;
    }
    case XR_PRED:
    { functor_t f;
      Module m;

      id = ++loadedXRTableId;
      f = (functor_t) loadXR(fd);
      m = (Module) loadXR(fd);
      xr = (word) lookupProcedure(f, m);
      DEBUG(3, Sdprintf("XR(%d) = proc %s\n", id, procedureName((Procedure)xr)));
      break;
    }
    case XR_MODULE:
    { atom_t name;
      id = ++loadedXRTableId;
      name = loadXR(fd);
      xr = (word) lookupModule(name);
      DEBUG(3, Sdprintf("XR(%d) = module %s\n", id, stringAtom(name)));
      break;
    }
    case XR_INT:
      return makeNum(getInt64(fd));
    case XR_FLOAT:
      return globalFloat(getFloat(fd));
#if O_STRING
    case XR_STRING:
    { char *s;
      unsigned len;

      s = getString(fd, &len);

      return globalString(len, s);
    }
    case XR_STRING_UTF8:
    { pl_wchar_t *w;
      size_t len;
      pl_wchar_t buf[256];
      word s;

      w = wicGetStringUTF8(fd, &len, buf, sizeof(buf)/sizeof(pl_wchar_t));
      s = globalWString(len, w);
      if ( w != buf )
	PL_free(w);

      return s;
    }
#endif
    case XR_FILE:
    { int c;

      id = ++loadedXRTableId;

      switch( (c=Qgetc(fd)) )
      { case 'u':
	case 's':
	{ atom_t name   = loadXR(fd);
	  word   time   = getLong(fd);
	  const char *s = stringAtom(name);
	  SourceFile sf = lookupSourceFile(qlfFixSourcePath(s), TRUE);

	  if ( !sf->time )
	  { sf->time   = time;
	    sf->system = (c == 's' ? TRUE : FALSE);
	  }
	  sf->count++;
	  xr = (word)sf;
	  break;
	}
	case '-':
	  xr = 0;
	  break;
	default:
	  xr = 0;			/* make gcc happy */
	  fatalError("Illegal XR file index %d: %c", Stell(fd)-1, c);
      }

      break;
    }
    default:
    { xr = 0;				/* make gcc happy */
      fatalError("Illegal XR entry at index %d: %c", Stell(fd)-1, c);
    }
  }

  storeXrId(id, xr);

  return xr;
}


static atom_t
getBlob(IOSTREAM *fd ARG_LD)
{ PL_blob_t *type = (PL_blob_t*)loadXR(fd);

  if ( type->load )
  { return (*type->load)(fd);
  } else
  { return getAtom(fd, type PASS_LD);
  }
}


static void
do_load_qlf_term(IOSTREAM *fd, term_t vars[], term_t term ARG_LD)
{ int c = Qgetc(fd);

  if ( c == 'v' )
  { int id = getInt(fd);

    if ( vars[id] )
      PL_unify(term, vars[id]);
    else
    { vars[id] = PL_new_term_ref();
      PL_put_term(vars[id], term);
    }
  } else if ( c == 't' )
  { functor_t f = (functor_t) loadXR(fd);
    term_t c2 = PL_new_term_ref();
    int arity = arityFunctor(f);
    int n;

    PL_unify_functor(term, f);
    for(n=0; n < arity; n++)
    { _PL_get_arg(n+1, term, c2);
      do_load_qlf_term(fd, vars, c2 PASS_LD);
    }
  } else
  { _PL_unify_atomic(term, loadXRc(c, fd PASS_LD));
  }
}


static void
loadQlfTerm(term_t term, IOSTREAM *fd ARG_LD)
{ int nvars;
  Word vars;

  DEBUG(3, Sdprintf("Loading from %d ...", Stell(fd)));

  if ( (nvars = getInt(fd)) )
  { term_t *v;
    int n;

    vars = alloca(nvars * sizeof(term_t));
    for(n=nvars, v=vars; n>0; n--, v++)
      *v = 0L;
  } else
    vars = NULL;

  PL_put_variable(term);
  do_load_qlf_term(fd, vars, term PASS_LD);
  DEBUG(3,
	Sdprintf("Loaded ");
	PL_write_term(Serror, term, 1200, 0);
	Sdprintf(" to %d\n", Stell(fd)));
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Load intermediate code state from the   specified  stream. This function
loads the initial saved state, either  boot32.prc, the state attached to
the executable or the argument of pl -x <state>.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
loadWicFromStream(IOSTREAM *fd)
{ GET_LD
  int rval;

  pushXrIdTable(PASS_LD1);
  rval = loadWicFd(fd);
  popXrIdTable(PASS_LD1);

  return rval;
}


static int
loadWicFile(const char *file)
{ IOSTREAM *fd;
  int rval;

  if ( !(fd = Sopen_file(file, "rbr")) )
  { warning("Cannot open Quick Load File %s: %s", file, OsError());
    return FALSE;
  }

  rval = loadWicFromStream(fd);
  Sclose(fd);

  return rval;
}


#define QLF_MAX_HEADER_LINES 100

static bool
loadWicFd(IOSTREAM *fd)
{ GET_LD
  char *s;
  Char c;
  char mbuf[100];
  int saved_wsize;
  int saved_version;
  int vm_signature;

  s = getMagicString(fd, mbuf, sizeof(mbuf));
  if ( !s || !streq(s, saveMagic) )
  { fatalError("Not a SWI-Prolog saved state");
    fail;				/* NOTREACHED */
  }

  if ( (saved_version=getInt(fd)) < LOADVERSION )
  { fatalError("Saved state has incompatible save version");
    fail;
  }
  if ( (vm_signature=getInt(fd)) != (int)VM_SIGNATURE )
  { fatalError("Saved state has incompatible VM signature");
    fail;
  }

  saved_wsize = getInt(fd);
  if ( saved_wsize != sizeof(word)*8 )
  { fatalError("Saved state has incompatible (%d) word-length", saved_wsize);
    fail;
  }
					/* fix paths for changed home */
  pushPathTranslation(fd, systemDefaults.home, PATH_ISDIR);
  load_state->saved_version = saved_version;

  for(;;)
  { c = Sgetc(fd);

    switch( c )
    { case EOF:
      case 'T':				/* trailer */
	popPathTranslation();
	succeed;
      case 'W':
	{ char *name = store_string(getString(fd, NULL) );

	  loadWicFile(name);
	  continue;
	}
      case 'X':
        break;
      default:
        { loadStatement(c, fd, FALSE PASS_LD);
	  continue;
	}
    }
  }
}


static bool
loadStatement(int c, IOSTREAM *fd, int skip ARG_LD)
{ switch(c)
  { case 'P':
      return loadPredicate(fd, skip PASS_LD);

    case 'O':
    { word mname = loadXR(fd);
      Module om = LD->modules.source;
      bool rval;

      LD->modules.source = lookupModule(mname);
      rval = loadPredicate(fd, skip PASS_LD);
      LD->modules.source = om;

      return rval;
    }
    case 'I':
      return loadImport(fd, skip PASS_LD);

    case 'D':
    { fid_t   cid = PL_open_foreign_frame();
      term_t goal = PL_new_term_ref();
      atom_t  osf = source_file_name;
      int     oln = source_line_no;

      source_file_name = (currentSource ? currentSource->name : NULL_ATOM);
      source_line_no   = getInt(fd);

      loadQlfTerm(goal, fd PASS_LD);
      DEBUG(2,
	    if ( source_file_name )
	    { Sdprintf("%s:%d: Directive: ",
			PL_atom_chars(source_file_name), source_line_no);
	    } else
	    { Sdprintf("Directive: ");
	    }
	    pl_write(goal);
	    Sdprintf("\n"));
      if ( !skip )
      { if ( !callProlog(MODULE_user, goal, PL_Q_NODEBUG, NULL) )
	{ Sfprintf(Serror,
		   "[WARNING: %s:%d: (loading %s) directive failed: ",
		   source_file_name ? stringAtom(source_file_name)
		                    : "<no file>",
		   source_line_no, wicFile);
	  PL_write_term(Serror, goal, 1200, 0);
	  Sfprintf(Serror, "]\n");
	}
      }
      PL_discard_foreign_frame(cid);

      source_file_name = osf;
      source_line_no   = oln;

      succeed;
    }

    case 'Q':
      return loadPart(fd, NULL, skip PASS_LD);

    case 'M':
      return loadInModule(fd, skip PASS_LD);

    default:
      return qlfLoadError(fd, "loadStatement()");
  }
}


static void
loadPredicateFlags(Definition def, IOSTREAM *fd, int skip ARG_LD)
{ if ( load_state->saved_version <= 31 )
  { if ( !skip && SYSTEM_MODE )
      set(def, SYSTEM|HIDE_CHILDS|LOCKED);
  } else
  { int	flags = getInt(fd);

    if ( !skip )
    { unsigned long lflags = 0L;

      if ( flags & PRED_SYSTEM )
	lflags |= SYSTEM;
      if ( flags & PRED_HIDE_CHILDS )
	lflags |= HIDE_CHILDS;

      set(def, lflags);
    }
  }
}


static bool
loadPredicate(IOSTREAM *fd, int skip ARG_LD)
{ Procedure proc;
  Definition def;
  Clause clause;
  functor_t f = (functor_t) loadXR(fd);
  SourceFile csf = NULL;

  proc = lookupProcedure(f, LD->modules.source);
  DEBUG(2, Sdprintf("Loading %s%s",
		    procedureName(proc),
		    skip ? " (skip)" : ""));

  def = proc->definition;
  if ( !skip && currentSource )
  { if ( def->definition.clauses )
      redefineProcedure(proc, currentSource, DISCONTIGUOUS_STYLE);
    addProcedureSourceFile(currentSource, proc);
  }
  if ( def->references == 0 && !def->hash_info )
    def->indexPattern |= NEED_REINDEX;
  loadPredicateFlags(def, fd, skip PASS_LD);

  for(;;)
  { switch(Sgetc(fd) )
    { case 'X':
      { unsigned long pattern = getLong(fd);

	if ( (def->indexPattern & ~NEED_REINDEX) != pattern )
	{ if ( def->references == 0 && !def->hash_info )
	    def->indexPattern = (pattern | NEED_REINDEX);
	  else if ( false(def, MULTIFILE|DYNAMIC) )
	    Sdprintf("Cannot change indexing of %s\n", predicateName(def));
	}

	DEBUG(2, Sdprintf("ok\n"));
	succeed;
      }
      case 'C':
      { Code bp, ep;
	int ncodes = getInt(fd);

	DEBUG(2, Sdprintf("."));
	clause = (Clause) allocHeap(sizeofClause(ncodes));
	clause->code_size = (unsigned int) ncodes;
	clause->line_no = (unsigned short) getInt(fd);

	{ SourceFile sf = (void *) loadXR(fd);
	  int sno = (sf ? sf->index : 0);

	  clause->source_no = sno;
	  if ( sf && sf != csf )
	  { addProcedureSourceFile(sf, proc);
	    csf = sf;
	  }
	}

	clearFlags(clause);
	clause->prolog_vars = (unsigned short) getInt(fd);
	clause->variables   = (unsigned short) getInt(fd);
#ifdef O_SHIFT_STACKS
	clause->marks = clause->variables - clause->prolog_vars;
#endif
	if ( getLong(fd) == 0 )		/* 0: fact */
	  set(clause, UNIT_CLAUSE);
	clause->procedure = proc;
	GD->statistics.codes += clause->code_size;

	bp = clause->codes;
	ep = bp + clause->code_size;

	while( bp < ep )
	{ code op = getInt(fd);
	  const char *ats;
	  int n = 0;

	  if ( op >= I_HIGHEST )
	    fatalError("Illegal op-code (%d) at %ld", op, Stell(fd));

	  ats = codeTable[op].argtype;
	  DEBUG(3, Sdprintf("\t%s from %ld\n", codeTable[op].name, Stell(fd)));
	  *bp++ = encode(op);
	  DEBUG(0, assert(codeTable[op].arguments == VM_DYNARGC ||
			  (size_t)codeTable[op].arguments == strlen(ats)));

	  for(n=0; ats[n]; n++)
	  { switch(ats[n])
	    { case CA1_PROC:
	      { *bp++ = loadXR(fd);
		break;
	      }
	      case CA1_FUNC:
	      case CA1_DATA:
	      { word w = loadXR(fd);
		if ( isAtom(w) )
		  PL_register_atom(w);
		*bp++ = w;
		break;
	      }
	      case CA1_MODULE:
		*bp++ = loadXR(fd);
		break;
	      case CA1_INTEGER:
	      case CA1_JUMP:
	      case CA1_VAR:
	      case CA1_CHP:
	      case CA1_AFUNC:
		*bp++ = (intptr_t)getInt64(fd);
		break;
	      case CA1_INT64:
	      { int64_t val = getInt64(fd);
		Word p = (Word)&val;

		cpInt64Data(bp, p);
		break;
	      }
	      case CA1_FLOAT:
	      { union
		{ word w[WORDS_PER_DOUBLE];
		  double f;
		} v;
		Word p = v.w;
		v.f = getFloat(fd);
		cpDoubleData(bp, p);
		break;
	      }
	      case CA1_STRING:		/* <n> chars */
	      { int l = getInt(fd);
		int lw = (l+sizeof(word))/sizeof(word);
		int pad = (lw*sizeof(word) - l);
		char *s = (char *)&bp[1];

		DEBUG(3, Sdprintf("String of %ld bytes\n", l));
		*bp = mkStrHdr(lw, pad);
		bp += lw;
		*bp++ = 0L;
		while(--l >= 0)
		  *s++ = Sgetc(fd);
		break;
	      }
	      case CA1_MPZ:
#ifdef O_GMP
	      DEBUG(3, Sdprintf("Loading MPZ from %ld\n", Stell(fd)));
	      { int mpsize = getInt(fd);
		int l      = abs(mpsize)*sizeof(mp_limb_t);
		int wsz	 = (l+sizeof(word)-1)/sizeof(word);
		word m     = mkIndHdr(wsz+1, TAG_INTEGER);
		char *s;

		*bp++     = m;
		*bp++     = mpsize;
		s         = (char*)bp;
		bp[wsz-1] = 0L;
		bp       += wsz;

		while(--l >= 0)
		  *s++ = Sgetc(fd);
		DEBUG(3, Sdprintf("Loaded MPZ to %ld\n", Stell(fd)));
		break;
	      }
#else
		fatalError("No support for MPZ numbers");
#endif
	      default:
		fatalError("No support for VM argtype %d (arg %d of %s)",
			   ats[n], n, codeTable[op].name);
	    }
	  }
	}

	if ( skip )
	  freeClause(clause PASS_LD);
	else
	{ if ( def->hash_info )
	  { reindexClause(clause, def, 0x1L);
	  }
	  assertProcedure(proc, clause, CL_END PASS_LD);
	}
      }
    }
  }
}


static bool
loadImport(IOSTREAM *fd, int skip ARG_LD)
{ Procedure proc = (Procedure) loadXR(fd);

  if ( !skip )
    return importDefinitionModule(LD->modules.source, proc->definition);

  succeed;
}


static atom_t
qlfFixSourcePath(const char *raw)
{ char buf[MAXPATHLEN];

  if ( load_state->has_moved && strprefix(raw, load_state->save_dir) )
  { char *s;
    size_t lensave = strlen(load_state->save_dir);
    const char *tail = &raw[lensave];

    if ( strlen(load_state->load_dir)+1+strlen(tail)+1 > MAXPATHLEN )
      fatalError("Path name too long: %s", raw);

    strcpy(buf, load_state->load_dir);
    s = &buf[strlen(buf)];
    *s++ = '/';
    strcpy(s, tail);
  } else
  { if ( strlen(raw)+1 > MAXPATHLEN )
      fatalError("Path name too long: %s", raw);
    strcpy(buf, raw);
  }

  return PL_new_atom(canonisePath(buf));
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
(**) Note. When loading a qlf  file   we  must do the possible reconsult
stuff associated with loading sourcefiles. If we are loading a state all
is nice and fresh, so we can skip that. Actually, we *must* skip that as
a state is  created  based  on   modules  rather  then  files. Multifile
predicates are stored with the module. If   we  take no measures loading
the file from which a clause originates  will remove the one loaded with
the module where it is a multifile one.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool
qlfLoadSource(IOSTREAM *fd)
{ char *str = getString(fd, NULL);
  intptr_t time = getLong(fd);
  int issys = (Qgetc(fd) == 's') ? TRUE : FALSE;
  atom_t fname;

  fname = qlfFixSourcePath(str);

  DEBUG(1, if ( !streq(stringAtom(fname), str) )
	     Sdprintf("Replaced path %s --> %s\n", str, stringAtom(fname)));

  currentSource = lookupSourceFile(fname, TRUE);
  currentSource->time = time;
  currentSource->system = issys;
  if ( GD->bootsession )		/* (**) */
    currentSource->count++;
  else
    startConsult(currentSource);
  PL_unregister_atom(fname);		/* locked with sourceFile */

  succeed;
}


static bool
loadPart(IOSTREAM *fd, Module *module, int skip ARG_LD)
{ Module om     = LD->modules.source;
  SourceFile of = currentSource;
  int stchk     = debugstatus.styleCheck;

  switch(Qgetc(fd))
  { case 'M':
    { atom_t mname = loadXR(fd);

      DEBUG(1, Sdprintf("Loading module %s\n", PL_atom_chars(mname)));

      switch( Qgetc(fd) )
      { case '-':
	{ LD->modules.source = lookupModule(mname);
					/* TBD: clear module? */
	  DEBUG(1, Sdprintf("\tNo source\n"));
	  break;
	}
	case 'F':
	{ Module m;

	  qlfLoadSource(fd);
	  DEBUG(1, Sdprintf("\tSource = %s\n",
			    PL_atom_chars(currentSource->name)));

	  m = lookupModule(mname);
	  if ( m->file && m->file != currentSource )
	  { warning("%s:\n\tmodule \"%s\" already loaded from \"%s\" (skipped)",
		    wicFile, stringAtom(m->name), stringAtom(m->file->name));
	    skip = TRUE;
	    LD->modules.source = m;
	  } else
	  { if ( !declareModule(mname, currentSource, 0, FALSE) ) /* TBD: line */
	      fail;
	  }

	  if ( module )
	    *module = LD->modules.source;

	  for(;;)
	  { switch(Qgetc(fd))
	    { case 'E':
	      { functor_t f = (functor_t) loadXR(fd);

		if ( !skip )
		{ Procedure proc = lookupProcedure(f, LD->modules.source);

		  addHTable(LD->modules.source->public, (void *)f, proc);
		} else
		{ if ( !lookupHTable(m->public, (void *)f) )
		  { FunctorDef fd = valueFunctor(f);

		    warning("%s: skipped module \"%s\" lacks %s/%d",
			    wicFile,
			    stringAtom(m->name),
			    stringAtom(fd->name),
			    fd->arity);
		  }
		}

		continue;
	      }
	      case 'X':
		break;
	      default:
		return qlfLoadError(fd, "loadPart()");
	    }
	    break;
	  }
	  break;
	}
	default:
	  qlfLoadError(fd, "loadPart()");
	  break;
      }
      break;
    }
    case 'F':
    { qlfLoadSource(fd);

      if ( module )
	*module = NULL;

      break;
    }
    default:
      return qlfLoadError(fd, "loadPart()");
  }

  for(;;)
  { int c = Qgetc(fd);

    switch(c)
    { case 'X':
      { LD->modules.source = om;
	currentSource  = of;
	debugstatus.styleCheck = stchk;
	systemMode(debugstatus.styleCheck & DOLLAR_STYLE);

	succeed;
      }
      default:
	loadStatement(c, fd, skip PASS_LD);
    }
  }
}


static bool
loadInModule(IOSTREAM *fd, int skip ARG_LD)
{ word mname = loadXR(fd);
  Module om = LD->modules.source;

  LD->modules.source = lookupModule(mname);

  for(;;)
  { int c = Qgetc(fd);

    switch(c)
    { case 'X':
      { LD->modules.source = om;
	succeed;
      }
      default:
	loadStatement(c, fd, skip PASS_LD);
    }
  }
}


		 /*******************************
		 *	WRITING .QLF FILES	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The code below handles the creation of `wic' files.  It offers a  number
of  predicates  which  enables  us  to write the compilation toplevel in
Prolog.

Note that we keep track of the `current procedure' to keep  all  clauses
of a predicate together.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Table	savedXRTable;		/* saved XR entries */
static intptr_t	savedXRTableId;		/* next id */
static intptr_t registered_atoms;	/* safety check */

#define STR_NOLEN ((size_t)-1)

static void
putString(const char *s, size_t len, IOSTREAM *fd)
{ const char *e;

  if ( len == STR_NOLEN )
    len = strlen(s);
  e = &s[len];

  putNum(len, fd);
  while(s<e)
  { Sputc(*s, fd);
    s++;
  }
}


static void
putStringW(const pl_wchar_t *s, size_t len, IOSTREAM *fd)
{ const pl_wchar_t *e;
  IOENC oenc = fd->encoding;

  if ( len == STR_NOLEN )
    len = wcslen(s);
  e = &s[len];

  putNum(len, fd);
  fd->encoding = ENC_UTF8;
  while(s<e)
  { Sputcode(*s, fd);
    s++;
  }
  fd->encoding = oenc;
}


static void
putAtom(atom_t w, IOSTREAM *fd)
{ Atom a = atomValue(w);
  static PL_blob_t *text_blob;

  if ( !text_blob )
    text_blob = PL_find_blob_type("text");

  if ( a->type != text_blob )
  { Sputc(XR_BLOB, fd);
    saveXRBlobType(a->type, fd);
    if ( a->type->save )
    { (*a->type->save)(a->atom, fd);
    } else
    { putString(a->name, a->length, fd);
    }
  } else
  { Sputc(XR_ATOM, fd);
    putString(a->name, a->length, fd);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Number encoding:

First byte:  bits 8&7  bits 1-6 (low order)

		0	6-bits signed value
		1      14-bits signed value
		2      22-bits signed value
		3      number of bytes following
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
putNum(int64_t n, IOSTREAM *fd)
{ int m;
  int64_t absn = (n >= 0 ? n : -n);

  DEBUG(8, Sdprintf("0x%x at %ld\n", (uintptr_t)n, Stell(fd)));

  if ( n != PLMININT )
  { if ( absn < (1L << 5) )
    { Sputc((int)(n & 0x3f), fd);
      return;
    } else if ( absn < (1L << 13) )
    { Sputc((int)(((n >> 8) & 0x3f) | (1 << 6)), fd);
      Sputc((int)(n & 0xff), fd);
      return;
    } else if ( absn < (1L << 21) )
    { Sputc((int)(((n >> 16) & 0x3f) | (2 << 6)), fd);
      Sputc((int)((n >> 8) & 0xff), fd);
      Sputc((int)(n & 0xff), fd);
      return;
    }
  }

  for(m = sizeof(n); ; m--)
  { int b = (int)(absn >> (((m-1)*8)-1)) & 0x1ff;

    if ( b == 0 )
      continue;
    break;
  }

  Sputc(m | (3 << 6), fd);

  for( ; m > 0; m--)
  { int b = (int)(n >> ((m-1)*8)) & 0xff;

    Sputc(b, fd);
  }
}


static void
putFloat(double f, IOSTREAM *fd)
{ unsigned char *cl = (unsigned char *)&f;
  unsigned int i;

  DEBUG(3, Sdprintf("putFloat(%f)\n", f));

  for(i=0; i<BYTES_PER_DOUBLE; i++)
    Sputc(cl[double_byte_order[i]], fd);
}


static void
putInt32(int v, IOSTREAM *fd)
{ Sputc((v>>24)&0xff, fd);
  Sputc((v>>16)&0xff, fd);
  Sputc((v>>8)&0xff, fd);
  Sputc(v&0xff, fd);
}


static void
freeXRSymbol(Symbol s)
{ word w = (word)s->name;

  if ( w&0x1 )
  { w &= ~0x1;
    if ( isAtom(w) )
    { registered_atoms--;
      PL_unregister_atom(w);
      DEBUG(5, Sdprintf("UNREG: %s\n", stringAtom(w)));
    }
  }
}


void
initXR()
{ currentProc		    = NULL;
  currentSource		    = NULL;
  savedXRTable		    = newHTable(256);
  savedXRTable->free_symbol = freeXRSymbol;
  savedXRTableId	    = 0;
  registered_atoms	    = 0;
}


void
destroyXR()
{ destroyHTable(savedXRTable);
  savedXRTable = NULL;
  assert(registered_atoms==0);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
XR (External Reference)  table  handling.   The  table  contains  atoms,
functors  and  various  types  of    pointers   (Module,  Procedure  and
SourceFile). For savedXR()  to  work,  atom_t   and  functor_t  may  not
conflict with pointers. We assume -as in  many other places in the code-
that pointers are 4-byte aligned.

savedXRConstant()  must  be  used  for    atom_t  and  functor_t,  while
savedXRPointer  must  be  used  for   the    pointers.   The  value  for
savedXRConstant() is or-ed with 0x1 to avoid conflict with pointers.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
savedXR(void *xr, IOSTREAM *fd)
{ Symbol s;
  intptr_t id;

  if ( (s = lookupHTable(savedXRTable, xr)) )
  { id = (intptr_t) s->value;
    Sputc(XR_REF, fd);
    putNum(id, fd);

    succeed;
  } else
  { id = ++savedXRTableId;
    addHTable(savedXRTable, xr, (void *)id);
  }

  fail;
}


static inline int
savedXRConstant(word w, IOSTREAM *fd)
{ int rc;

  assert(tag(w) == TAG_ATOM);		/* Only functor_t and atom_t */

  if ( !(rc=savedXR((void *)(w|0x1), fd)) && isAtom(w) )
  { registered_atoms++;
    DEBUG(5, Sdprintf("REG: %s\n", stringAtom(w)));
    PL_register_atom(w);
  }

  return rc;
}


static inline int
savedXRPointer(void *p, IOSTREAM *fd)
{ assert(((word)p & 0x1) == 0);

  return savedXR(p, fd);
}


static void
saveXR__LD(word xr, IOSTREAM *fd ARG_LD)
{ if ( isTaggedInt(xr) )		/* TBD: switch */
  { Sputc(XR_INT, fd);
    putNum(valInt(xr), fd);
    return;
  } else if ( isBignum(xr) )
  { Sputc(XR_INT, fd);
    putNum(valBignum(xr), fd);
    return;
  } else if ( isFloat(xr) )
  { Sputc(XR_FLOAT, fd);
    putFloat(valFloat(xr), fd);
    return;
#if O_STRING
  } else if ( isString(xr) )
  { char *s;
    pl_wchar_t *w;
    size_t len;

    if ( (s = getCharsString(xr, &len)) )
    { Sputc(XR_STRING, fd);
      putString(s, len, fd);
    } else if ( (w=getCharsWString(xr, &len)) )
    { Sputc(XR_STRING_UTF8, fd);
      putStringW(w, len, fd);
    }
    return;
#endif /* O_STRING */
  }

  if ( savedXRConstant(xr, fd) )
    return;

  if ( isAtom(xr) )
  { DEBUG(3, Sdprintf("XR(%d) = '%s'\n", savedXRTableId, stringAtom(xr)));
    putAtom(xr, fd);
    return;
  }

  assert(0);
}
#define saveXR(xr, s) saveXR__LD(xr, s PASS_LD)


static void
saveXRBlobType(PL_blob_t *type, IOSTREAM *fd)
{ if ( savedXRPointer(type, fd) )
    return;

  Sputc(XR_BLOB_TYPE, fd);
  putString(type->name, STR_NOLEN, fd);
}


static void
saveXRModule(Module m, IOSTREAM *fd ARG_LD)
{ if ( savedXRPointer(m, fd) )
    return;

  Sputc(XR_MODULE, fd);
  DEBUG(3, Sdprintf("XR(%d) = module %s\n", savedXRTableId, stringAtom(m->name)));
  saveXR(m->name, fd);
}


static void
saveXRFunctor(functor_t f, IOSTREAM *fd ARG_LD)
{ FunctorDef fdef;

  if ( savedXRConstant(f, fd) )
    return;

  fdef = valueFunctor(f);

  DEBUG(3, Sdprintf("XR(%d) = %s/%d\n",
		savedXRTableId, stringAtom(fdef->name), fdef->arity));
  Sputc(XR_FUNCTOR, fd);
  saveXR(fdef->name, fd);
  putNum(fdef->arity, fd);
}


static void
saveXRProc(Procedure p, IOSTREAM *fd ARG_LD)
{ if ( savedXRPointer(p, fd) )
    return;

  DEBUG(3, Sdprintf("XR(%d) = proc %s\n", savedXRTableId, procedureName(p)));
  Sputc(XR_PRED, fd);
  saveXRFunctor(p->definition->functor->functor, fd PASS_LD);
  saveXRModule(p->definition->module, fd PASS_LD);
}


static void
saveXRSourceFile(SourceFile f, IOSTREAM *fd ARG_LD)
{ if ( savedXRPointer(f, fd) )
    return;

  Sputc(XR_FILE, fd);

  if ( f )
  { DEBUG(3, Sdprintf("XR(%d) = file %s\n", savedXRTableId, stringAtom(f->name)));
    Sputc(f->system ? 's' : 'u', fd);
    saveXR(f->name, fd);
    putNum(f->time, fd);
  } else
  { DEBUG(3, Sdprintf("XR(%d) = <no file>\n", savedXRTableId));
    Sputc('-', fd);
  }
}



static void
do_save_qlf_term(Word t, IOSTREAM *fd ARG_LD)
{ deRef(t);

  if ( isTerm(*t) )
  { functor_t f = functorTerm(*t);

    if ( f == FUNCTOR_var1 )
    { int id = (int)valInt(argTerm(*t, 0));

      Sputc('v', fd);
      putNum(id, fd);
    } else
    { Word q = argTermP(*t, 0);
      int n, arity = arityFunctor(f);

      Sputc('t', fd);
      saveXRFunctor(f, fd PASS_LD);
      for(n=0; n < arity; n++, q++)
	do_save_qlf_term(q, fd PASS_LD);
    }
  } else
  { assert(isAtomic(*t));
    saveXR(*t, fd);
  }
}


static void
saveQlfTerm(term_t t, IOSTREAM *fd ARG_LD)
{ int nvars;
  fid_t cid;
  nv_options options;

  cid = PL_open_foreign_frame();

  DEBUG(3,
	Sdprintf("Saving ");
	PL_write_term(Serror, t, 1200, 0);
	Sdprintf(" from %d ... ", Stell(fd)));
  options.functor = FUNCTOR_var1;
  options.on_attvar = AV_SKIP;
  options.singletons = FALSE;		/* TBD: TRUE may be better! */
  nvars = numberVars(t, &options, 0 PASS_LD);
  putNum(nvars, fd);
  do_save_qlf_term(valTermRef(t), fd PASS_LD);	/* TBD */
  DEBUG(3, Sdprintf("to %d\n", Stell(fd)));

  PL_discard_foreign_frame(cid);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
saveWicClause()  saves  a  clause  to  the  .qlf  file.   For  predicate
references of I_CALL and I_DEPART, we  cannot store the predicate itself
as this would lead to an inconsistency if   the .qlf file is loaded into
another context module.  Therefore we just   store the functor.  For now
this is ok as constructs of the   form  module:goal are translated using
the meta-call mechanism.  This needs consideration   if we optimise this
(which is not that likely as I	think  module:goal, where `module' is an
atom,  should  be  restricted  to  very    special  cases  and  toplevel
interaction.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
saveWicClause(Clause clause, IOSTREAM *fd)
{ GET_LD
  Code bp, ep;

  Sputc('C', fd);
  putNum(clause->code_size, fd);
  putNum(clause->line_no, fd);
  saveXRSourceFile(indexToSourceFile(clause->source_no), fd PASS_LD);
  putNum(clause->prolog_vars, fd);
  putNum(clause->variables, fd);
  putNum(true(clause, UNIT_CLAUSE) ? 0 : 1, fd);

  bp = clause->codes;
  ep = bp + clause->code_size;

  while( bp < ep )
  { code op = decode(*bp++);
    const char *ats = codeTable[op].argtype;
    int n;

    putNum(op, fd);
    DEBUG(3, Sdprintf("\t%s at %ld\n", codeTable[op].name, Stell(fd)));
    for(n=0; ats[n]; n++)
    { switch(ats[n])
      { case CA1_PROC:
	{ Procedure p = (Procedure) *bp++;
	  saveXRProc(p, fd PASS_LD);
	  break;
	}
	case CA1_MODULE:
	{ Module m = (Module) *bp++;
	  saveXRModule(m, fd PASS_LD);
	  break;
	}
	case CA1_FUNC:
	{ functor_t f = (functor_t) *bp++;
	  saveXRFunctor(f, fd PASS_LD);
	  break;
	}
	case CA1_DATA:
	{ word xr = (word) *bp++;
	  saveXR(xr, fd);
	  break;
	}
	case CA1_INTEGER:
	case CA1_JUMP:
	case CA1_VAR:
	case CA1_CHP:
	case CA1_AFUNC:
	{ putNum(*bp++, fd);
	  break;
	}
	case CA1_INT64:
	{ int64_t val;
	  Word p = (Word)&val;

	  cpInt64Data(p, bp);
	  putNum(val, fd);
	  break;
	}
	case CA1_FLOAT:
	{ union
	  { word w[WORDS_PER_DOUBLE];
	    double f;
	  } v;
	  Word p = v.w;
	  cpDoubleData(p, bp);
	  putFloat(v.f, fd);
	  break;
	}
	case CA1_STRING:
	{ word m = *bp;
	  char *s = (char *)++bp;
	  size_t wn = wsizeofInd(m);
	  size_t l = wn*sizeof(word) - padHdr(m);
	  bp += wn;

	  putNum(l, fd);
	  while(l-- > 0)
	    Sputc(*s++&0xff, fd);
	  break;
	}
#ifdef O_GMP
	case CA1_MPZ:
	{ word m = *bp++;
	  size_t wn = wsizeofInd(m);
	  int mpsize = (int)*bp;
	  int l = abs(mpsize)*sizeof(mp_limb_t);
	  char *s = (char*)&bp[1];
	  bp += wn;

	  DEBUG(3, Sdprintf("Saving MPZ from %ld\n", Stell(fd)));
	  putNum(mpsize, fd);
	  while(--l >= 0)
	    Sputc(*s++&0xff, fd);
	  DEBUG(3, Sdprintf("Saved MPZ to %ld\n", Stell(fd)));
	  break;
	}
#endif
	default:
	  fatalError("No support for VM argtype %d (arg %d of %s)",
		     ats[n], n, codeTable[op].name);
      }
    }
  }
}


		/********************************
		*         COMPILATION           *
		*********************************/

static void
closeProcedureWic(IOSTREAM *fd)
{ if ( currentProc != (Procedure) NULL )
  { Sputc('X', fd);
    putNum(currentProc->definition->indexPattern & ~NEED_REINDEX, fd);
    currentProc = (Procedure) NULL;
  }
}


static int
predicateFlags(Definition def, atom_t sclass)
{ int flags = 0;

  if ( sclass == ATOM_kernel )
  { if ( true(def, SYSTEM) && false(def, HIDE_CHILDS) )
      return PRED_SYSTEM;
    return (PRED_SYSTEM|PRED_HIDE_CHILDS);
  }

  if ( true(def, SYSTEM) )
    flags |= PRED_SYSTEM;
  if ( true(def, HIDE_CHILDS) )
    flags |= PRED_HIDE_CHILDS;

  return flags;
}


static void
openProcedureWic(Procedure proc, IOSTREAM *fd, atom_t sclass ARG_LD)
{ if ( proc != currentProc)
  { Definition def = proc->definition;
    int mode = predicateFlags(def, sclass);

    closeProcedureWic(fd);
    currentProc = proc;

    if ( def->module != LD->modules.source )
    { Sputc('O', fd);
      saveXR(def->module->name, fd);
    } else
    { Sputc('P', fd);
    }

    saveXRFunctor(def->functor->functor, fd PASS_LD);
    putNum(mode, fd);
  }
}


static bool
putMagic(const char *s, IOSTREAM *fd)
{ for(; *s; s++)
    Sputc(*s, fd);
  Sputc(EOS, fd);

  succeed;
}


static bool
writeWicHeader(IOSTREAM *fd)
{ wicFd = fd;

  putMagic(saveMagic, fd);
  putNum(VERSION, fd);
  putNum(VM_SIGNATURE, fd);
  putNum(sizeof(word)*8, fd);	/* bits-per-word */
  if ( systemDefaults.home )
    putString(systemDefaults.home, STR_NOLEN, fd);
  else
    putString("<no home>",  STR_NOLEN, fd);

  initXR();

  DEBUG(2, Sdprintf("Header complete ...\n"));
  succeed;
}


static bool
writeWicTrailer(IOSTREAM *fd)
{ if ( !fd )
    fail;

  closeProcedureWic(fd);
  Sputc('X', fd);
  destroyXR();
  Sputc('T', fd);

  wicFd = NULL;
  wicFile = NULL;

  succeed;
}

static bool
addClauseWic(term_t term, atom_t file ARG_LD)
{ Clause clause;
  sourceloc loc;

  loc.file = file;
  loc.line = source_line_no;

  if ( (clause = assert_term(term, CL_END, &loc PASS_LD)) )
  { IOSTREAM *s = wicFd;

    openProcedureWic(clause->procedure, s, ATOM_development PASS_LD);
    saveWicClause(clause, s);

    succeed;
  }

  Sdprintf("Failed to compile: "); pl_write(term); Sdprintf("\n");
  fail;
}

static bool
addDirectiveWic(term_t term, IOSTREAM *fd ARG_LD)
{ closeProcedureWic(fd);
  Sputc('D', fd);
  putNum(source_line_no, fd);
  saveQlfTerm(term, fd PASS_LD);

  succeed;
}


static bool
importWic(Procedure proc, IOSTREAM *fd ARG_LD)
{ closeProcedureWic(fd);

  Sputc('I', fd);
  saveXRProc(proc, fd PASS_LD);

  succeed;
}

		 /*******************************
		 *	    PART MARKS		*
		 *******************************/

typedef struct source_mark *SourceMark;

struct source_mark
{ long		file_index;
  SourceMark	next;
};

static SourceMark source_mark_head = NULL;
static SourceMark source_mark_tail = NULL;

static void
initSourceMarks()
{ source_mark_head = source_mark_tail = NULL;
}


static void
sourceMark(IOSTREAM *s ARG_LD)
{ SourceMark pm = allocHeap(sizeof(struct source_mark));

  pm->file_index = Stell(s);
  pm->next = NULL;
  if ( source_mark_tail )
  { source_mark_tail->next = pm;
    source_mark_tail = pm;
  } else
  { source_mark_tail = source_mark_head = pm;
  }
}


static int
writeSourceMarks(IOSTREAM *s ARG_LD)
{ long n = 0;
  SourceMark pn, pm = source_mark_head;

  DEBUG(1, Sdprintf("Writing source marks: "));

  for( ; pm; pm = pn )
  { pn = pm->next;

    DEBUG(1, Sdprintf(" %d", pm->file_index));
    putInt32(pm->file_index, s);
    freeHeap(pm, sizeof(*pm));
    n++;
  }
  source_mark_head = source_mark_tail = NULL;

  DEBUG(1, Sdprintf("Written %d marks\n", n));
  putInt32(n, s);

  return 0;
}


static int
qlfSourceInfo(IOSTREAM *s, size_t offset, term_t list ARG_LD)
{ char *str;
  term_t head = PL_new_term_ref();
  atom_t fname;

  if ( Sseek(s, offset, SIO_SEEK_SET) != 0 )
    return warning("%s: seek failed: %s", wicFile, OsError());
  if ( Sgetc(s) != 'F' || !(str=getString(s, NULL)) )
    return warning("QLF format error");
  fname = qlfFixSourcePath(str);

  return PL_unify_list(list, head, list) &&
         PL_unify_atom(head, fname);
}


static word
qlfInfo(const char *file,
	term_t cversion, term_t version, term_t wsize,
	term_t files0 ARG_LD)
{ IOSTREAM *s = NULL;
  int lversion;
  int nqlf, i;
  size_t *qlfstart = NULL;
  word rval = TRUE;
  term_t files = PL_copy_term_ref(files0);
  int saved_wsize;
  int vm_signature;

  TRY(PL_unify_integer(cversion, VERSION));

  wicFile = (char *)file;

  if ( !(s = Sopen_file(file, "rbr")) )
  { term_t f = PL_new_term_ref();

    PL_put_atom_chars(f, file);
    return PL_error(NULL, 0, OsError(), ERR_FILE_OPERATION,
		    ATOM_open, ATOM_source_sink, f);
  }

  if ( !(lversion = qlfVersion(s)) )
  { Sclose(s);
    fail;
  }
  TRY(PL_unify_integer(version, lversion));

  vm_signature = getInt(s);		/* TBD: provide to Prolog layer */
  saved_wsize = getInt(s);		/* word-size of file */
  TRY(PL_unify_integer(wsize, saved_wsize));

  pushPathTranslation(s, file, 0);

  if ( Sseek(s, -4, SIO_SEEK_END) < 0 )	/* 4 bytes of PutInt32() */
    return warning("qlf_info/4: seek failed: %s", OsError());
  nqlf = (int)getInt32(s);
  DEBUG(1, Sdprintf("Found %d sources at", nqlf));
  qlfstart = (size_t*)allocHeap(sizeof(long) * nqlf);
  Sseek(s, -4 * (nqlf+1), SIO_SEEK_END);
  for(i=0; i<nqlf; i++)
  { qlfstart[i] = (size_t)getInt32(s);
    DEBUG(1, Sdprintf(" %ld", qlfstart[i]));
  }
  DEBUG(1, Sdprintf("\n"));

  for(i=0; i<nqlf; i++)
  { if ( !qlfSourceInfo(s, qlfstart[i], files PASS_LD) )
    { rval = FALSE;
      goto out;
    }
  }

  rval = PL_unify_nil(files);
  popPathTranslation();

out:
  if ( qlfstart )
    freeHeap(qlfstart, sizeof(*qlfstart) * nqlf);
  if ( s )
    Sclose(s);

  return rval;
}


static
PRED_IMPL("$qlf_info", 5, qlf_info, 0)
{ PRED_LD
  char *name;

  if ( !PL_get_file_name(A1, &name, PL_FILE_ABSOLUTE) )
    fail;

  return qlfInfo(name, A2, A3, A4, A5 PASS_LD);
}



		 /*******************************
		 *	NEW MODULE SUPPORT	*
		 *******************************/

static bool
qlfOpen(atom_t name)
{ char *absname;
  char tmp[MAXPATHLEN];

  wicFile = stringAtom(name);
  if ( !(absname = AbsoluteFile(wicFile, tmp)) )
    fail;

  if ( !(wicFd = Sopen_file(wicFile, "wbr")) )
    return warning("qlf_open/1: can't open %s: %s", wicFile, OsError());

  mkWicFile = wicFile;

  putMagic(qlfMagic, wicFd);
  putNum(VERSION, wicFd);
  putNum(VM_SIGNATURE, wicFd);
  putNum(sizeof(word)*8, wicFd);

  putString(absname, STR_NOLEN, wicFd);
  initXR();
  initSourceMarks();

  succeed;
}


static bool
qlfClose(ARG1_LD)
{ IOSTREAM *fd = wicFd;

  closeProcedureWic(fd);
  writeSourceMarks(fd PASS_LD);
  Sclose(fd);
  wicFd = NULL;
  mkWicFile = NULL;
  destroyXR();

  succeed;
}


static int
qlfVersion(IOSTREAM *s)
{ char mbuf[100];
  char *magic;

  if ( !(magic = getMagicString(s, mbuf, sizeof(mbuf))) ||
       !streq(magic, qlfMagic) )
  { Sclose(s);
    return warning("%s: not a SWI-Prolog .qlf file", wicFile);
  }

  return getInt(s);
}


static int
pushPathTranslation(IOSTREAM *fd, const char *absloadname, int flags)
{ GET_LD
  char *abssavename;
  qlf_state *new = allocHeap(sizeof(*new));

  memset(new, 0, sizeof(*new));
  new->previous = load_state;
  load_state = new;

  abssavename = getString(fd, NULL);
  if ( absloadname && !streq(absloadname, abssavename) )
  { char load[MAXPATHLEN];
    char save[MAXPATHLEN];
    char *l, *s, *le, *se;

    new->has_moved = TRUE;

    if ( (flags & PATH_ISDIR) )
    { l = strcpy(load, absloadname);
      s = strcpy(save, abssavename);
    } else
    { l = DirName(absloadname, load);
      s = DirName(abssavename, save);
    }
    le = l+strlen(l);
    se = s+strlen(s);
    for( ;le>l && se>s && le[-1] == se[-1]; le--, se--)
    { if ( le[-1] == '/' )
      { *le = EOS;
        *se = EOS;
      }
    }

    new->load_dir = store_string(l);
    new->save_dir = store_string(s);
    DEBUG(1, Sdprintf("QLF file has moved; replacing %s --> %s\n",
		      load_state->save_dir, load_state->load_dir));
  }

  succeed;
}


static void
popPathTranslation()
{ GET_LD

  if ( load_state )
  { qlf_state *old = load_state;

    load_state = load_state->previous;

    if ( old->has_moved )
    { remove_string(old->load_dir);
      remove_string(old->save_dir);
      freeHeap(old, sizeof(*old));
    }
  }
}

static bool
qlfLoad(IOSTREAM *fd, Module *module ARG_LD)
{ bool rval;
  int lversion;
  const char *absloadname;
  char tmp[MAXPATHLEN];
  int saved_wsize;
  int vm_signature;
  atom_t file;

  if ( (file = fileNameStream(fd)) )
  { PL_chars_t text;

    if ( !get_atom_text(file, &text) )
      fail;
    if ( !PL_mb_text(&text, REP_FN) )
    { PL_free_text(&text);
      fail;
    }
    wicFile = text.text.t;
    if ( !(absloadname = AbsoluteFile(wicFile, tmp)) )
      fail;
    PL_free_text(&text);
  } else
  { absloadname = NULL;
  }

  if ( !(lversion = qlfVersion(fd)) || lversion < LOADVERSION )
  { if ( lversion )
      warning("$qlf_load/1: %s bad version (file version = %d, prolog = %d)",
	      wicFile, lversion, VERSION);
    fail;
  }
  vm_signature = getInt(fd);
  if ( vm_signature != (int)VM_SIGNATURE )
  { warning("QLF file %s has incompatible VM-signature (0x%x; expected 0x%x)",
	    stringAtom(file),
	    (unsigned int)vm_signature,
	    (unsigned int)VM_SIGNATURE);
    fail;
  }
  saved_wsize = getInt(fd);
  if ( saved_wsize != sizeof(word)*8 )
  { warning("QLF file %s has incompatible (%d) word-length",
	    stringAtom(file), (int)saved_wsize);
    fail;
  }

  pushPathTranslation(fd, absloadname, 0);
  load_state->saved_version = lversion;
  if ( Qgetc(fd) != 'Q' )
    return qlfLoadError(fd, "qlfLoad()");

  pushXrIdTable(PASS_LD1);
  rval = loadPart(fd, module, FALSE PASS_LD);
  popXrIdTable(PASS_LD1);
  popPathTranslation();

  return rval;
}


static bool
qlfSaveSource(SourceFile f, IOSTREAM *fd ARG_LD)
{ Atom a = atomValue(f->name);

  sourceMark(fd PASS_LD);
  Sputc('F', fd);
  putString(a->name, a->length, fd);
  putNum(f->time, fd);
  Sputc(f->system ? 's' : 'u', fd);

  currentSource = f;

  succeed;
}


static bool
qlfStartModule(Module m, IOSTREAM *fd ARG_LD)
{ closeProcedureWic(fd);
  Sputc('Q', fd);
  Sputc('M', fd);
  saveXR(m->name, fd);
  if ( m->file )
    qlfSaveSource(m->file, fd PASS_LD);
  else
    Sputc('-', fd);

  DEBUG(2, Sdprintf("MODULE %s\n", stringAtom(m->name)));
  for_unlocked_table(m->public, s,
		     { functor_t f = (functor_t)s->name;

		       DEBUG(2, Sdprintf("Exported %s/%d\n",
					 stringAtom(nameFunctor(f)),
					 arityFunctor(f)));
		       Sputc('E', fd);
		       saveXRFunctor(f, fd PASS_LD);
		     })

  Sputc('X', fd);

  succeed;
}


static bool
qlfStartSubModule(Module m, IOSTREAM *fd ARG_LD)
{ closeProcedureWic(fd);
  Sputc('M', fd);
  saveXR(m->name, fd);

  succeed;
}


static bool
qlfStartFile(SourceFile f, IOSTREAM *fd ARG_LD)
{ closeProcedureWic(fd);
  Sputc('Q', fd);
  qlfSaveSource(f, fd PASS_LD);

  succeed;
}


static bool
qlfEndPart(IOSTREAM  *fd)
{ closeProcedureWic(fd);
  Sputc('X', fd);

  succeed;
}


word
pl_qlf_start_module(term_t name)
{ if ( wicFd )
  { GET_LD
    Module m;

    if ( !PL_get_module_ex(name, &m) )
      fail;

    return qlfStartModule(m, wicFd PASS_LD);
  }

  succeed;
}


word
pl_qlf_start_sub_module(term_t name)
{ if ( wicFd )
  { GET_LD
    Module m;

    if ( !PL_get_module_ex(name, &m) )
      fail;

    return qlfStartSubModule(m, wicFd PASS_LD);
  }

  succeed;
}


word
pl_qlf_start_file(term_t name)
{ if ( wicFd )
  { GET_LD
    atom_t a;

    if ( !PL_get_atom_ex(name, &a) )
      fail;

    return qlfStartFile(lookupSourceFile(a, TRUE), wicFd PASS_LD);
  }

  succeed;
}


word
pl_qlf_end_part()
{ if ( wicFd )
  { return qlfEndPart(wicFd);
  }

  succeed;
}


word
pl_qlf_open(term_t file)
{ atom_t a;

  if ( PL_get_atom_ex(file, &a) )
    return qlfOpen(a);

  fail;
}


word
pl_qlf_close()
{ GET_LD
  return qlfClose(PASS_LD1);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
$qlf_load(:Stream, -ModuleOut)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static
PRED_IMPL("$qlf_load", 2, qlf_load, PL_FA_TRANSPARENT)
{ GET_LD
  term_t qstream = A1;
  term_t module = A2;
  Module m, oldsrc = LD->modules.source;
  bool rval;
  term_t stream = PL_new_term_ref();
  IOSTREAM *fd;
  IOENC saved_enc;

  m = oldsrc;
  if ( !PL_strip_module(qstream, &m, stream) )
    fail;
  if ( !PL_get_stream_handle(stream, &fd) )
    fail;

  saved_enc = fd->encoding;
  fd->encoding = ENC_OCTET;
  LD->modules.source = m;
  rval = qlfLoad(fd, &m PASS_LD);
  LD->modules.source = oldsrc;
  fd->encoding = saved_enc;

  if ( rval )
  { if ( m )
      return PL_unify_atom(module, m->name);

    return PL_unify_integer(module, 0);
  }

  fail;
}


		/********************************
		*        PROLOG SUPPORT         *
		*********************************/

word
pl_open_wic(term_t to)
{ IOSTREAM *fd;

  if ( PL_get_stream_handle(to, &fd) )
  { wicFd = fd;
    writeWicHeader(fd);
    succeed;
  }

  fail;					/* PL_get_stream_handle() */
					/* throws exception */
}

word
pl_close_wic()
{ IOSTREAM *fd = wicFd;

  if ( fd )
  { writeWicTrailer(fd);
    wicFd = NULL;

    succeed;
  }

  fail;
}


word
pl_add_directive_wic(term_t term)
{ if ( wicFd )
  { GET_LD
    if ( !(PL_is_compound(term) || PL_is_atom(term)) )
      return PL_error("$add_directive_wic", 1, NULL, ERR_TYPE,
		      ATOM_callable, term);

    return addDirectiveWic(term, wicFd PASS_LD);
  }

  succeed;
}


word
pl_import_wic(term_t module, term_t pi)
{ if ( wicFd )
  { GET_LD
    Module m = NULL;
    functor_t fd;

    if ( !PL_get_module(module, &m) ||
	 !get_functor(pi, &fd, &m, 0, GF_PROCEDURE) )
      fail;

    return importWic(lookupProcedure(fd, m), wicFd PASS_LD);
  }

  succeed;
}


word
pl_qlf_assert_clause(term_t ref, term_t saveclass)
{ if ( wicFd )
  { GET_LD
    Clause clause;
    IOSTREAM *s = wicFd;
    atom_t sclass;

    if ( !get_clause_ptr_ex(ref, &clause) )
      fail;
    if ( !PL_get_atom_ex(saveclass, &sclass) )
      fail;

    openProcedureWic(clause->procedure, s, sclass PASS_LD);
    saveWicClause(clause, s);
  }

  succeed;
}


		/********************************
		*     BOOTSTRAP COMPILATION     *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The code below offers a restricted compilation  toplevel  used  for  the
bootstrap  compilation  (-b  option).  It handles most things the Prolog
defined compiler handles as well, except:

  - Be carefull to define  a  predicate  first  before  using  it  as  a
    directive
  - It does not offer `consult', `ensure_loaded' or the  list  notation.
    (there is no way to include other files).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Check whether clause is  of  the  form   :-  directive.  If  so, put the
directive in directive and succeed. If the   term has no explicit module
tag, add one from the current source-module.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
directiveClause(term_t directive, term_t clause, const char *functor)
{ GET_LD
  atom_t name;
  int arity;
  term_t d0 = PL_new_term_ref();
  functor_t f;

  if ( !PL_get_name_arity(clause, &name, &arity) ||
       arity != 1 ||
       !streq(stringAtom(name), functor) )
    fail;

  PL_get_arg(1, clause, d0);
  if ( PL_get_functor(d0, &f) && f == FUNCTOR_colon2 )
    PL_put_term(directive, d0);
  else
  { term_t m = PL_new_term_ref();

    PL_put_atom(m, LD->modules.source->name);
    PL_cons_functor(directive, FUNCTOR_colon2, m, d0);
  }

  succeed;
}

/*  Compile an entire file into intermediate code.

 ** Thu Apr 28 13:44:43 1988  jan@swivax.UUCP (Jan Wielemaker)  */

static bool
compileFile(const char *file)
{ GET_LD
  char tmp[MAXPATHLEN];
  char *path;
  term_t f = PL_new_term_ref();
  atom_t nf;

  DEBUG(1, Sdprintf("Boot compilation of %s\n", file));
  if ( !(path = AbsoluteFile(file, tmp)) )
    fail;
  DEBUG(2, Sdprintf("Expanded to %s\n", path));

  nf = PL_new_atom(path);
  PL_put_atom(f, nf);
  DEBUG(2, Sdprintf("Opening\n"));
  if ( !pl_see(f) )
    fail;
  DEBUG(2, Sdprintf("pl_start_consult()\n"));
  pl_start_consult(f);
  qlfStartFile(lookupSourceFile(nf, TRUE), wicFd PASS_LD);

  for(;;)
  { fid_t	 cid = PL_open_foreign_frame();
    term_t         t = PL_new_term_ref();
    term_t directive = PL_new_term_ref();
    atom_t eof;

    DEBUG(2, Sdprintf("pl_read_clause() -> "));
    PL_put_variable(t);
    if ( !read_clause(Scurin, t PASS_LD) ) /* syntax error */
    { Sdprintf("%s:%d: Syntax error\n",
	       PL_atom_chars(source_file_name),
	       source_line_no);
      continue;
    }
    if ( PL_get_atom(t, &eof) && eof == ATOM_end_of_file )
      break;

    DEBUG(2, PL_write_term(Serror, t, 1200, PL_WRT_NUMBERVARS); pl_nl());

    if ( directiveClause(directive, t, ":-") )
    { DEBUG(1,
	    Sdprintf(":- ");
	    PL_write_term(Serror, directive, 1200, 0);
	    Sdprintf(".\n") );
      addDirectiveWic(directive, wicFd PASS_LD);
      if ( !callProlog(MODULE_user, directive, PL_Q_NODEBUG, NULL) )
	Sdprintf("%s:%d: directive failed\n",
		 PL_atom_chars(source_file_name),
		 source_line_no);
    } else if ( directiveClause(directive, t, "$:-") )
    { DEBUG(1,
	    Sdprintf("$:- ");
	    PL_write_term(Serror, directive, 1200, 0);
	    Sdprintf(".\n"));
      callProlog(MODULE_user, directive, PL_Q_NODEBUG, NULL);
    } else
      addClauseWic(t, nf PASS_LD);

    PL_discard_foreign_frame(cid);
  }

  qlfEndPart(wicFd);
  pl_seen();

  succeed;
}

bool
compileFileList(IOSTREAM *fd, int argc, char **argv)
{ TRY(writeWicHeader(fd));

  systemMode(TRUE);
  PL_set_prolog_flag("autoload", PL_BOOL, FALSE);

  for(;argc > 0; argc--, argv++)
  { if ( streq(argv[0], "-c" ) )
      break;
    compileFile(argv[0]);
  }

  PL_set_prolog_flag("autoload", PL_BOOL, TRUE);
  systemMode(FALSE);

  { predicate_t pred = PL_predicate("$load_additional_boot_files", 0, "user");

    PL_call_predicate(MODULE_user, TRUE, pred, 0);
  }

  return writeWicTrailer(fd);
}

		 /*******************************
		 *	     CLEANUP		*
		 *******************************/

void
qlfCleanup()
{ if ( mkWicFile )
  { printMessage(ATOM_warning,
		 PL_FUNCTOR_CHARS, "qlf", 1,
		   PL_FUNCTOR_CHARS, "removed_after_error", 1,
		     PL_CHARS, mkWicFile);
    RemoveFile(mkWicFile);
    mkWicFile = NULL;
  }

  if ( getstr_buffer )
  { free(getstr_buffer);
    getstr_buffer = NULL;
    getstr_buffer_size = 0;
  }
}

		 /*******************************
		 *	 PUBLIC FUNCTIONS	*
		 *******************************/

void
wicPutNum(int64_t n, IOSTREAM *fd)
{ putNum(n, fd);
}


int64_t
wicGetNum(IOSTREAM *fd)
{ return getInt64(fd);
}


void
wicPutStringW(const pl_wchar_t *w, size_t len, IOSTREAM *fd)
{ putStringW(w, len, fd);
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(wic)
  PRED_DEF("$qlf_info", 5, qlf_info, 0)
  PRED_DEF("$qlf_load", 2, qlf_load, PL_FA_TRANSPARENT)
EndPredDefs
