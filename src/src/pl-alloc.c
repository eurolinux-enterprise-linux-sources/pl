/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2002, University of Amsterdam

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

#include "pl-incl.h"

#ifndef O_MYALLOC
#define O_MYALLOC 1
#endif

#ifndef ALLOC_DEBUG
#if defined(_DEBUG) && defined(__WINDOWS__)
#define ALLOC_DEBUG 1
#else
#define ALLOC_DEBUG 0
#endif
#endif

#define LOCK()   PL_LOCK(L_ALLOC)
#define UNLOCK() PL_UNLOCK(L_ALLOC)
#undef LD
#define LD LOCAL_LD

#if O_MYALLOC

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module defines memory allocation for the heap (the  program  space)
and  the  various  stacks.   Memory  allocation below ALLOCFAST bytes is
based entirely on a perfect fit algorithm.  Above ALLOCFAST  the  system
memory  allocation  function  (typically malloc() is used to optimise on
space.  Perfect fit memory allocation is fast and because  most  of  the
memory  is allocated in small segments and these segments normally occur
in similar relative frequencies it does not waste much memory.

The prolog machinery using these memory allocation functions always know
how  much  memory  is  allocated  and  provides  this  argument  to  the
corresponding  unalloc()  call if memory need to be freed.  This saves a
word to store the size of the memory segment.


Releasing memory to the system in PL_cleanup()
----------------------------------------------

Big chunks are allocated using allocBigHeap(),  which preserves a double
linked list of malloc() allocated object, so  we can easily and reliably
return all allocated memory back to  the OS. Perfect-fit-allocated small
blobs are formed from allocBigHeap() allocated   objects, so again there
is no need to keep track  of  the   allocated  memory  for freeing it in
PL_cleanup().


Efficiency considerations in multi-threaded code
------------------------------------------------

Most malloc() libraries are thread-safe only by using a mutex around the
entire function.  This poses a serious slow-down, especially in assert()
and findall/3 and friends.

We use a perfect-fit pool for each thread,   as well as a global pool in
GD.  Rules:

  * If a thread terminates  all  remaining   memory  is  merged with the
    global pool.

  * Memory freed during the execution of a thread is stored in the pool
    of the thread.

  * Memory is first allocated from the thread-pool.  If this is empty,
    a quick-and-dirty access to the global pool is tried.  If there
    appears to be memory it will try to nicely import the first 100
    cells of the same size from the global pool.  If it fails, allocate()
    allocates new memory.

  * If a local pool holds more than 2*100 cells of a size, the first 100
    are moved to the global pool.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifndef ALIGN_SIZE
#if (defined(__sgi) && !defined(__GNUC__)) || defined __sparc__ || defined __sparc64__
#define ALIGN_SIZE sizeof(double)
#else
#ifdef __ia64__
#define ALIGN_SIZE sizeof(intptr_t[2])
#else
#define ALIGN_SIZE sizeof(intptr_t)
#endif
#endif
#endif
#define ALLOC_MIN  sizeof(Chunk)

typedef struct big_heap
{ struct big_heap *next;
  struct big_heap *prev;
} *BigHeap;

static void *allocate(AllocPool pool, size_t size);
static void *allocBigHeap(size_t size);
static void  freeBigHeap(void *p);
static void  freeAllBigHeaps(void);

#define ALLOCROUND(n) ROUND(n, ALIGN_SIZE)


		 /*******************************
		 *	DEBUG ENCAPSULATION	*
		 *******************************/

#if ALLOC_DEBUG

#define INUSE_MAGIC 0x42424242
#define FREE_MAGIC 0x43434343
#define ALLOC_FREE_MAGIC 0x5f
#define ALLOC_MAGIC 0xbf
#define ALLOC_VIRGIN_MAGIC 0x7f

void core_freeHeap__LD(void *mem, size_t n ARG_LD);
void *core_allocHeap__LD(size_t n ARG_LD);

void
freeHeap__LD(void *mem, size_t n ARG_LD)
{ intptr_t *p = mem;

  assert(p[-1] == (intptr_t)n);
  assert(p[-2] == INUSE_MAGIC);
  p[-2] = FREE_MAGIC;			/* trap double free */
  memset(mem, ALLOC_FREE_MAGIC, n);

  core_freeHeap__LD(p-3, n+3*sizeof(intptr_t) PASS_LD);
}


void *
allocHeap__LD(size_t n ARG_LD)
{ intptr_t *p = core_allocHeap__LD(n+3*sizeof(intptr_t) PASS_LD);

  p += 3;
  p[-1] = n;
  p[-2] = INUSE_MAGIC;
  memset(p, ALLOC_MAGIC, n);

  return p;
}

#define freeHeap__LD core_freeHeap__LD
#define allocHeap__LD core_allocHeap__LD

#endif /*ALLOC_DEBUG*/

		 /*******************************
		 *	       FREE		*
		 *******************************/

#define MAX_FREE_LENGTH 100

static void
freeToPool(AllocPool pool, void *mem, size_t n, int islocal)
{ Chunk p = (Chunk) mem;

  pool->allocated -= n;
  DEBUG(9, Sdprintf("freed %ld bytes at %ld\n",
		    (uintptr_t)n, (uintptr_t)p));

  n /= ALIGN_SIZE;
  p->next = pool->free_chains[n];
  pool->free_chains[n] = p;
  pool->free_count[n]++;

#ifdef O_PLMT
  if ( islocal && pool->free_count[n] > 2*MAX_FREE_LENGTH )
  { Chunk l, c;
    int i = MAX_FREE_LENGTH;

    for(l=c=pool->free_chains[n]; --i > 0; c = c->next)
    { assert(c);
    }
    pool->free_chains[n] = c->next;
    c->next = NULL;
    pool->free_count[n] -= MAX_FREE_LENGTH;

    LOCK();
    c->next = GD->alloc_pool.free_chains[n];
    GD->alloc_pool.free_chains[n] = l;
    GD->alloc_pool.free_count[n] += MAX_FREE_LENGTH;
    UNLOCK();
  }
#endif
}


void
freeHeap__LD(void *mem, size_t n ARG_LD)
{ if ( mem == NULL )
    return;
  n = ALLOCROUND(n);

  if ( n <= ALLOCFAST )
  {
#ifdef O_PLMT
    if ( LD )				/* LD might be gone already */
      freeToPool(&LD->alloc_pool, mem, n, TRUE);
    else
#endif
    { LOCK();
      freeToPool(&GD->alloc_pool, mem, n, FALSE);
      UNLOCK();
    }
  } else
  { LOCK();
    freeBigHeap(mem);
    GD->statistics.heap -= n;
    UNLOCK();
  }
}


		 /*******************************
		 *	     ALLOCATE		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
No perfect fit is available.  We pick memory from the big chunk  we  are
working  on.   If this is not big enough we will free the remaining part
of it.  Next we check whether any areas are  assigned  to  be  used  for
allocation.   If  all  this fails we allocate new core using Allocate(),
which normally calls malloc().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
leftoverToChains(AllocPool pool)
{ if ( pool->free >= sizeof(struct chunk) )
  { size_t m = pool->free/ALIGN_SIZE;
    Chunk ch = (Chunk)pool->space;

    assert(m <= ALLOCFAST/ALIGN_SIZE);

    ch->next = pool->free_chains[m];
    pool->free_chains[m] = ch;
    pool->free_count[m]++;
  }

  pool->free = 0;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
allocate() gets new memory from the free  space   of  a pool, or if this
does not have the required memory, get a new block using allocBigHeap().
It can be called both with a local  pool or the global pool as argument.
In the latter case the call is   protected by the default LOCK/UNLOCK of
this module. Calls with the local pools are  *not* locked, so we need to
do locking to access the left_over_pool and allocBigHeap().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void *
allocate(AllocPool pool, size_t n)
{ char *p;
  int mustlock = (pool != &GD->alloc_pool);
  int welocked = FALSE;

  if ( n <= pool->free )
  { p = pool->space;
    pool->space += n;
    pool->free -= n;
    pool->allocated += n;
    return p;
  }

  leftoverToChains(pool);
#ifdef O_PLMT
  if ( GD->left_over_pool )
  { FreeChunk *fp, ch;

    if ( mustlock )
    { welocked = TRUE;
      LOCK();
    }
    for( fp = &GD->left_over_pool; (ch=*fp); fp = &ch->next )
    { if ( ch->size >= n )
      { *fp = ch->next;
        if ( welocked )
	  UNLOCK();

	p = (char *)ch;
	pool->space = p + n;
	pool->free  = ch->size - n;
	pool->allocated += n;

	return p;
      }
    }
  }
#endif

  if ( mustlock && !welocked )
  { LOCK();
    welocked = TRUE;
  }

  if ( !(p = allocBigHeap(ALLOCSIZE)) )
  { if ( welocked )
      UNLOCK();
    outOfCore();
  }

  pool->space = p + n;
  pool->free  = ALLOCSIZE - n;
  pool->allocated += n;

  if ( welocked )
    UNLOCK();

  return p;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
allocFromPool() allocates a block of size m*ALIGN_SIZE as available from
the m-th chain.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void *
allocFromPool(AllocPool pool, size_t m)
{ Chunk f;

  if ( (f = pool->free_chains[m]) )
  { pool->free_chains[m] = f->next;
    pool->free_count[m]--;
    DEBUG(9, Sdprintf("(r) %p\n", f));
    pool->allocated += m*ALIGN_SIZE;

    return f;
  }

  return NULL;
}


void *
allocHeap__LD(size_t n ARG_LD)
{ void *mem;

  if ( n == 0 )
    return NULL;
  n = ALLOCROUND(n);

  if ( n <= ALLOCFAST )
  { size_t m = n / (int) ALIGN_SIZE;
#ifdef O_PLMT
    if ( LD )			/* deal with alloc before PL_initialise() */
    { if ( !(mem = allocFromPool(&LD->alloc_pool, m)) )
      { if ( GD->alloc_pool.free_chains[m] )
	{ Chunk c;
	  int i = MAX_FREE_LENGTH;

	  LOCK();
	  LD->alloc_pool.free_chains[m] = GD->alloc_pool.free_chains[m];
	  for(c=LD->alloc_pool.free_chains[m]; c && --i > 0; c = c->next)
	    ;
	  if ( c )
	  { GD->alloc_pool.free_chains[m] = c->next;
	    c->next = NULL;
	    LD->alloc_pool.free_count[m] += MAX_FREE_LENGTH;
	    GD->alloc_pool.free_count[m] -= MAX_FREE_LENGTH;
	  } else
	  { GD->alloc_pool.free_chains[m] = NULL;
	    LD->alloc_pool.free_count[m] += GD->alloc_pool.free_count[m];
	    GD->alloc_pool.free_count[m] = 0;
	  }
	  UNLOCK();

	  if ( !(mem = allocFromPool(&LD->alloc_pool, m)) )
	    mem = allocate(&LD->alloc_pool, n);
	} else
	  mem = allocate(&LD->alloc_pool, n);
      }
    } else
#endif /*O_PLMT*/
    { LOCK();
      if ( !(mem = allocFromPool(&GD->alloc_pool, m)) )
	mem = allocate(&GD->alloc_pool, n);
      UNLOCK();
    }
  } else
  { LOCK();
    mem = allocBigHeap(n);
    GD->statistics.heap += n;
    UNLOCK();
  }

  return mem;
}


