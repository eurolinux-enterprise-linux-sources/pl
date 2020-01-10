/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2007, University of Amsterdam

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

/*#define O_SECURE 1*/
/*#define O_DEBUG 1*/
#include "pl-incl.h"

#define WORDS_PER_PLINT (sizeof(int64_t)/sizeof(word))

static RecordList lookupRecordList(word);
static RecordList isCurrentRecordList(word);
static void freeRecordRef(RecordRef r);

#define RECORDA 0
#define RECORDZ 1

#define LOCK()   PL_LOCK(L_RECORD)
#define UNLOCK() PL_UNLOCK(L_RECORD)

#undef LD
#define LD LOCAL_LD

void
initRecords(void)
{ GD->tables.record_lists = newHTable(8);
}


/* MT: locked by caller (record())
*/

static RecordList
lookupRecordList(word key)
{ Symbol s;

  if ( (s = lookupHTable(GD->tables.record_lists, (void *)key)) )
  { return s->value;
  } else
  { GET_LD
    RecordList l;

    if ( isAtom(key) )			/* can also be functor_t */
      PL_register_atom(key);
    l = allocHeap(sizeof(*l));
    l->key = key;
    l->type = RECORD_TYPE;
    l->references = 0;
    l->flags = 0;
    l->firstRecord = l->lastRecord = NULL;
    addHTable(GD->tables.record_lists, (void *)key, l);

    return l;
  }
}


static RecordList
isCurrentRecordList(word key)
{ Symbol s;

  if ( (s = lookupHTable(GD->tables.record_lists, (void *)key)) )
    return s->value;

  return NULL;
}


/* MT: Locked by called
*/

static void
cleanRecordList(RecordList rl)
{ RecordRef *p;
  RecordRef r, prev=NULL;

  for(p = &rl->firstRecord; (r=*p); )
  { if ( true(r->record, ERASED) )
    { *p = r->next;
      if ( r == rl->lastRecord )
	rl->lastRecord = prev;
      freeRecordRef(r);
    } else
    { prev = r;
      p = &r->next;
    }
  }
}


		 /*******************************
		 *	    HEAP STORAGE	*
		 *******************************/


#undef uint
#undef uchar
#define uint unsigned int
#define uchar unsigned char

#ifndef offsetof
#define offsetof(structure, field) ((int) &(((structure *)NULL)->field))
#endif

#define SIZERECORD(flags) \
	((flags & R_DUPLICATE) ? offsetof(struct record, buffer[0]) : \
	                         offsetof(struct record, references)) \

#define dataRecord(r) ((char *)addPointer(r, SIZERECORD(r->flags)))

typedef struct
{ tmp_buffer code;			/* code buffer */
  tmp_buffer vars;			/* variable pointers */
  size_t     size;			/* size on global stack */
  uint	     nvars;			/* # variables */
  int	     external;			/* Allow for external storage */
  int	     lock;			/* lock compiled atoms */
} compile_info, *CompileInfo;

#define	PL_TYPE_VARIABLE	(1)	/* variable */
#define PL_TYPE_ATOM		(2)	/* atom */
#define PL_TYPE_INTEGER	  	(3)	/* big integer */
#define PL_TYPE_TAGGED_INTEGER  (4)	/* tagged integer */
#define PL_TYPE_FLOAT	  	(5)	/* double */
#define PL_TYPE_STRING	  	(6)	/* string */
#define PL_TYPE_COMPOUND	(7)	/* compound term */
#define PL_TYPE_CONS		(8)	/* list-cell */

#define PL_TYPE_EXT_ATOM	(9)	/* External (inlined) atom */
#define PL_TYPE_EXT_COMPOUND	(10)	/* External (inlined) functor */
#define PL_TYPE_EXT_FLOAT	(11)	/* float in standard-byte order */
#define PL_TYPE_ATTVAR		(12)	/* Attributed variable */
#define PL_REC_ALLOCVAR		(13)	/* Allocate a variable on global */
#define PL_REC_CYCLE		(14)	/* cyclic reference */
#define PL_REC_MPZ		(15)	/* GMP integer */

#define addUnalignedBuf(b, ptr, type) \
	do \
	{ if ( (b)->top + sizeof(type) > (b)->max ) \
	    growBuffer((Buffer)b, sizeof(type)); \
	  memcpy((b)->top, ptr, sizeof(type)); \
	  (b)->top += sizeof(type); \
	} while(0)

