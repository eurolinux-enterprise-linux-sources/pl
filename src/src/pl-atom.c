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

/*#define O_DEBUG 1*/
#include "pl-incl.h"
#include "pl-ctype.h"

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Implementation issues
---------------------

There are two parts in the atom   administration. One is a dynamic array
(called buffer) atom_array, which is there to   find  the atoms back. An
atom as it appears is a intptr_t   of the form (n<<LMASK_BITS)|TAG_ATOM. The
atom  structure  is  located  by  getting  the  n-th  pointer  from  the
atom_array dynamic array.  See atomValue() for translating the intptr_t into
the address of the structure.

Next, there is a hash-table, which is a normal `open' hash-table mapping
char * to the atom structure. This   thing is dynamically rehashed. This
table is used by lookupAtom() below.

Atom garbage collection
-----------------------

There is no such thing, but below is an outline of what in entails.

There are various categories of atoms:

	# Built-in atoms
	These are used directly in the C-source of the system and cannot
	be removed. These are the atoms upto a certain number. This
	number is sizeof(atoms)/sizeof(char *).

	# Foreign referenced atoms
	These are references hold in foreign code by means of
	PL_new_atom() or other calls returning an atom. The system has
	no way to determine the lifetime of them. Most probably the best
	approach is to offer a locking/unlocking flag to deal this type
	of atoms. The lock/unlock may be nested. The proposed functions
	are:

		PL_register_atom(atom_t atom)
		PL_unregister_atom(atom_t atom)

	# References from the Prolog stacks
	Reference counting is unacceptable here, which implies a pass
	similar to the normal garbage-collector is required to deal with
	them. It might be worthwhile to include the atom
	garbage-collection in the normal garbage collector.

	# References from other structures
	Various of the structures contain or may contain atom
	references.  There are two options: lock/unlock them using
	PL_register_atom() on creation/destruction of the structure
	or enumerate them and flag the atoms.  The choice depends a
	bit on the structure.  FunctorDef for example is not garbage
	collected itself, so simply locking it makes sence.

	# References from compiled code and records
	Again both aproaches are feasible.  Reference counting is
	easy, except that the destruction of clauses and records
	will be more slowly as the code needs to be analysed for
	atom-references.

Reclaiming
----------

To reclaim an atom, it needs to be   deleted from the hash-table, a NULL
pointer should be set in the dynamic array and the structure needs to be
disposed using unalloc(). Basically, this is not a hard job.

The dynamic array will get holes   and  registerAtom() should first spot
for a hole (probably using a globally   defined index that points to the
first location that might be a hole   to avoid repetive scanning for the
array while there is no place anyway).   It cannot be shrunk, unless all
atoms above a certain index are gone. There is simply no way to find all
atom references (that why we need the PL_register_atom()).

In an advanced form, the hash-table could be shrunk, but it is debatable
whether this is worth the trouble. So, alltogether the system will waist
an average 1.5 machine word per reclaimed  atom, which will be reused as
the atom-space grows again.


Status
------

Some prelimary parts of atom garbage   collection  have been implemented
and flagged using #ifdef O_ATOMGC  ...   #endif.  The foreign code hooks
have been defined as well.


Atom GC and multi-threading
---------------------------

This is a hard problem. I think the   best  solution is to add something
like PL_thread_signal_async(int tid, void (*f)(void)) and call this from
the invoking thread on all other threads.   These  thread will then scan
their stacks and mark any references from their. Next they can carry on,
as intptr_t as the invoking thread keeps   the  atom mutex locked during the
whole atom garbage collection process. This   implies  the thread cannot
create any atoms as intptr_t as the collection is going on.

We do have to define some mechanism to   know  all threads are done with
their marking.

Don't know yet about Windows.  They   can't  do anything asynchronously.
Maybe they have ways to ensure  all   other  threads  are sleeping for a
while, so we can control the whole  process from the invoking thread. If
this is the case we could also do this in Unix:

	thread_kill(<thread>, SIG_STOP);
	<mark from thread>;
	thread_kill(<thread>, SIG_CONT);

Might be wise  to  design  the  marking   routine  suitable  to  take  a
PL_local_data term as argument, so it can be called from any thread.

All this will only work if we can call the atom garbage synchronously!!!

Measures to allow for asynchronous atom GC
------------------------------------------

	* lookupAtom() returns a referenced atom
	If not, it can be collected right-away!  Actually this might be
	a good idea anyway to avoid foreign-code that caches atoms from
	having to be updated.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void	rehashAtoms();

#define atom_buckets GD->atoms.buckets
#define atomTable    GD->atoms.table

#if O_DEBUG
#define lookups GD->atoms.lookups
#define	cmps	GD->atoms.cmps
#endif

#define LOCK()   PL_LOCK(L_ATOM)
#define UNLOCK() PL_UNLOCK(L_ATOM)
#undef LD
#define LD LOCAL_LD

		 /*******************************
		 *	      TYPES		*
		 *******************************/

static PL_blob_t text_atom =
{ PL_BLOB_MAGIC,
  PL_BLOB_UNIQUE|PL_BLOB_TEXT,		/* unique representation of text */
  "text"
};


static PL_blob_t unregistered_blob_atom =
{ PL_BLOB_MAGIC,
  PL_BLOB_NOCOPY|PL_BLOB_TEXT,
  "unregistered"
};


void
PL_register_blob_type(PL_blob_t *type)
{ PL_LOCK(L_MISC);			/* cannot use L_ATOM */

  if ( !type->registered )
  { if ( !GD->atoms.types )
    { GD->atoms.types = type;
      type->atom_name = ATOM_text;	/* avoid deadlock */
      type->registered = TRUE;
    } else
    { PL_blob_t *t = GD->atoms.types;

      while(t->next)
	t = t->next;

      t->next = type;
      type->rank = t->rank+1;
      type->registered = TRUE;
      type->atom_name = PL_new_atom(type->name);
    }

  }

  PL_UNLOCK(L_MISC);
}


PL_blob_t *
PL_find_blob_type(const char *name)
{ PL_blob_t *t;

  PL_LOCK(L_MISC);
  for(t = GD->atoms.types; t; t = t->next)
  { if ( streq(name, t->name) )
      break;
  }
  PL_UNLOCK(L_MISC);

  return t;
}



int
PL_unregister_blob_type(PL_blob_t *type)
{ unsigned int i;
  PL_blob_t **t;
  int discarded = 0;
  Atom *ap;

  PL_LOCK(L_MISC);
  for(t = &GD->atoms.types; *t; t = &(*t)->next)
  { if ( *t == type )
    { *t = type->next;
      type->next = NULL;
    }
  }
  PL_UNLOCK(L_MISC);

  PL_register_blob_type(&unregistered_blob_atom);

  LOCK();

  ap = GD->atoms.array;
  for(i=0; i < GD->atoms.count; i++, ap++ )
  { Atom atom;

    if ( (atom = *ap) )
    { if ( atom->type == type )
      { atom->type = &unregistered_blob_atom;

	atom->name = "<discarded blob>";
	atom->length = strlen(atom->name);

	discarded++;
      }
    }
  }
  UNLOCK();

  return discarded == 0 ? TRUE : FALSE;
}


		 /*******************************
		 *      BUILT-IN ATOM TABLE	*
		 *******************************/

#define ATOM(s) s

typedef const char * ccharp;
static const ccharp atoms[] = {
#include "pl-atom.ic"
  ATOM((char *)NULL)
};
#undef ATOM

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
It might be wise to  provide  for   an  option  that does not reallocate
atoms. In that case accessing a GC'ed   atom  causes a crash rather then
another atom.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
registerAtom(Atom a)
{ size_t n = GD->atoms.count;
#ifdef O_ATOMGC				/* try to find a hole! */
  Atom *ap = GD->atoms.array;
  Atom *ep = ap+n;
  Atom *p;

  for(p = &ap[GD->atoms.no_hole_before]; p < ep; p++)
  { if ( *p == NULL )
    { n = p - ap;
      *p = a;
      a->atom = (n<<LMASK_BITS)|TAG_ATOM;
      if ( indexAtom(a->atom) != (uintptr_t)n )
      {	/* TBD: user-level exception */
	fatalError("Too many (%d) atoms", n);
      }
      GD->atoms.no_hole_before = n+1;

      return;
    }
  }
  GD->atoms.no_hole_before = n+1;
#endif /*O_ATOMGC*/

  a->atom = (n<<LMASK_BITS)|TAG_ATOM;
  if ( n >= GD->atoms.array_allocated )
  { size_t newcount = GD->atoms.array_allocated * 2;
    size_t newsize  = newcount*sizeof(Atom);
    Atom *np = PL_malloc(newsize);

    memcpy(np, ap, newsize/2);
    GD->atoms.array = np;
    GD->atoms.array_allocated = newcount;
    PL_free(ap);
    ap = np;
  }
  ap[n++] = a;
  GD->atoms.count = n;
}


		 /*******************************
		 *	  GENERAL LOOKUP	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
(*) AGC starting. As we cannot run AGC if   we  are not in a safe state,
AGC is started using a   software interrupt using PL_raise(SIG_ATOM_GC).
Earlier versions only fired the signal   at exactly (last+margin) atoms,
but it is possible the signal is not  handled due to the thread dying or
the thread starting an indefinite  wait.   Therefore  we keep signalling
every 128 new atoms. Sooner or later   some  actually active thread will
pick up the request and process it.

PL_handle_signals() decides on the actual invocation of atom-gc and will
treat the signal as bogus if agc has already been performed.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
lookupBlob(const char *s, size_t length, PL_blob_t *type, int *new)
{ GET_LD
  unsigned int v0, v;
  uintptr_t oldheap;
  Atom a;

  if ( !type->registered )		/* avoid deadlock */
    PL_register_blob_type(type);

  startCritical;
  LOCK();
  v0 = MurmurHashAligned2(s, length, MURMUR_SEED);
  v  = v0 & (atom_buckets-1);
  DEBUG(0, lookups++);

  if ( true(type, PL_BLOB_UNIQUE) )
  { if ( false(type, PL_BLOB_NOCOPY) )
    { for(a = atomTable[v]; a; a = a->next)
      { DEBUG(0, cmps++);
	if ( length == a->length &&
	     type == a->type &&
	     memcmp(s, a->name, length) == 0 )
	{
#ifdef O_ATOMGC
	  if ( indexAtom(a->atom) >= GD->atoms.builtin )
	    a->references++;
#endif
          UNLOCK();
	  endCritical;
	  *new = FALSE;
	  return a->atom;
	}
      }
    } else
    { for(a = atomTable[v]; a; a = a->next)
      { DEBUG(0, cmps++);

	if ( length == a->length &&
	     type == a->type &&
	     s == a->name )
	{
#ifdef O_ATOMGC
	  a->references++;
#endif
          UNLOCK();
	  endCritical;
	  *new = FALSE;
	  return a->atom;
	}
      }
    }
  }

  oldheap = GD->statistics.heap;
  a = allocHeap(sizeof(struct atom));
  a->length = length;
  a->type = type;
  if ( false(type, PL_BLOB_NOCOPY) )
  { a->name = allocHeap(length+1);
    memcpy(a->name, s, length);
    a->name[length] = EOS;
  } else
  { a->name = (char *)s;
  }
#ifdef O_TERMHASH
  a->hash_value = v0;
#endif
#ifdef O_ATOMGC
  a->references = 1;
#endif
  registerAtom(a);
  if ( true(type, PL_BLOB_UNIQUE) )
  { a->next       = atomTable[v];
    atomTable[v]  = a;
  }
  GD->statistics.atoms++;

#ifdef O_ATOMGC
  if ( GD->atoms.margin != 0 &&
       GD->statistics.atoms >= GD->atoms.non_garbage + GD->atoms.margin )
  { intptr_t x = GD->statistics.atoms - (GD->atoms.non_garbage + GD->atoms.margin);

    if ( x % 128 == 0 )			/* see (*) above */
      PL_raise(SIG_ATOM_GC);
  }
#endif

  if ( atom_buckets * 2 < GD->statistics.atoms )
    rehashAtoms();

  GD->statistics.atomspace += (GD->statistics.heap - oldheap);

  UNLOCK();

  *new = TRUE;
  if ( type->acquire )
    (*type->acquire)(a->atom);
  endCritical;

  return a->atom;
}


word
lookupAtom(const char *s, size_t length)
{ int new;

  return lookupBlob(s, length, &text_atom, &new);
}


		 /*******************************
		 *	      ATOM-GC		*
		 *******************************/

#ifdef O_ATOMGC

#ifdef O_DEBUG_ATOMGC
static char *tracking;
IOSTREAM *atomLogFd;

void
_PL_debug_register_atom(atom_t a,
			const char *file, int line, const char *func)
{ int i = indexAtom(a);
  int mx = entriesBuffer(&atom_array, Atom);
  Atom atom;

  assert(i>=0 && i<mx);
  atom = fetchBuffer(&atom_array, i, Atom);

  atom->references++;
  if ( atomLogFd && strprefix(atom->name, tracking) )
    Sfprintf(atomLogFd, "%s:%d: %s(): ++ (%d) for `%s' (#%d)\n",
	     file, line, func, atom->references, atom->name, i);
}