static void
destroyAllocPool(AllocPool pool)
{ memset(pool->free_chains, 0, sizeof(pool->free_chains));
  memset(pool->free_count,  0, sizeof(pool->free_count));
  pool->free  = 0;
  pool->space = NULL;
}


void
cleanupMemAlloc(void)
{ freeAllBigHeaps();

  destroyAllocPool(&GD->alloc_pool);
}


		 /*******************************
		 *	 EXCHANGING POOLS	*
		 *******************************/

#ifdef O_PLMT
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
mergeAllocPool(to, from)
    Merge all chain from `from' into `to'.  Used to move the pool of a
    terminated thread into the global pool.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
mergeAllocPool(AllocPool to, AllocPool from)
{ Chunk *t, *f;
  unsigned int i;

  assert(to == &GD->alloc_pool);	/* for now */
  DEBUG(2, Sdprintf("Free pool has %d bytes\n", from->free));

  LOCK();
  if ( from->free > ALLOCFAST )
  { FreeChunk p = (FreeChunk)from->space;
    p->size = from->free;
    from->free = 0;
    p->next = GD->left_over_pool;
    GD->left_over_pool = p;
  } else
    leftoverToChains(from);

  for(i=0, t=to->free_chains, f = from->free_chains;
      i < (ALLOCFAST/ALIGN_SIZE);
      i++, t++, f++)
  { if ( *f )
    { if ( to->free_count[i] )
      { if ( to->free_count[i] <= from->free_count[i] )
	{ Chunk c = *t;

	  while(c->next)
	    c = c->next;
	  c->next = *f;
	} else
	{ Chunk c = *f;

	  while(c->next)
	    c = c->next;
	  c->next = *t;
	  *t = *f;
	}
      } else
	*t = *f;

      to->free_count[i] += from->free_count[i];
      from->free_count[i] = 0;

      *f = NULL;
    }
  }
  UNLOCK();

  to->allocated += from->allocated;
}