static inline void
addOpCode(CompileInfo info, int code)
{ addBuffer(&info->code, code, uchar);
  DEBUG(9, Sdprintf("Added %d, now %d big\n",
		    code, sizeOfBuffer(&info->code)));
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
addSizeInt() deals with integers that should  be large enough to specify
the size of an object on the  stack. This counts for variables, arities,
sizes of strings and atoms, etc.

Encoding: 7-bits per byte, MSF. All but the last (LSB) have the 8-th bit
set. This format allows for arbitrary   bit integers and is architecture
independent.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static inline void
addUintBuffer(Buffer b, size_t val)
{ if ( !(val & ~0x7f) )
    addBuffer(b, (uchar)val, uchar);
  else
  { int zips = ((sizeof(val))*8+7-1)/7 - 1;
    int leading = TRUE;

    for(; zips >= 0; zips--)
    { uint d = (uint)((val >> zips*7) & 0x7f);

      if ( d || !leading )
      { if ( zips != 0 )
	  d |= 0x80;
	addBuffer(b, d, uchar);
	leading = FALSE;
      }
    }
  }
}


static inline void
addSizeInt(CompileInfo info, size_t val)
{ addUintBuffer((Buffer)&info->code, val);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Add a signed intptr_t value. First byte   is  number of bytes, remaining are
value-bytes, starting at most-significant.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
addInt64(CompileInfo info, int64_t v)
{ int i = sizeof(v);

  if ( v != PLMININT )
  { int64_t absn = (v >= 0 ? v : -v);
    int64_t mask = (int64_t)-1 << (INT64BITSIZE-9);

    for(; i>1; i--, mask >>= 8)
    { if ( absn & mask )
	break;
    }
  }

  addBuffer(&info->code, i, uchar);

  while( --i >= 0 )
  { int b = (int)(v>>(i*8)) & 0xff;

    addBuffer(&info->code, b, uchar);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Floats. If we are adding floats for external  use they will be stored in
normalised byte-order. Otherwise they are stored verbatim.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef WORDS_BIGENDIAN
static const int double_byte_order[] = { 7,6,5,4,3,2,1,0 };
#else
static const int double_byte_order[] = { 0,1,2,3,4,5,6,7 };
#endif


static inline void
addFloat(CompileInfo info, void *val)
{ if ( info->external )
  { unsigned char *cl = val;
    unsigned int i;

    addOpCode(info, PL_TYPE_EXT_FLOAT);
    for(i=0; i<sizeof(double); i++)
      addBuffer(&info->code, cl[double_byte_order[i]], uchar);
  } else
  { addOpCode(info, PL_TYPE_FLOAT);

    addUnalignedBuf(&info->code, val, double);
  }
}


static inline void
addWord(CompileInfo info, word w)
{ addUnalignedBuf(&info->code, &w, word);
}


static inline void
addChars(CompileInfo info, size_t len, const char *data)
{ addSizeInt(info, len);

  addMultipleBuffer(&info->code, data, len, char);
}


static inline void
addAtomValue(CompileInfo info, atom_t name)
{ Atom a = atomValue(name);

  addSizeInt(info, a->length);
  addMultipleBuffer(&info->code, a->name, a->length, char);
}


static void
addAtom(CompileInfo info, atom_t a)
{ if ( info->external )
  { addOpCode(info, PL_TYPE_EXT_ATOM);
    addAtomValue(info, a);
  } else
  { addOpCode(info, PL_TYPE_ATOM);
    addWord(info, a);
    if ( info->lock )
      PL_register_atom(a);
  }
}


static void
addFunctor(CompileInfo info, functor_t f)
{ if ( f == FUNCTOR_dot2 )
  { addOpCode(info, PL_TYPE_CONS);
  } else
  { if ( info->external )
    { FunctorDef fd = valueFunctor(f);

      addOpCode(info, PL_TYPE_EXT_COMPOUND);
      addSizeInt(info, fd->arity);
      addAtomValue(info, fd->name);
    } else
    { addOpCode(info, PL_TYPE_COMPOUND);
      addWord(info, f);
    }
  }
}


typedef struct
{ Functor	term;
  functor_t	fdef;
} cycle_mark;


#define mkAttVarP(p)  ((Word)((word)(p) | 0x1L))
#define isAttVarP(p)  ((word)(p) & 0x1)
#define valAttVarP(p) ((Word)((word)(p) & ~0x1L))

static void
compile_term_to_heap(Word p, CompileInfo info ARG_LD)
{ word w;

right_recursion:
  w = *p;

  switch(tag(w))
  { case TAG_VAR:
    { intptr_t n = info->nvars++;

      *p = (n<<7)|TAG_ATOM|STG_GLOBAL;
      addBuffer(&info->vars, p, Word);
      addOpCode(info, PL_TYPE_VARIABLE);
      addSizeInt(info, n);

      return;
    }
#if O_ATTVAR
    case TAG_ATTVAR:
    { intptr_t n = info->nvars++;
      Word ap = valPAttVar(w);

      if ( isEmptyBuffer(&info->code) )
      { addOpCode(info, PL_REC_ALLOCVAR); 	/* only an attributed var */
	info->size++;
      }

      addBuffer(&info->vars, *p, word);		/* save value */
      *p = (n<<7)|TAG_ATOM|STG_GLOBAL;
      addBuffer(&info->vars, mkAttVarP(p), Word);
      addOpCode(info, PL_TYPE_ATTVAR);
      addSizeInt(info, n);
      info->size++;
      DEBUG(9, Sdprintf("Added attvar %d\n", n));

      p = ap;
      goto right_recursion;
    }
#endif
    case TAG_ATOM:
    { if ( storage(w) == STG_GLOBAL )	/* this is a variable */
      { intptr_t n = ((intptr_t)(w) >> 7);

	addOpCode(info, PL_TYPE_VARIABLE);
	addSizeInt(info, n);
	DEBUG(9, Sdprintf("Added var-link %d\n", n));
      } else
      { addAtom(info, w);
	DEBUG(9, Sdprintf("Added '%s'\n", stringAtom(w)));
      }

      return;
    }
    case TAG_INTEGER:
    { int64_t val;

      if ( isTaggedInt(w) )
      { val = valInt(w);
	addOpCode(info, PL_TYPE_TAGGED_INTEGER);
	addInt64(info, val);
      } else
      { number n;

	info->size += wsizeofIndirect(w) + 2;

	get_integer(w, &n);
	switch(n.type)
	{ case V_INTEGER:
	    addOpCode(info, PL_TYPE_INTEGER);
	    addInt64(info, n.value.i);
	    break;
#ifdef O_GMP
	  case V_MPZ:
	    addOpCode(info, PL_REC_MPZ);
	    addMPZToBuffer((Buffer)&info->code, n.value.mpz);
	    break;
#endif
  	  default:
	    assert(0);
	}
      }

      return;
    }
    case TAG_STRING:
    { Word f     = addressIndirect(w);
      size_t n   = wsizeofInd(*f);
      size_t pad = padHdr(*f);		/* see also getCharsString() */
      size_t l   = n*sizeof(word)-pad;

      info->size += n+2;
      addOpCode(info, PL_TYPE_STRING);
      addChars(info, l, (const char *)(f+1)); /* +1 to skip header */

      return;
    }
    case TAG_FLOAT:
    { info->size += WORDS_PER_DOUBLE + 2;
      addFloat(info, valIndirectP(w));

      return;
    }
    case TAG_COMPOUND:
    { Functor f = valueTerm(w);
      int arity;
      word functor;

#if O_CYCLIC
      if ( isInteger(f->definition) )
      { addOpCode(info, PL_REC_CYCLE);
	addSizeInt(info, valInt(f->definition));

	DEBUG(1, Sdprintf("Added cycle for offset = %d\n",
			  valInt(f->definition)));

	return;
      } else
      { cycle_mark mark;

	arity   = arityFunctor(f->definition);
	functor = f->definition;

	requireStack(argument, sizeof(Word)*2);
	mark.term = f;
	mark.fdef = f->definition;
	pushSegStack(&LD->cycle.stack, &mark);
	f->definition = (functor_t)consInt(info->size);
	assert(valInt(f->definition) == (intptr_t)info->size); /* overflow test */
      }
#endif

      info->size += arity+1;
      addFunctor(info, functor);
      DEBUG(9, if ( GD->io_initialised )
	         Sdprintf("Added %s/%d\n",
			  stringAtom(valueFunctor(functor)->name),
			  arityFunctor(functor)));
      p = f->arguments;
      for(; --arity > 0; p++)
	compile_term_to_heap(p, info PASS_LD);
      goto right_recursion;
    }
    case TAG_REFERENCE:
      p = unRef(w);
      goto right_recursion;
    default:
      assert(0);
  }
}


#if O_CYCLIC

static void
init_cycle(ARG1_LD)
{ LD->cycle.stack.unit_size = sizeof(cycle_mark);
}


static void
unvisit(ARG1_LD)
{ cycle_mark mark;

  while( popSegStack(&LD->cycle.stack, &mark) )
  { mark.term->definition = mark.fdef;
  }
}

#else

static void init_cycle(ARG1_LD) {}
static void unvisit(ARG1_LD) {}

#endif


Record
compileTermToHeap__LD(term_t t, int flags ARG_LD)
{ compile_info info;
  Record record;
  Word *p;
  size_t size;
  size_t rsize = SIZERECORD(flags);
  init_cycle(PASS_LD1);

  SECURE(checkData(valTermRef(t)));

  initBuffer(&info.code);
  initBuffer(&info.vars);
  info.size = 0;
  info.nvars = 0;
  info.external = (flags & R_EXTERNAL);
  info.lock = !(info.external || (flags&R_NOLOCK));

  compile_term_to_heap(valTermRef(t), &info PASS_LD);
  p = topBuffer(&info.vars, Word);
  while(p > baseBuffer(&info.vars, Word))
  { p--;
    if (isAttVarP(*p) )
    { *valAttVarP(*p) = (word)p[-1];
      p--;
    } else
      setVar(**p);
  }
  discardBuffer(&info.vars);

  unvisit(PASS_LD1);

  size = rsize + sizeOfBuffer(&info.code);
  record = allocHeap(size);
  record->gsize = (unsigned int)info.size; /* only 28-bit */
  record->nvars = info.nvars;
  record->size  = (int)size;
  record->flags = flags;
  if ( flags & R_DUPLICATE )
  { record->references = 1;
  }
  memcpy(addPointer(record, rsize), info.code.base, sizeOfBuffer(&info.code));
  discardBuffer(&info.code);

  DEBUG(3, Sdprintf("--> record at %p\n", record));

  return record;
}

		 /*******************************
		 *	 EXTERNAL RECORDS	*
		 *******************************/

#define	REC_32	    0x01		/* word is 32-bits	*/
#define	REC_64	    0x02		/* word is 64-bits	*/
#define	REC_INT	    0x04		/* Record just contains	int  */
#define	REC_ATOM    0x08		/* Record just contains	atom */
#define	REC_GROUND  0x10		/* Record is ground */
#define	REC_VMASK   0xe0		/* Version mask */
#define REC_VSHIFT     5		/* shift for version mask */
#define	REC_VERSION 0x01		/* Version id */

#define REC_SZMASK  (REC_32|REC_64)	/* SIZE_MASK */

#if SIZEOF_VOIDP == 8
#define REC_SZ REC_64
#else
#define REC_SZ REC_32
#endif

#define REC_HDR		(REC_SZ|(REC_VERSION<<REC_VSHIFT))
#define REC_COMPAT(m)	(((m)&(REC_VMASK|REC_SZMASK)) == REC_HDR)

char *
PL_record_external(term_t t, size_t *len)
{ GET_LD
  compile_info info;
  Word p, *vp;
  int n;
  tmp_buffer hdr;
  int scode, shdr;
  char *rec;
  int first = REC_HDR;

  SECURE(checkData(valTermRef(t)));
  p = valTermRef(t);
  deRef(p);

  init_cycle(PASS_LD1);
  initBuffer(&info.code);

  if ( isInteger(*p) )			/* integer-only record */
  { int64_t v;

    if ( isTaggedInt(*p) )
      v = valInt(*p);
    else
      v = valBignum(*p);

    first |= (REC_INT|REC_GROUND);
    addOpCode(&info, first);
    addInt64(&info, v);

  ret_primitive:
    scode = (int)sizeOfBuffer(&info.code);
    rec = allocHeap(scode);
    memcpy(rec, info.code.base, scode);
    discardBuffer(&info.code);
    *len = scode;
    return rec;
  } else if ( isAtom(*p) )
  { first |= (REC_ATOM|REC_GROUND);
    addOpCode(&info, first);
    addAtomValue(&info, *p);
    goto ret_primitive;
  }

  initBuffer(&info.vars);
  info.size = 0;
  info.nvars = 0;
  info.external = TRUE;
  info.lock = FALSE;

  compile_term_to_heap(p, &info PASS_LD);
  n = info.nvars;
  vp = (Word *)info.vars.base;
  while(--n >= 0)
    setVar(**vp++);
  discardBuffer(&info.vars);
  scode = (int)sizeOfBuffer(&info.code);
  if ( info.nvars == 0 )
    first |= REC_GROUND;

  unvisit(PASS_LD1);

  initBuffer(&hdr);
  addBuffer(&hdr, first, uchar);		/* magic code */
  addUintBuffer((Buffer)&hdr, scode);		/* code size */
  addUintBuffer((Buffer)&hdr, info.size);	/* size on stack */
  if ( info.nvars > 0 )
    addUintBuffer((Buffer)&hdr, info.nvars);	/* Number of variables */
  shdr = (int)sizeOfBuffer(&hdr);

  rec = allocHeap(shdr + scode);
  memcpy(rec, hdr.base, shdr);
  memcpy(rec+shdr, info.code.base, scode);

  discardBuffer(&info.code);
  discardBuffer(&hdr);

  *len = shdr + scode;

  return rec;
}


		 /*******************************
		 *	   HEAP --> STACK	*
		 *******************************/

typedef struct
{ const char   *data;
  const char   *base;			/* start of data */
  Word	       *vars;
  Word		gbase;			/* base of term on global stack */
  Word 		gstore;			/* current storage location */
					/* for se_record() */
  uint  	nvars;			/* Variables seen */
  TmpBuffer 	avars;			/* Values stored for attvars */
} copy_info, *CopyInfo;


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Handle temporary variable  pointers.  Upto   MAX_ALLOCA_VARS  these  are
allocated using alloca() for speed  and avoiding fragmentation. alloca()
for big chunks has problems on various   platforms,  so we'll use normal
heep allocation in this case. We could   also  consider using one of the
other stacks as scratch-area.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define MAX_ALLOCA_VARS 2048		/* most machines should do 8k */
#define INITCOPYVARS(info, n) \
{ if ( (n) > 0 ) \
  { Word *p; \
    uint i; \
    if ( (n) > MAX_ALLOCA_VARS ) \
      info.vars = allocHeap(sizeof(Word) * (n)); \
    else \
    { if ( !(info.vars = alloca(sizeof(Word) * (n))) ) \
	fatalError("alloca() failed"); \
    } \
    for(p = info.vars, i=(n)+1; --i > 0;) \
      *p++ = 0; \
  } \
}
#define FREECOPYVARS(info, n) \
{ if ( n > MAX_ALLOCA_VARS ) \
    freeHeap(info.vars, sizeof(Word) * n); \
}


#define fetchBuf(b, var, type) \
		do \
		{ memcpy(var, (b)->data, sizeof(type)); \
		  (b)->data += sizeof(type); \
		} while(0)
#define fetchMultipleBuf(b, var, times, type) \
		do \
		{ memcpy(var, (b)->data, times*sizeof(type)); \
 		  (b)->data +=  times*sizeof(type); \
		} while(0)
#define skipBuf(b, type) \
		((b)->data += sizeof(type))


static inline int
fetchOpCode(CopyInfo b)
{ uchar tag;

  fetchBuf(b, &tag, uchar);
  DEBUG(9, Sdprintf("fetchOpCode() --> %d, (at %d)\n",
		    tag, b->data-b->base));
  return tag;
}


static uint
fetchSizeInt(CopyInfo b)
{ uint r = 0;
  uint end;

  do
  { uint d = *b->data++;

    end = !(d & 0x80);
    r = (r<<7)|(d&0x7f);
  } while(!end);

  return r;
}


static int64_t
fetchInt64(CopyInfo b)
{ int64_t val = 0;
  uint bytes = *b->data++;
  uint shift = (sizeof(int64_t)-bytes)*8;

  while(bytes-- > 0)
    val = (val << 8) | (*b->data++ & 0xff);

  val <<= shift;
  val >>= shift;

  return val;
}


static word
fetchWord(CopyInfo b)
{ word val;

  fetchBuf(b, &val, word);

  return val;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Fetch a float.  Note that the destination might not be double-aligned!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
fetchFloat(CopyInfo b, void *f)
{ fetchBuf(b, f, double);
}


static void
fetchExtFloat(CopyInfo b, void *f)
{ unsigned char *dst = f;
  unsigned int i;

  for(i=0; i<sizeof(double); i++)
    dst[double_byte_order[i]] = *b->data++;
}


static void
fetchAtom(CopyInfo b, atom_t *a)
{ unsigned int len = fetchSizeInt(b);

  *a = lookupAtom(b->data, len);

  (b)->data += len;
}


static void
fetchChars(CopyInfo b, unsigned len, Word to)
{ fetchMultipleBuf(b, (char *)to, len, char);
}


static void
copy_record(Word p, CopyInfo b ARG_LD)
{ intptr_t tag;

right_recursion:
  switch( (tag = fetchOpCode(b)) )
  { case PL_TYPE_VARIABLE:
    { intptr_t n = fetchSizeInt(b);

      if ( b->vars[n] )
      { if ( p > b->vars[n] )		/* ensure the reference is in the */
	  *p = makeRef(b->vars[n]);	/* right direction! */
	else
	{ *p = *b->vars[n];		/* wrong way.  make sure b->vars[n] */
	  *b->vars[n] = makeRef(p);	/* stays at the real variable */
	  b->vars[n] = p;		/* NOTE: also links attvars! */
	}
      } else
      {	setVar(*p);
	b->vars[n] = p;
      }

      return;
    }
    case PL_REC_ALLOCVAR:
    { setVar(*b->gstore);
      *p = makeRefG(b->gstore);
      p = b->gstore++;
      goto right_recursion;
    }
#if O_ATTVAR
    case PL_TYPE_ATTVAR:
    { intptr_t n = fetchSizeInt(b);

      *p = consPtr(b->gstore, TAG_ATTVAR|STG_GLOBAL);
      b->vars[n] = p;
      p = b->gstore++;
      goto right_recursion;
    }
#endif
    case PL_TYPE_ATOM:
    { *p = fetchWord(b);

      return;
    }
    case PL_TYPE_EXT_ATOM:
    { fetchAtom(b, p);
      PL_unregister_atom(*p);
      return;
    }
    case PL_TYPE_TAGGED_INTEGER:
    { int64_t val = fetchInt64(b);

      *p = consInt(val);

      return;
    }
    case PL_TYPE_INTEGER:
    { size_t i;
      union
      { int64_t i64;
	word    w[WORDS_PER_PLINT];
      } val;

      val.i64 = fetchInt64(b);

      *p = consPtr(b->gstore, TAG_INTEGER|STG_GLOBAL);
      *b->gstore++ = mkIndHdr(WORDS_PER_PLINT, TAG_INTEGER);
      for(i=0; i<WORDS_PER_PLINT; i++)
	*b->gstore++ = val.w[i];
      *b->gstore++ = mkIndHdr(WORDS_PER_PLINT, TAG_INTEGER);

      return;
    }
#ifdef O_GMP
    case PL_REC_MPZ:
    { b->data = loadMPZFromCharp(b->data, p, &b->gstore);

      return;
    }
#endif
    case PL_TYPE_FLOAT:
    case PL_TYPE_EXT_FLOAT:
    { *p = consPtr(b->gstore, TAG_FLOAT|STG_GLOBAL);
      *b->gstore++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
      if ( tag == PL_TYPE_FLOAT )
	fetchFloat(b, b->gstore);
      else
	fetchExtFloat(b, b->gstore);
      b->gstore += WORDS_PER_DOUBLE;
      *b->gstore++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);

      return;
    }
    case PL_TYPE_STRING:
    { unsigned len = fetchSizeInt(b);
      int lw, pad;
      word hdr;

      lw = (len+sizeof(word))/sizeof(word); /* see globalNString() */
      pad = (lw*sizeof(word) - len);
      *p = consPtr(b->gstore, TAG_STRING|STG_GLOBAL);
      *b->gstore++ = hdr = mkStrHdr(lw, pad);
      b->gstore[lw-1] = 0L;		/* zero-padding */
      fetchChars(b, len, b->gstore);
      b->gstore += lw;
      *b->gstore++ = hdr;

      return;
    }
#ifdef O_CYCLIC
    case PL_REC_CYCLE:
    { unsigned offset = fetchSizeInt(b);
      Word ct = b->gbase+offset;

      *p = consPtr(ct, TAG_COMPOUND|STG_GLOBAL);

      return;
    }
#endif
  { word fdef;
    int arity;
    case PL_TYPE_COMPOUND:

      fdef = fetchWord(b);
      arity = arityFunctor(fdef);

    compound:
      *p = consPtr(b->gstore, TAG_COMPOUND|STG_GLOBAL);
      *b->gstore++ = fdef;
      p = b->gstore;
      b->gstore += arity;
      for(; --arity > 0; p++)
	copy_record(p, b PASS_LD);
      goto right_recursion;
    case PL_TYPE_EXT_COMPOUND:
    { atom_t name;

      arity = (int)fetchSizeInt(b);
      fetchAtom(b, &name);
      fdef = lookupFunctorDef(name, arity);

      goto compound;
    }
  }
    case PL_TYPE_CONS:
    { *p = consPtr(b->gstore, TAG_COMPOUND|STG_GLOBAL);
      *b->gstore++ = FUNCTOR_dot2;
      p = b->gstore;
      b->gstore += 2;
      copy_record(p, b PASS_LD);
      p++;
      goto right_recursion;
    }
    default:
      assert(0);
  }
}


void
copyRecordToGlobal(term_t copy, Record r ARG_LD)
{ copy_info b;

  DEBUG(3, Sdprintf("PL_recorded(%p)\n", r));

  b.base = b.data = dataRecord(r);
  b.gbase = b.gstore = allocGlobal(r->gsize);

  INITCOPYVARS(b, r->nvars);
  copy_record(valTermRef(copy), &b PASS_LD);
  FREECOPYVARS(b, r->nvars);

  assert(b.gstore == gTop);
  SECURE(checkData(valTermRef(copy)));
}


#ifdef O_ATOMGC

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
We could consider some  optimisation  here,   notably  as  this stuff in
inderlying findall() and friends.  I  guess  we   can  get  rid  of  the
recursion.   Other   options:   combine     into    copyRecordToGlobal()
(recorded+erase), add a list of atoms as a separate entity.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
skipAtom(CopyInfo b)
{ uint len = fetchSizeInt(b);

  b->data += len;
}


static void
skipSizeInt(CopyInfo b)
{ while( b->data[0] & 0x80 )
    b->data++;
  b->data++;
}


static void
skipLong(CopyInfo b)
{ b->data += b->data[0] + 1;
}


static void
scanAtomsRecord(CopyInfo b, void (*func)(atom_t a))
{
right_recursion:

  switch( fetchOpCode(b) )
  { case PL_TYPE_VARIABLE:
    case PL_REC_CYCLE:
    { skipSizeInt(b);
      return;
    }
    case PL_REC_ALLOCVAR:
      goto right_recursion;
#ifdef O_ATTVAR
    case PL_TYPE_ATTVAR:
    { skipSizeInt(b);
      goto right_recursion;
    }
#endif
    case PL_TYPE_ATOM:
    { atom_t a = fetchWord(b);

      (*func)(a);
      return;
    }
    case PL_TYPE_EXT_ATOM:
    { skipAtom(b);
      return;
    }
    case PL_TYPE_TAGGED_INTEGER:
    case PL_TYPE_INTEGER:
    { skipLong(b);
      return;
    }
#ifdef O_GMP
    case PL_REC_MPZ:
      b->data = skipMPZOnCharp(b->data);
      return;
#endif
    case PL_TYPE_FLOAT:
    case PL_TYPE_EXT_FLOAT:
    { skipBuf(b, double);
      return;
    }
    case PL_TYPE_STRING:
    { uint len = fetchSizeInt(b);

      b->data += len;

      return;
    }
    case PL_TYPE_COMPOUND:
    { word fdef = fetchWord(b);
      int arity;

      arity = arityFunctor(fdef);
      while(--arity > 0)
	scanAtomsRecord(b, func);
      goto right_recursion;
    }
    case PL_TYPE_EXT_COMPOUND:
    { intptr_t arity = fetchSizeInt(b);

      skipAtom(b);
      while(--arity > 0)
	scanAtomsRecord(b, func);
      goto right_recursion;
    }
    case PL_TYPE_CONS:
    { scanAtomsRecord(b, func);
      goto right_recursion;
    }
    default:
      assert(0);
  }
}

#endif /*O_ATOMGC*/


		 /*******************************
		 *     STRUCTURAL EQUIVALENCE	*
		 *******************************/

static int
se_record(Word p, CopyInfo info ARG_LD)
{ word w;
  int stag;

right_recursion:
  stag = fetchOpCode(info);
unref_cont:
  w = *p;

  switch(tag(w))
  { case TAG_VAR:
      if ( stag == PL_TYPE_VARIABLE )
      { uint i = fetchSizeInt(info);

	if ( i != info->nvars )
	  fail;

	*p = (info->nvars<<7)|TAG_ATOM|STG_GLOBAL;
	info->vars[info->nvars++] = p;
	succeed;
      }
      fail;
    case TAG_ATTVAR:
      if ( stag == PL_REC_ALLOCVAR )	/* skip variable allocation */
	stag = fetchOpCode(info);
      if ( stag == PL_TYPE_ATTVAR )
      { Word ap = valPAttVar(w);
	uint i = fetchSizeInt(info);

	if ( i != info->nvars )
	  fail;

	addBuffer(info->avars, *p, word);
	*p = (info->nvars<<7)|TAG_ATOM|STG_GLOBAL;
	info->vars[info->nvars++] = mkAttVarP(p);

	p = ap;				/* do the attribute value */
	goto right_recursion;
      }
      fail;
    case TAG_ATOM:
      if ( storage(w) == STG_GLOBAL )
      { if ( stag == PL_TYPE_VARIABLE )
	{ uint n = (uint)((uintptr_t)(w) >> 7);
	  uint i = fetchSizeInt(info);

	  if ( i == n )
	    succeed;
	}
	fail;
      }

      DEBUG(9, Sdprintf("Matching '%s'\n", stringAtom(w)));
      if ( stag == PL_TYPE_ATOM )
      { atom_t val = fetchWord(info);

	if ( val == w )
	  succeed;
      } else if ( stag == PL_TYPE_EXT_ATOM )
      { atom_t val;

	fetchAtom((CopyInfo)info, &val);		/* TBD: Optimise! */
	if ( val == w )
	  succeed;
      }

      fail;
    case TAG_INTEGER:
      if ( isTaggedInt(w) )
      { if ( stag == PL_TYPE_TAGGED_INTEGER )
	{ int64_t val = valInt(w);
	  int64_t v2 = fetchInt64(info);

	  if ( v2 == val )
	    succeed;
	}
      } else
      { if ( stag == PL_TYPE_INTEGER )
	{ int64_t val = valBignum(w);
	  int64_t v2 = fetchInt64(info);

	  if ( v2 == val )
	    succeed;
	}
      }
      fail;
    case TAG_STRING:
      if ( stag == PL_TYPE_STRING )
      { Word f      = addressIndirect(w);
	size_t n    = wsizeofInd(*f);
	size_t pad  = padHdr(*f);		/* see also getCharsString() */
	size_t l    = n*sizeof(word)-pad;
	size_t llen = fetchSizeInt(info);

	if ( llen == l &&
	     memcmp((char *)(f+1), info->data, l) == 0 )
	{ info->data += l;

	  succeed;
	}
      }
      fail;
    case TAG_FLOAT:
      if ( stag == PL_TYPE_FLOAT )
      { Word v = valIndirectP(w);
	Word d = (Word)info->data;

	if ( memcmp(v, d, sizeof(double)) == 0 )
	{ info->data += sizeof(double);
	  succeed;
	}
      } else if ( stag == PL_TYPE_EXT_FLOAT )
      { Word v = valIndirectP(w);
	double d;

	fetchExtFloat(info, &d);
	if ( memcmp(v, &d, sizeof(double)) == 0 )
	  succeed;
      }

      fail;
    case TAG_COMPOUND:
      DEBUG(9, Sdprintf("Matching %s/%d\n",
			stringAtom(valueFunctor(functorTerm(w))->name),
			arityTerm(w)));
      if ( stag == PL_TYPE_COMPOUND )
      { Functor f = valueTerm(w);
	word fdef = fetchWord(info);

	if ( fdef == f->definition )
	{ int arity = arityFunctor(fdef);

	  p = f->arguments;
	  for(; --arity > 0; p++)
	  { if ( !se_record(p, info PASS_LD) )
	      fail;
	  }
	  goto right_recursion;
	}
      } else if ( stag == PL_TYPE_EXT_COMPOUND )
      { Functor f = valueTerm(w);
	FunctorDef fd = valueFunctor(f->definition);
	intptr_t arity = fetchSizeInt(info);
	atom_t name;

	if ( (unsigned)arity != fd->arity )
	  fail;
	fetchAtom((CopyInfo)info, &name);	/* TBD: optimise */
	if ( name != fd->name )
	  fail;

	p = f->arguments;
	for(; --arity > 0; p++)
	{ if ( !se_record(p, info PASS_LD) )
	    fail;
	}
        goto right_recursion;
      } else if ( stag == PL_TYPE_CONS )
      { Functor f = valueTerm(w);

	if ( f->definition == FUNCTOR_dot2 )
	{ p = f->arguments;
	  if ( !se_record(p, info PASS_LD) )
	    fail;
	  p++;
	  goto right_recursion;
	}
      }

      fail;
    case TAG_REFERENCE:
      p = unRef(w);
      goto unref_cont;
    default:
      assert(0);
      fail;
  }
}


int
structuralEqualArg1OfRecord(term_t t, Record r ARG_LD)
{ tmp_buffer avars;
  copy_info info;
  int n, rval, navars;
  Word *p;
  intptr_t stag;

  DEBUG(3, Sdprintf("structuralEqualArg1OfRecord() of ");
	   PL_write_term(Serror, t, 1200, PL_WRT_ATTVAR_WRITE);
	   Sdprintf("\n"));

  info.base = info.data = dataRecord(r);
  info.nvars = 0;
  INITCOPYVARS(info, r->nvars);
  initBuffer(&avars);
  info.avars = &avars;

					/* skip PL_TYPE_COMPOUND <functor> */
  stag = fetchOpCode(&info);
  if ( stag == PL_TYPE_COMPOUND )
    skipBuf(&info, word);
  else if ( stag == PL_TYPE_EXT_COMPOUND )
  { skipBuf(&info, intptr_t);		/* arity */
    skipAtom((CopyInfo)&info);		/* name */
  } else
    assert(0);

  rval = se_record(valTermRef(t), &info PASS_LD);

  for(p = info.vars, n=info.nvars, navars=0; --n >= 0; p++)
  { if ( isAttVarP(*p) )
    { *valAttVarP(*p) = fetchBuffer(&avars, navars++, word);
    } else
      setVar(**p);
  }

  discardBuffer(&avars);
  FREECOPYVARS(info, r->nvars);

  DEBUG(3, Sdprintf("structuralEqualArg1OfRecord() --> %d\n", rval));

  return rval;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
markAtomsRecord(Record record ARG_LD) must be called on all records that
use the R_NOLOCK option.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
markAtomsRecord(Record record)
{
#ifdef O_ATOMGC
  copy_info ci;

  ci.base = ci.data = dataRecord(record);
  scanAtomsRecord(&ci, markAtom);
  assert(ci.data == addPointer(record, record->size));
#endif
}


bool
freeRecord__LD(Record record ARG_LD)
{ if ( true(record, R_DUPLICATE) && --record->references > 0 )
    succeed;

#ifdef O_ATOMGC
  if ( false(record, (R_EXTERNAL|R_NOLOCK)) )
  { copy_info ci;

    DEBUG(3, Sdprintf("freeRecord(%p)\n", record));

    ci.base = ci.data = dataRecord(record);
    scanAtomsRecord(&ci, PL_unregister_atom);
    assert(ci.data == addPointer(record, record->size));
  }
#endif

  freeHeap(record, record->size);

  succeed;
}


static void
freeRecordRef(RecordRef r)
{ GET_LD
  freeRecord(r->record);
  freeHeap(r, sizeof(*r));
}


		 /*******************************
		 *	 EXTERNAL RECORDS	*
		 *******************************/

int
PL_recorded_external(const char *rec, term_t t)
{ GET_LD
  copy_info b;
  uint gsize;
  uchar m;

  b.base = b.data = rec;
  fetchBuf(&b, &m, uchar);

  if ( !REC_COMPAT(m) )
  { Sdprintf("PL_recorded_external: Incompatible version\n");
    fail;
  }

  if ( m & (REC_INT|REC_ATOM) )		/* primitive cases */
  { if ( m & REC_INT )
    { int64_t v = fetchInt64(&b);

      return PL_unify_int64(t, v);
    } else
    { atom_t a;

      fetchAtom(&b, &a);
      return PL_unify_atom(t, a);
    }
  }

  skipSizeInt(&b);			/* code-size */
  gsize = fetchSizeInt(&b);
  b.gbase = b.gstore = allocGlobal(gsize);
  if ( !(m & REC_GROUND) )
  { uint nvars = fetchSizeInt(&b);

    INITCOPYVARS(b, nvars);
    copy_record(valTermRef(t), &b PASS_LD);
    FREECOPYVARS(b, nvars);
  } else
  { copy_record(valTermRef(t), &b PASS_LD);
  }
  assert(b.gstore == gTop);

  SECURE(checkData(valTermRef(t)));

  return TRUE;
}


int
PL_erase_external(char *rec)
{ GET_LD
  copy_info b;
  uint scode;
  uchar m;

  b.base = b.data = rec;
  fetchBuf(&b, &m, uchar);
  if ( !REC_COMPAT(m) )
  { Sdprintf("PL_erase_external(): incompatible version\n");
    fail;
  }
  if (  m & (REC_INT|REC_ATOM) )
  { if (  m & REC_INT )
      skipLong(&b);
    else
      skipAtom(&b);
  } else
  { scode = fetchSizeInt(&b);
    skipSizeInt(&b);			/* gsize */
    if ( !(m & REC_GROUND) )
      skipSizeInt(&b);			/* nvars */
    b.data += scode;
  }

  freeHeap(rec, b.data-b.base);
  return TRUE;
}


		/********************************
		*       PROLOG CONNECTION       *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The key is stored as an atom, integer  or functor header as found on the
global-stack. A functor is a type with  the   same  mask as an atom, but
using the STG_GLOBAL storage indicator.  So,   the  first line denotes a
real atom.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool
unifyKey(term_t key, word val)
{ if ( (isAtom(val) && storage(val) != STG_GLOBAL) ||
       isTaggedInt(val) )
    return _PL_unify_atomic(key, val);

  return PL_unify_functor(key, (functor_t) val);
}


int
getKeyEx(term_t key, word *w ARG_LD)
{ Word k = valTermRef(key);
  deRef(k);

  if ( isAtom(*k) || isTaggedInt(*k) )
    *w = *k;
  else if ( isTerm(*k) )
    *w = (word)functorTerm(*k);
  else
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_key, key);

  succeed;
}


word
pl_current_key(term_t k, control_t h)
{ TableEnum e;
  Symbol s;
  fid_t fid;

  switch( ForeignControl(h) )
  { case FRG_FIRST_CALL:
      e = newTableEnum(GD->tables.record_lists);
      break;
    case FRG_REDO:
      e = ForeignContextPtr(h);
      break;
    case FRG_CUTTED:
    default:				/* fool gcc */
      e = ForeignContextPtr(h);
      freeTableEnum(e);
      succeed;
  }

  fid = PL_open_foreign_frame();
  while( (s=advanceTableEnum(e)) )
  { RecordList l = s->value;

    if ( l->firstRecord && unifyKey(k, l->key) )
      ForeignRedoPtr(e);

    PL_rewind_foreign_frame(fid);
  }

  freeTableEnum(e);
  fail;
}


static bool
record(term_t key, term_t term, term_t ref, int az)
{ GET_LD
  RecordList l;
  RecordRef r;
  Record copy;
  word k = 0L;

  DEBUG(3, Sdprintf("record() of ");
	   PL_write_term(Serror, term, 1200, PL_WRT_ATTVAR_WRITE);
	   Sdprintf("\n"));

  if ( !getKeyEx(key, &k PASS_LD) )
    fail;
  if ( !PL_is_variable(ref) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_variable, ref);

  copy = compileTermToHeap(term, 0);
  r = allocHeap(sizeof(*r));
  r->record = copy;
  PL_unify_pointer(ref, r);

  LOCK();
  l = lookupRecordList(k);
  r->list = l;

  if ( !l->firstRecord )
  { r->next = NULL;
    l->firstRecord = l->lastRecord = r;
  } else if ( az == RECORDA )
  { r->next = l->firstRecord;
    l->firstRecord = r;
  } else
  { r->next = NULL;
    l->lastRecord->next = r;
    l->lastRecord = r;
  }

  UNLOCK();

  succeed;
}


word
pl_recorda(term_t key, term_t term, term_t ref)
{ return record(key, term, ref, RECORDA);
}


word
pl_recordz(term_t key, term_t term, term_t ref)
{ return record(key, term, ref, RECORDZ);
}


word
pl_recorded(term_t key, term_t term, term_t ref, control_t h)
{ RecordList rl;
  RecordRef record;
  word k = 0L;
  term_t copy;
  word rval;

  switch( ForeignControl(h) )
  { case FRG_FIRST_CALL:
    { GET_LD
      void *ptr;

      if ( PL_get_pointer(ref, &ptr) )
      { record = ptr;
	LOCK();
	if ( isRecordRef(record) )
	{ if ( unifyKey(key, record->list->key) )
	  { GET_LD
	    copy = PL_new_term_ref();
	    copyRecordToGlobal(copy, record->record PASS_LD);
	    rval = PL_unify(term, copy);
	  } else
	    rval = FALSE;
	} else
	  rval = PL_error("recorded", 3, NULL, ERR_TYPE, ATOM_record, record);
	UNLOCK();
	return rval;
      }
      if ( !getKeyEx(key, &k PASS_LD) ||
	   !(rl = isCurrentRecordList(k)) )
	fail;
      LOCK();
      rl->references++;
      record = rl->firstRecord;
      break;
    }
    case FRG_REDO:
    { record = ForeignContextPtr(h);
      rl = record->list;

      assert(rl->references > 0);

      LOCK();
      break;
    }
    case FRG_CUTTED:
    { record = ForeignContextPtr(h);

      if ( record )
      { rl = record->list;

	LOCK();
	if ( --rl->references == 0 && true(rl, R_DIRTY) )
	  cleanRecordList(rl);
	UNLOCK();
      }
    }
      /* FALLTHROUGH */
    default:
      succeed;
  }

{ GET_LD
  fid_t fid;

  copy = PL_new_term_ref();
  fid = PL_open_foreign_frame();

  for( ; record; record = record->next )
  { if ( true(record->record, ERASED) )
      continue;

    copyRecordToGlobal(copy, record->record PASS_LD);
    if ( PL_unify(term, copy) && PL_unify_pointer(ref, record) )
    { PL_close_foreign_frame(fid);

      if ( !record->next )
      { if ( --rl->references == 0 && true(rl, R_DIRTY) )
	  cleanRecordList(rl);
	UNLOCK();
	succeed;
      } else
      { UNLOCK();
	ForeignRedoPtr(record->next);
      }
    }

    PL_rewind_foreign_frame(fid);
  }

  PL_close_foreign_frame(fid);
}

  if ( --rl->references == 0 && true(rl, R_DIRTY) )
    cleanRecordList(rl);

  UNLOCK();
  fail;
}


word
pl_erase(term_t ref)
{ GET_LD
  void *ptr;
  RecordRef record;
  RecordRef prev, r;
  RecordList l;
  word rval;

  if ( !PL_get_pointer(ref, &ptr) || !inCore(ptr))
    return PL_error("erase", 1, NULL, ERR_TYPE, ATOM_db_reference, ref);

  record = ptr;
  if ( isClause(record) )
  { Clause clause = (Clause) record;
    Definition def = getProcDefinition(clause->procedure);

    if ( !true(def, DYNAMIC) )
      return PL_error("erase", 1, NULL, ERR_PERMISSION,
		      ATOM_clause, ATOM_erase, ref);

    return retractClauseDefinition(def, clause PASS_LD);
  }

  LOCK();
  if ( isRecordRef(record) )
  {
#if O_DEBUGGER
    callEventHook(PLEV_ERASED, record);
#endif

    l = record->list;
    if ( l->references )		/* a recorded has choicepoints */
    { set(record->record, ERASED);
      set(l, R_DIRTY);
    } else if ( record == l->firstRecord )
    { if ( !record->next )
	l->lastRecord = NULL;
      l->firstRecord = record->next;
      freeRecordRef(record);
    } else
    { prev = l->firstRecord;
      r = prev->next;
      for(; r; prev = r, r = r->next)
      { if (r == record)
	{ if ( !r->next )
	  { assert(r == l->lastRecord);
	    l->lastRecord = prev;
	  }
	  prev->next = r->next;
	  freeRecordRef(r);
	  goto ok;
	}
      }
      goto nok;
    }

  ok:
    rval = TRUE;
  } else
  { nok:
    rval = PL_error("erase", 1, NULL, ERR_DOMAIN, ATOM_db_reference, ref);
  }
  UNLOCK();

  return rval;
}

		 /*******************************
		 *	     COMPLEXITY		*
		 *******************************/

static int
count_term(Word t, int left ARG_LD)
{ int count = 0;

right_recursion:
  deRef(t);

  if ( --left < 0 )
    return -1;
  count++;

  if ( isAttVar(*t) )
  { Word p = valPAttVar(*t);

    assert(onStackArea(global, p));
    t = p;
    goto right_recursion;
  } else if ( isTerm(*t) )
  { int arity = arityTerm(*t);
    int me;

    for(t = argTermP(*t, 0); arity-- > 0; count += me, t++ )
    { if ( arity == 0 )
	goto right_recursion;

      me = count_term(t, left PASS_LD);
      if ( me < 0 )
	return me;
      left -= me;
      if ( left < 0 )
	return -1;
    }
  }

  return count+1;
}


#ifndef INT_MAX
#define INT_MAX	    ((int)(((unsigned int)1<<(sizeof(int)*8-1))-1))
#define INT_MIN     (-(INT_MIN)-1)
#endif

word
pl_term_complexity(term_t t, term_t mx, term_t count)
{ GET_LD
  int c, m;

  if ( !PL_get_integer(mx, &m) )
    m = INT_MAX;

  c = count_term(valTermRef(t), m PASS_LD);
  if ( c < 0 || c > m )
    fail;

  return PL_unify_integer(count, c);
}