void
_PL_debug_unregister_atom(atom_t a,
			  const char *file, int line, const char *func)
{ int i = indexAtom(a);
  int mx = entriesBuffer(&atom_array, Atom);
  Atom atom;

  assert(i>=0 && i<mx);
  atom = fetchBuffer(&atom_array, i, Atom);

  assert(atom->references >= 1);
  atom->references--;
  if ( atomLogFd && strprefix(atom->name, tracking) )
    Sfprintf(atomLogFd, "%s:%d: %s(): -- (%d) for `%s' (#%d)\n",
	     file, line, func, atom->references, atom->name, i);
}


Atom
_PL_debug_atom_value(atom_t a)
{ GET_LD
  int i = indexAtom(a);
  Atom atom = fetchBuffer(&atom_array, i, Atom);

  if ( !atom )
  { char buf[32];

    Sdprintf("*** No atom at index (#%d) ***", i);
    trap_gdb();

    atom = allocHeap(sizeof(*atom));
    Ssprintf(buf, "***(#%d)***", i);
    atom->name = store_string(buf);
    atom->length = strlen(atom->name);
  }

  return atom;
}


word
pl_track_atom(term_t which, term_t stream)
{ char *s;

  if ( tracking )
    remove_string(tracking);
  tracking = NULL;
  atomLogFd = NULL;

  if ( PL_get_nil(stream) )
    succeed;

  if ( !PL_get_chars(which, &s, CVT_LIST) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_list, which);
  if ( !PL_get_stream_handle(stream, &atomLogFd) )
    fail;

  tracking = store_string(s);

  succeed;
}
#endif /*O_DEBUG_ATOMGC*/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
lockAtoms() discards all currently defined atoms for garbage collection.
To be used after loading the program,   so we won't traverse the program
atoms each pass.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
lockAtoms()
{ GD->atoms.builtin     = GD->atoms.count;
  GD->atoms.non_garbage = GD->atoms.builtin;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Mark an atom from the stacks.  We must be prepared to handle fake-atoms!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
markAtom(atom_t a)
{ size_t i = indexAtom(a);
  Atom ap;

  if ( i >= GD->atoms.count )
    return;				/* not an atom */
  if ( i < GD->atoms.builtin )
    return;				/* locked range */

  ap = GD->atoms.array[i];

  if ( ap )
  {
#ifdef O_DEBUG_ATOMGC
    if ( atomLogFd )
      Sfprintf(atomLogFd, "Marked `%s' at (#%d)\n", ap->name, i);
#endif
    ap->references |= ATOM_MARKED_REFERENCE;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
destroyAtom()  actually  discards  an  atom.  The  code  marked  (*)  is
sometimes inserted to debug atom-gc. The   trick  is to create xxxx<...>
atoms that should *not* be subject to AGC.   If we find one collected we
know we trapped a bug.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
destroyAtom(Atom *ap, uintptr_t mask ARG_LD)
{ Atom a = *ap;
  Atom *ap2 = &atomTable[a->hash_value & mask];

  if ( a->type->release )
  { if ( !(*a->type->release)(a->atom) )
      return;
  } else if ( GD->atoms.gc_hook )
  { if ( !(*GD->atoms.gc_hook)(a->atom) )
      return;				/* foreign hooks says `no' */
  }

#if 0
  if ( strncmp(a->name, "xxxx", 4) == 0 ) 	/* (*) see above */
  { Sdprintf("Deleting %s\n", a->name);
    assert(0);
  }
#endif

#ifdef O_DEBUG_ATOMGC
  if ( atomLogFd )
    Sfprintf(atomLogFd, "Deleted `%s'\n", a->name);
#endif

  for( ; ; ap2 = &(*ap2)->next )
  { assert(*ap2);			/* MT: TBD: failed a few times!? */

    if ( *ap2 == a )
    { *ap2 = a->next;
      break;
    }
  }

  *ap = NULL;			/* delete from index array */
  GD->atoms.collected++;
  GD->statistics.atoms--;

  if ( false(a->type, PL_BLOB_NOCOPY) )
    freeHeap(a->name, a->length+1);
  freeHeap(a, sizeof(*a));
}


static void
collectAtoms(void)
{ GET_LD
  Atom *ap0 = GD->atoms.array;
  Atom *ap  = ap0 + GD->atoms.builtin;
  Atom *ep  = ap0 + GD->atoms.count;
  int hole_seen = FALSE;
  uintptr_t mask = atom_buckets-1;

  ap--;
  while(++ap < ep)
  { Atom a = *ap;

    if ( !a )
    { if ( !hole_seen )
      { hole_seen = TRUE;
	GD->atoms.no_hole_before = ap-ap0;
      }
      continue;
    }

    if ( a->references == 0 )
    { destroyAtom(ap, mask PASS_LD);
      if ( !hole_seen && *ap == NULL )
      { hole_seen = TRUE;
	GD->atoms.no_hole_before = ap-ap0;
      }
    } else
    { a->references &= ~ATOM_MARKED_REFERENCE;
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
pl_garbage_collect_atoms() realised the atom   garbage  collector (AGC).
This is a tricky beast that   needs  careful synchronisation with normal
GC. These issues are described with enterGC() in pl-gc.c.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
pl_garbage_collect_atoms()
{ GET_LD
  intptr_t oldheap, freed;
  int64_t oldcollected;
  int verbose;
  double t;
  sigset_t set;

  PL_LOCK(L_GC);
  if ( gc_status.blocked )		/* Tricky things; avoid problems. */
  { PL_UNLOCK(L_GC);
    succeed;
  }

#ifdef O_PLMT
  if ( GD->gc.active ) 			/* GC in progress: delay */
  { DEBUG(2, Sdprintf("GC active; delaying AGC\n"));
    GD->gc.agc_waiting = TRUE;
    PL_UNLOCK(L_GC);
    succeed;
  }
#endif

  gc_status.blocked++;			/* avoid recursion */

  if ( (verbose = truePrologFlag(PLFLAG_TRACE_GC)) )
  {
#ifdef O_DEBUG_ATOMGC
/*
    Sdprintf("Starting ATOM-GC.  Stack:\n");
    systemMode(TRUE);
    backTrace(NULL, 5);
    systemMode(FALSE);
*/
#endif
    printMessage(ATOM_informational,
		 PL_FUNCTOR_CHARS, "agc", 1,
		   PL_CHARS, "start");
  }

  PL_LOCK(L_THREAD);
  PL_LOCK(L_AGC);
  LOCK();
  blockSignals(&set);
  t = CpuTime(CPU_USER);
  markAtomsOnStacks(LD);
#ifdef O_PLMT
  markAtomsThreads();
  forThreadLocalData(markAtomsOnStacks, 0);
#endif
  oldcollected = GD->atoms.collected;
  oldheap = GD->statistics.heap;
  collectAtoms();
  GD->atoms.non_garbage = GD->statistics.atoms;
  t = CpuTime(CPU_USER) - t;
  GD->atoms.gc_time += t;
  GD->atoms.gc++;
  freed = oldheap - GD->statistics.heap;
  GD->statistics.atomspacefreed += freed;
  GD->statistics.atomspace -= freed;
  unblockSignals(&set);
  UNLOCK();
  PL_UNLOCK(L_AGC);
  PL_UNLOCK(L_THREAD);
  gc_status.blocked--;
  PL_UNLOCK(L_GC);

  if ( verbose )
    printMessage(ATOM_informational,
		 PL_FUNCTOR_CHARS, "agc", 1,
		   PL_FUNCTOR_CHARS, "done", 3,
		     PL_INT64, GD->atoms.collected - oldcollected,
		     PL_INT, GD->statistics.atoms,
		     PL_DOUBLE, (double)t);

  succeed;
}


PL_agc_hook_t
PL_agc_hook(PL_agc_hook_t new)
{ PL_agc_hook_t old = GD->atoms.gc_hook;
  GD->atoms.gc_hook = new;

  return old;
}


#endif /*O_ATOMGC*/

#undef PL_register_atom
#undef PL_unregister_atom

void
resetAtoms()
{
}


void
PL_register_atom(atom_t a)
{
#ifdef O_ATOMGC
  size_t index = indexAtom(a);

  if ( index >= GD->atoms.builtin )
  { Atom p;

    LOCK();
    p = GD->atoms.array[index];
    p->references++;
    UNLOCK();
  }
#endif
}

void
PL_unregister_atom(atom_t a)
{
#ifdef O_ATOMGC
  size_t index = indexAtom(a);

  if ( index >= GD->atoms.builtin )
  { Atom p;

    LOCK();
    p = GD->atoms.array[index];
    p->references--;
    if ( p->references == (unsigned)-1 )
    { Sdprintf("OOPS: -1 references to '%s'\n", p->name);
      trap_gdb();
    }
    UNLOCK();
  }
#endif
}

#define PL_register_atom error		/* prevent using them after this */
#define PL_unregister_atom error

		 /*******************************
		 *	    REHASH TABLE	*
		 *******************************/

static void
rehashAtoms()
{ GET_LD
  Atom *oldtab   = atomTable;
  int   oldbucks = atom_buckets;
  size_t mx = GD->atoms.count;
  uintptr_t mask;
  Atom *ap, *ep;

  startCritical;
  atom_buckets *= 2;
  mask = atom_buckets-1;
  atomTable = allocHeap(atom_buckets * sizeof(Atom));
  memset(atomTable, 0, atom_buckets * sizeof(Atom));

  DEBUG(0, Sdprintf("rehashing atoms (%d --> %d)\n", oldbucks, atom_buckets));

  for(ap = GD->atoms.array, ep = ap+mx;
      ap < ep;
      ap++)
  { Atom a = *ap;
    size_t v = a->hash_value & mask;

    a->next = atomTable[v];
    atomTable[v] = a;
  }

  freeHeap(oldtab, oldbucks * sizeof(Atom));
  endCritical;
}


word
pl_atom_hashstat(term_t idx, term_t n)
{ GET_LD
  long i, m;
  Atom a;

  if ( !PL_get_long(idx, &i) || i < 0 || i >= (long)atom_buckets )
    fail;
  for(m = 0, a = atomTable[i]; a; a = a->next)
    m++;

  return PL_unify_integer(n, m);
}


static void
registerBuiltinAtoms()
{ GET_LD
  int size = sizeof(atoms)/sizeof(char *) - 1;
  Atom a = allocHeap(size * sizeof(struct atom));
  const ccharp *s;

  GD->statistics.atoms = size;

  for(s = atoms; *s; s++, a++)
  { size_t len = strlen(*s);
    unsigned int v0 = MurmurHashAligned2(*s, len, MURMUR_SEED);
    unsigned int v = v0 & (atom_buckets-1);

    a->name       = (char *)*s;
    a->length     = len;
    a->type       = &text_atom;
#ifdef O_ATOMGC
    a->references = 0;
#endif
#ifdef O_TERMHASH
    a->hash_value = v0;
#endif
    a->next       = atomTable[v];
    atomTable[v]  = a;
    registerAtom(a);
  }
}


#if O_DEBUG
static void
exitAtoms(int status, void *arg)
{ Sdprintf("hashstat: %d lookupAtom() calls used %d strcmp() calls\n",
	   lookups, cmps);
}
#endif


void
initAtoms(void)
{ LOCK();
  if ( !atomTable )
  { GET_LD
    atom_buckets = ATOMHASHSIZE;
    atomTable = allocHeap(atom_buckets * sizeof(Atom));

    memset(atomTable, 0, atom_buckets * sizeof(Atom));
    GD->atoms.array_allocated = 4096;
    GD->atoms.array = PL_malloc(GD->atoms.array_allocated*sizeof(Atom));
    registerBuiltinAtoms();
#ifdef O_ATOMGC
    GD->atoms.margin = 10000;
    lockAtoms();
#endif
    PL_register_blob_type(&text_atom);

    DEBUG(0, PL_on_halt(exitAtoms, NULL));
  }
  UNLOCK();
}


void
cleanupAtoms(void)
{ PL_free(GD->atoms.array);
}


static word
current_blob(term_t a, term_t type, frg_code call, intptr_t i ARG_LD)
{ atom_t type_name = 0;

  switch( call )
  { case FRG_FIRST_CALL:
    { PL_blob_t *bt;

      if ( PL_is_blob(a, &bt) )
      { if ( type )
	  return PL_unify_atom(type, bt->atom_name);
	else if ( false(bt, PL_BLOB_TEXT) )
	  fail;

	succeed;
      }
      if ( !PL_is_variable(a) )
	return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_atom, a);

      i = 0;
      break;
    }
    case FRG_REDO:
      break;
    case FRG_CUTTED:
    default:
      succeed;
  }

  if ( type )
  { if ( !PL_is_variable(type) &&
	 !PL_get_atom_ex(type, &type_name) )
      fail;
  }

  for( ; i < (int)GD->atoms.count; i++ )
  { Atom atom;

    if ( (atom = GD->atoms.array[i]) )
    { if ( type )
      { if ( type_name && type_name != atom->type->atom_name )
	  continue;

	PL_unify_atom(type, atom->type->atom_name);
      } else if ( false(atom->type, PL_BLOB_TEXT) )
	continue;

      PL_unify_atom(a, atom->atom);
      ForeignRedoInt(i+1);
    }
  }

  fail;
}


static
PRED_IMPL("current_blob", 2, current_blob, PL_FA_NONDETERMINISTIC)
{ PRED_LD

  return current_blob(A1, A2, CTX_CNTRL, CTX_INT PASS_LD);
}


static
PRED_IMPL("current_atom", 1, current_atom, PL_FA_NONDETERMINISTIC)
{ PRED_LD

  return current_blob(A1, 0, CTX_CNTRL, CTX_INT PASS_LD);
}

static
PRED_IMPL("$atom_references", 2, atom_references, 0)
{ PRED_LD
  atom_t atom;

  if ( PL_get_atom_ex(A1, &atom) )
  { Atom av = atomValue(atom);

    return PL_unify_integer(A2, av->references);
  }

  fail;
}

		 /*******************************
		 *	 ATOM COMPLETION	*
		 *******************************/

#define ALT_SIZ 80		/* maximum length of one alternative */
#define ALT_MAX 256		/* maximum number of alternatives */
#define stringMatch(m)	((m)->name->name)

typedef struct match
{ Atom	name;
  int	length;
} *Match;


static bool
allAlpha(const char *s)
{ for( ; *s; s++)
  { if ( !isAlpha(*s) )
      fail;
  }
  succeed;
}


static int
extendAtom(char *prefix, bool *unique, char *common)
{ intptr_t i, mx = GD->atoms.count;
  Atom a;
  bool first = TRUE;
  int lp = (int) strlen(prefix);
  Atom *ap = GD->atoms.array;

  *unique = TRUE;

  for(i=0; i<mx; i++, ap++)
  { if ( (a=*ap) && strprefix(a->name, prefix) )
    { if ( strlen(a->name) >= LINESIZ )
	continue;
      if ( first == TRUE )
      { strcpy(common, a->name+lp);
	first = FALSE;
      } else
      { char *s = common;
	char *q = a->name+lp;
	while( *s && *s == *q )
	  s++, q++;
	*s = EOS;
	*unique = FALSE;
      }
    }
  }

  return !first;
}


word
pl_complete_atom(term_t prefix, term_t common, term_t unique)
{ char *p;
  bool u;
  char buf[LINESIZ];
  char cmm[LINESIZ];

  if ( !PL_get_chars_ex(prefix, &p, CVT_ALL) )
    fail;
  strcpy(buf, p);

  if ( extendAtom(p, &u, cmm) )
  { GET_LD

    strcat(buf, cmm);
    if ( PL_unify_list_codes(common, buf) &&
	 PL_unify_atom(unique, u ? ATOM_unique
				 : ATOM_not_unique) )
      succeed;
  }

  fail;
}


static int
compareMatch(const void *m1, const void *m2)
{ return strcmp(stringMatch((Match)m1), stringMatch((Match)m2));
}


static bool
extend_alternatives(char *prefix, struct match *altv, int *altn)
{ intptr_t i, mx = GD->atoms.count;
  Atom a, *ap;
  char *as;
  int l;

  *altn = 0;
  ap = GD->atoms.array;
  for(i=0; i<mx; i++, ap++)
  { if ( !(a=*ap) )
      continue;

    as = a->name;
    if ( strprefix(as, prefix) &&
	 allAlpha(as) &&
	 (l = (int)strlen(as)) < ALT_SIZ )
    { Match m = &altv[(*altn)++];
      m->name = a;
      m->length = l;
      if ( *altn > ALT_MAX )
	break;
    }
  }

  qsort(altv, *altn, sizeof(struct match), compareMatch);

  succeed;
}


word
pl_atom_completions(term_t prefix, term_t alternatives)
{ GET_LD
  char *p;
  char buf[LINESIZ];
  struct match altv[ALT_MAX];
  int altn;
  int i;
  term_t alts = PL_copy_term_ref(alternatives);
  term_t head = PL_new_term_ref();

  if ( !PL_get_chars_ex(prefix, &p, CVT_ALL) )
    fail;
  strcpy(buf, p);

  extend_alternatives(buf, altv, &altn);

  for(i=0; i<altn; i++)
  { if ( !PL_unify_list(alts, head, alts) ||
	 !PL_unify_atom(head, altv[i].name->atom) )
      fail;
  }

  return PL_unify_nil(alts);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Completeness generation for the GNU readline library. This function uses
a state variable to indicate  the   generator  should maintain/reset its
state. Horrible!

We must use thread-local data here.  Worse   is  we can't use the normal
Prolog one as there might  not  be   a  Prolog  engine associated to the
thread.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef O_PLMT
#include <pthread.h>
static pthread_key_t key;
#endif

#define is_signalled() (LD && LD->pending_signals != 0)

static int
alnum_text(PL_chars_t *txt)
{ switch(txt->encoding)
  { case ENC_ISO_LATIN_1:
    { const unsigned char *s = (const unsigned char *)txt->text.t;
      const unsigned char *e = &s[txt->length];

      for(; s<e; s++)
      { if ( !isAlpha(*s) )
	  return FALSE;
      }
      return TRUE;
    }
    case ENC_WCHAR:
    { const pl_wchar_t *s = (const pl_wchar_t*)txt->text.w;
      const pl_wchar_t *e = &s[txt->length];

      for(; s<e; s++)
      { if ( !isAlphaW(*s) )
	  return FALSE;
      }
      return TRUE;
    }
    default:
      assert(0);
      return FALSE;
  }
}


static int
atom_generator(PL_chars_t *prefix, PL_chars_t *hit, int state)
{ GET_LD
  intptr_t i, mx = GD->atoms.count;
  Atom *ap;

#ifdef O_PLMT
  if ( !key )
    pthread_key_create(&key, NULL);
#endif

  if ( !state )
    i = 0;
  else
  {
#ifdef O_PLMT
    i = (intptr_t)pthread_getspecific(key);
#else
    i = LD->atoms.generator;
#endif
  }

  for(ap=&GD->atoms.array[i]; i<mx; i++, ap++)
  { Atom a;

    if ( !(a = *ap) )
      continue;

    if ( is_signalled() )		/* Notably allow windows version */
      PL_handle_signals();		/* to break out on ^C */

    if ( get_atom_ptr_text(a, hit) &&
	 hit->length < ALT_SIZ &&
	 PL_cmp_text(prefix, 0, hit, 0, prefix->length) == 0 &&
	 alnum_text(hit) )
    {
#ifdef O_PLMT
      pthread_setspecific(key, (void *)(i+1));
#else
      LD->atoms.generator = i+1;
#endif

      return TRUE;
    }
  }

  return FALSE;
}


char *
PL_atom_generator(const char *prefix, int state)
{ PL_chars_t txt, hit;

  PL_init_text(&txt);
  txt.text.t   = (char *)prefix;
  txt.encoding = ENC_ISO_LATIN_1;
  txt.length   = strlen(prefix);

  while ( atom_generator(&txt, &hit, state) )
  { if ( hit.encoding == ENC_ISO_LATIN_1 )
      return hit.text.t;		/* text is from atoms, thus static */
    state = TRUE;
  }

  return NULL;
}


pl_wchar_t *
PL_atom_generator_w(const pl_wchar_t *prefix,
		    pl_wchar_t *buffer,
		    size_t buflen,
		    int state)
{ PL_chars_t txt, hit;

  PL_init_text(&txt);
  txt.text.w   = (pl_wchar_t *)prefix;
  txt.encoding = ENC_WCHAR;
  txt.length   = wcslen(prefix);

  for( ; atom_generator(&txt, &hit, state); state = TRUE )
  { if ( buflen > hit.length+1 )
    { if ( hit.encoding == ENC_WCHAR )
      { wcscpy(buffer, hit.text.w);
      } else
      { const unsigned char *s = (const unsigned char *)hit.text.t;
	const unsigned char *e = &s[hit.length];
	pl_wchar_t *o;

	for(o=buffer; s<e;)
	  *o++ = *s++;
	*o = EOS;
      }

      return buffer;
    }
  }

  return NULL;
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(atom)
  PRED_DEF("current_blob",  2, current_blob, PL_FA_NONDETERMINISTIC)
  PRED_DEF("current_atom", 1, current_atom, PL_FA_NONDETERMINISTIC)
  PRED_DEF("$atom_references", 2, atom_references, 0)
EndPredDefs