#endif /*O_PLMT*/

		 /*******************************
		 *	   LARGE CHUNKS		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Deal with big allocHeap() calls. We have   comitted ourselves to be able
to free all memory we have allocated, so   we  must know the pointers we
have allocated. Normally big chunks are rather infrequent, but there can
be a lot if the program uses lots of big clauses, records or atoms.

MT: calls must be guarded by L_ALLOC
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static BigHeap big_heaps;

static void *
allocBigHeap(size_t size)
{ BigHeap h = malloc(size+sizeof(*h));

  if ( !h )
  { outOfCore();
    return NULL;
  }

  h->next = big_heaps;
  h->prev = NULL;
  if ( big_heaps )
    big_heaps->prev = h;
  big_heaps = h;
  h++;					/* point to user-data */

  if ( !hTop )
  { hBase = (char *)h;
    hTop = (char *)h + size;
    heap_base = (uintptr_t)h & ~(uintptr_t)0x007fffff; /* 8MB */
  } else
  { SetHBase(h);
    SetHTop((char *)h + size);
  }

  return (void *)h;
}


static void
freeBigHeap(void *p)
{ BigHeap h = p;

  h--;					/* get the base */
  if ( h->prev )
    h->prev->next = h->next;
  else
    big_heaps = h->next;
  if ( h->next )
    h->next->prev = h->prev;

  free(h);
}


static void
freeAllBigHeaps(void)
{ BigHeap h, next;

  for(h=big_heaps; h; h=next)
  { next = h->next;
    free(h);
  }
  big_heaps = NULL;
}

#if ALLOC_DEBUG
#undef freeHeap__LD
#undef allocHeap__LD
#endif /*ALLOC_DEBUG*/

#else /*O_MYALLOC*/

void *
allocHeap__LD(size_t n ARG_LD)
{ if ( n )
  { void *mem = malloc(n);

    if ( !mem )
      outOfCore();

    GD->statistics.heap += n;
    if ( !hTop )
    { hBase = mem;
      hTop = (char *)mem + n;
      heap_base = (uintptr_t)mem & ~(uintptr_t)0x007fffff; /* 8MB */
    } else
    { SetHBase(mem);
      SetHTop((char *)mem + n);
    }

    return mem;
  }

  return NULL;
}


void
freeHeap__LD(void *mem, size_t n ARG_LD)
{
#if ALLOC_DEBUG
  memset((char *) mem, ALLOC_FREE_MAGIC, n);
#endif

  free(mem);
  GD->statistics.heap -= n;
}


void
cleanupMemAlloc(void)
{ 					/* TBD: Cleanup! */
}

void
mergeAllocPool(AllocPool to, AllocPool from)
{
}

#endif /*O_MYALLOC*/


		/********************************
		*             STACKS            *
		*********************************/

word
outOfStack(void *stack, stack_overflow_action how)
{ GET_LD
  Stack s = stack;

  LD->trim_stack_requested = TRUE;

  switch(how)
  { case STACK_OVERFLOW_FATAL:
      LD->outofstack = s;
      updateAlerted(LD);
      Sdprintf("ERROR: Out of %s stack (ungraceful overflow)", s->name);

      pl_abort(ABORT_FATAL);
      assert(0);
      fail;
    case STACK_OVERFLOW_THROW:
    case STACK_OVERFLOW_RAISE:
    { fid_t fid = PL_open_foreign_frame();
      LD->outofstack = NULL;
      updateAlerted(LD);
      PL_clearsig(SIG_GC);
      s->gced_size = 0;			/* after handling, all is new */
      PL_unify_term(LD->exception.tmp,
		    PL_FUNCTOR, FUNCTOR_error2,
		      PL_FUNCTOR, FUNCTOR_resource_error1,
		        PL_ATOM, ATOM_stack,
		      PL_CHARS, s->name);
      if ( how == STACK_OVERFLOW_THROW )
      { PL_throw(LD->exception.tmp);
	warning("Out of %s stack while not in Prolog!?", s->name);
	assert(0);
      } else
      { PL_raise_exception(LD->exception.tmp);
      }
      PL_close_foreign_frame(fid);
      fail;
    }
    case STACK_OVERFLOW_SIGNAL:
      LD->outofstack = s;
      updateAlerted(LD);
      succeed;
  }
  assert(0);
  fail;
}


void
outOfCore()
{ fatalError("Could not allocate memory: %s", OsError());
}

		 /*******************************
		 *	REFS AND POINTERS	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
__consPtr() is inlined for this module (including pl-wam.c), but external
for the other modules, where it is far less fime-critical.

Actually, for normal operation, consPtr() is a macro from pl-data.h
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if !defined(consPtr) || defined(SECURE_GC) || defined(_DEBUG)
#undef consPtr

static inline word
__consPtr(void *p, word ts)
{ GET_LD
  uintptr_t v = (uintptr_t) p;

  v -= base_addresses[ts&STG_MASK];
  assert(v < MAXTAGGEDPTR && !(v&0x3));
  return (v<<5)|ts;
}

word
consPtr(void *p, word ts)
{ return __consPtr(p, ts);
}

#define consPtr(p, s) __consPtr(p, s)
#endif


		/********************************
		*        GLOBAL STACK           *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
allocGlobal() allocates on the global stack.  Many  functions  do  this
inline  as  it is simple and usualy very time critical.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if O_SHIFT_STACKS

Word
allocGlobal__LD(size_t n ARG_LD)
{ Word result;

  if ( roomStack(global) < (intptr_t) (n * sizeof(word)) )
  { growStacks(NULL, NULL, NULL, 0, n * sizeof(word), 0);

    if ( roomStack(global) < (intptr_t) (n * sizeof(word)) )
      outOfStack((Stack) &LD->stacks.global, STACK_OVERFLOW_THROW);
  }

  result = gTop;
  gTop += n;

  return result;
}

Word
allocGlobalNoShift__LD(size_t n ARG_LD)
{ Word result;

  if ( roomStack(global) < (intptr_t) (n * sizeof(word)) )
    return NULL;

  result = gTop;
  gTop += n;

  return result;
}

#else

static inline Word
__allocGlobal(size_t n ARG_LD)
{ Word result = gTop;

  requireStack(global, n * sizeof(word));
  gTop += n;

  return result;
}

Word allocGlobal__LD(size_t n ARG_LD)
{ return __allocGlobal(n PASS_LD);
}

#undef allocGlobal			/* use inline version here */
#define allocGlobal(n) __allocGlobal(n PASS_LD)

#endif

word
globalFunctor(functor_t f)
{ GET_LD
  int arity = arityFunctor(f);
  Word a = allocGlobal(1 + arity);
  Word t = a;

  *a = f;
  while( --arity >= 0 )
    setVar(*++a);

  return consPtr(t, TAG_COMPOUND|STG_GLOBAL);
}


Word
newTerm(void)
{ GET_LD
  Word t = allocGlobal(1);

  setVar(*t);

  return t;
}

		 /*******************************
		 *      OPERATIONS ON LONGS	*
		 *******************************/

word
globalLong(int64_t l ARG_LD)
{ Word p;
  word r, m;

#if SIZEOF_VOIDP == 8
  p = allocGlobal(3);
  r = consPtr(p, TAG_INTEGER|STG_GLOBAL);
  m = mkIndHdr(1, TAG_INTEGER);

  *p++ = m;
  *p++ = l;
  *p   = m;
#else
#if SIZEOF_VOIDP == 4
  p = allocGlobal(4);
  r = consPtr(p, TAG_INTEGER|STG_GLOBAL);
  m = mkIndHdr(2, TAG_INTEGER);

  *p++ = m;
#ifdef WORDS_BIGENDIAN
  *p++ = (word)(l>>32);
  *p++ = (word)l;
#else
  *p++ = (word)l;
  *p++ = (word)(l>>32);
#endif
  *p   = m;
#else
#error "FIXME: Unsupported sizeof intptr_t."
#endif
#endif


  return r;
}


		 /*******************************
		 *    OPERATIONS ON STRINGS	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
To distinguish between byte and wide strings,   the system adds a 'B' or
'W' in front of the real string. For   a  'W', the following 3 bytes are
ignored to avoid alignment restriction problems.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Word
allocString(size_t len ARG_LD)
{ size_t lw = (len+sizeof(word))/sizeof(word);
  int pad = (int)(lw*sizeof(word) - len);
  Word p = allocGlobal(2 + lw);
  word m = mkStrHdr(lw, pad);

  p[0]    = m;
  p[lw]   = 0L;				/* zero the pad bytes */
  p[lw+1] = m;

  return p;
}


word
globalString(size_t len, const char *s)
{ GET_LD
  Word p = allocString(len+1 PASS_LD);
  char *q = (char *)&p[1];

  *q++ = 'B';
  memcpy(q, s, len);

  return consPtr(p, TAG_STRING|STG_GLOBAL);
}


word
globalWString(size_t len, const pl_wchar_t *s)
{ GET_LD
  const pl_wchar_t *e = &s[len];
  const pl_wchar_t *p;
  Word g;

  for(p=s; p<e; p++)
  { if ( *p > 0xff )
      break;
  }

  if ( p == e )				/* 8-bit string */
  { unsigned char *t;

    g = allocString(len+1 PASS_LD);
    t = (unsigned char *)&g[1];
    *t++ = 'B';
    for(p=s; p<e; )
      *t++ = *p++ & 0xff;
  } else				/* wide string */
  { char *t;
    pl_wchar_t *w;

    g = allocString((len+1)*sizeof(pl_wchar_t) PASS_LD);
    t = (char *)&g[1];
    w = (pl_wchar_t*)t;
    w[0] = 0;
    *t = 'W';
    memcpy(&w[1], s, len*sizeof(pl_wchar_t));
  }

  return consPtr(g, TAG_STRING|STG_GLOBAL);
}


char *
getCharsString__LD(word w, size_t *len ARG_LD)
{ Word p = valPtr(w);
  word m = *p;
  size_t wn  = wsizeofInd(m);
  size_t pad = padHdr(m);
  char *s;

  if ( len )
    *len = wn*sizeof(word) - pad - 1;	/* -1 for the 'B' */

  s = (char *)&p[1];

  if ( *s == 'B' )
    return s+1;

  assert(*s == 'W');
  return NULL;
}


pl_wchar_t *
getCharsWString__LD(word w, size_t *len ARG_LD)
{ Word p = valPtr(w);
  word m = *p;
  size_t wn  = wsizeofInd(m);
  size_t pad = padHdr(m);
  char *s;
  pl_wchar_t *ws;

  s = (char *)&p[1];
  if ( *s != 'W' )
    return NULL;

  if ( len )
    *len = ((wn*sizeof(word) - pad)/sizeof(pl_wchar_t)) - 1;

  ws = (pl_wchar_t *)&p[1];
  return ws+1;
}



		 /*******************************
		 *     OPERATIONS ON DOUBLES	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Storage of floats (doubles) on the  stacks   and  heap.  Such values are
packed into two `guards words'.  An   intermediate  structure is used to
ensure the possibility of  word-aligned  copy   of  the  data. Structure
assignment is used here  to  avoid  a   loop  for  different  values  of
WORDS_PER_DOUBLE.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct
{ word w[WORDS_PER_DOUBLE];
} fword;


void
doublecpy(void *to, void *from)
{ fword *t = to;
  fword *f = from;

  *t = *f;
}


double					/* take care of alignment! */
valFloat__LD(word w ARG_LD)
{ fword *v = (fword *)valIndirectP(w);
  union
  { double d;
    fword  l;
  } val;

  val.l = *v;

  return val.d;
}


word
globalFloat(double d)
{ GET_LD
  Word p = allocGlobal(2+WORDS_PER_DOUBLE);
  word r = consPtr(p, TAG_FLOAT|STG_GLOBAL);
  word m = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
  union
  { double d;
    fword  l;
  } val;
  fword *v;

  val.d = d;
  *p++ = m;
  v = (fword *)p;
  *v++ = val.l;
  p = (Word) v;
  *p   = m;

  return r;
}


		 /*******************************
		 *	  64-BIT INTEGERS	*
		 *******************************/

#ifdef INT64_ALIGNMENT

int64_t					/* take care of alignment! */
valBignum__LD(word w ARG_LD)
{ Word p = valIndirectP(w);
  union
  { int64_t i;
    word w[WORDS_PER_INT64];
  } val;

#if ( SIZEOF_VOIDP == 4 )
  val.w[0] = p[0];
  val.w[1] = p[1];
#else
#error "Unsupported int64_t alignment conversion"
#endif

  return val.i;
}

#endif

		 /*******************************
		 *  GENERIC INDIRECT OPERATIONS	*
		 *******************************/

int
equalIndirect(word w1, word w2)
{ GET_LD
  Word p1 = addressIndirect(w1);
  Word p2 = addressIndirect(w2);

  if ( *p1 == *p2 )
  { size_t n = wsizeofInd(*p1);

    while( n-- > 0 )
    { if ( *++p1 != *++p2 )
	fail;
    }

    succeed;
  }

  fail;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Copy an indirect data object to the heap.  The type is not of importance,
neither is the length or original location.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
globalIndirect(word w)
{ GET_LD
  Word p = addressIndirect(w);
  word t = *p;
  size_t  n = wsizeofInd(t);
  Word h = allocGlobal((n+2));
  Word hp = h;

  *hp = t;
  while(n-- > 0)
    *++hp = *++p;
  *++hp = t;

  return consPtr(h, tag(w)|STG_GLOBAL);
}


word
globalIndirectFromCode(Code *PC)
{ GET_LD
  Code pc = *PC;
  word m = *pc++;
  size_t n = wsizeofInd(m);
  Word p = allocGlobal(n+2);
  word r = consPtr(p, tag(m)|STG_GLOBAL);

  *p++ = m;
  while(n-- > 0)
    *p++ = *pc++;
  *p++ = m;

  *PC = pc;
  return r;
}


static int				/* used in pl-wam.c */
equalIndirectFromCode(word a, Code *PC)
{ GET_LD
  Word pc = *PC;
  Word pa = addressIndirect(a);

  if ( *pc == *pa )
  { size_t n = wsizeofInd(*pc);

    while(n-- > 0)
    { if ( *++pc != *++pa )
	fail;
    }
    pc++;
    *PC = pc;
    succeed;
  }

  fail;
}


		/********************************
		*            STRINGS            *
		*********************************/


#ifdef O_DEBUG
#define CHAR_INUSE 0x42
#define CHAR_FREED 0x41

char *
store_string(const char *s)
{ if ( s )
  { GET_LD
    char *copy = (char *)allocHeap(strlen(s)+2);

    *copy++ = CHAR_INUSE;
    strcpy(copy, s);

    return copy;
  } else
  { return NULL;
  }
}


void
remove_string(char *s)
{ if ( s )
  { GET_LD
    assert(s[-1] == CHAR_INUSE);

    s[-1] = CHAR_FREED;
    freeHeap(s-1, strlen(s)+2);
  }
}

#else /*O_DEBUG*/

char *
store_string(const char *s)
{ if ( s )
  { GET_LD

    char *copy = (char *)allocHeap(strlen(s)+1);

    strcpy(copy, s);
    return copy;
  } else
  { return NULL;
  }
}


void
remove_string(char *s)
{ if ( s )
  { GET_LD
    freeHeap(s, strlen(s)+1);
  }
}

#endif /*O_DEBUG*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Hash function for strings.  This function has been evaluated on Shelley,
which defines about 5000 Prolog atoms.  It produces a very nice  uniform
distribution over these atoms.  Note that size equals 2^n.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
unboundStringHashValue(const char *t, size_t len)
{ unsigned int value = 0;
  unsigned int shift = 5;

  while(len-- != 0)
  { unsigned int c = *t++;

    c -= 'a';
    value ^= c << (shift & 0xf);
    shift ^= c;
  }

  return value ^ (value >> 16);
}


		 /*******************************
		 *	     GNU MALLOC		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
These functions are used by various GNU-libraries and -when not linked
with the GNU C-library lead to undefined symbols.  Therefore we define
them in SWI-Prolog so that we can also give consistent warnings.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if !defined(xmalloc) && defined(O_XMALLOC)

void *
xmalloc(size_t size)
{ void *mem;

  if ( (mem = malloc(size)) )
    return mem;
  if ( size )
    outOfCore();

  return NULL;
}


void *
xrealloc(void *mem, size_t size)
{ void *newmem;

  newmem = mem ? realloc(mem, size) : malloc(size);
  if ( newmem )
    return newmem;
  if ( size )
    outOfCore();

  return NULL;
}

#endif /*xmalloc*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Allocation on behalf of foreign code. There  is generally no need to use
this, unless malloced data is returned by Prolog and the foreign routine
wants to free it (e.g. using BUF_MALLOC).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void *
PL_malloc(size_t size)
{ void *mem;

  if ( size == 0 )
    return NULL;
  if ( (mem = malloc(size)) )
    return mem;

  outOfCore();

  return NULL;
}


void
PL_free(void *mem)
{ if ( mem )
    free(mem);
}


void *
PL_realloc(void *mem, size_t size)
{ void *newmem;

  if ( mem )
  { if ( size )
    { if ( !(newmem = realloc(mem, size)) )
	outOfCore();

      return newmem;
    } else
    { free(mem);
      return NULL;
    }
  }

  return PL_malloc(size);
}


#undef LOCK
#undef UNLOCK
