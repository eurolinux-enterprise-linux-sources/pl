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

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Maximum number of clauses we  look  ahead   on  indexed  clauses  for an
alternative clause. If the choice is committed   this is lost effort, it
it reaches the end of the clause list   without  finding one the call is
deterministic.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define MAXSEARCH 100

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Clause indexing.  Clauses store an  `index  structure',  which  provides
summary information on the unification behaviour of the clause (e.i. its
head  arguments.   This  structure  consists  of  two words: a key and a
varmask.  Indexing can be done with upto 4 arguments.   Both  words  are
divided  into  the  same  number  of  bit  groups  as  there are indexed
arguments.  If an argument  is  indexable  (atom,  integer  or  compound
term),  the  corresponding  bit group is filled with bits taken from the
atom  pointer,  integer  or  functor  pointer.    In   this   case   all
corresponding  bits  in  the varmask field are 1.  Otherwise the bits in
both the varmask and the key are all 0.

To find a clause using indexing, we calculate an  index  structure  from
the  calling arguments to the goal using the same rules.  Now, we can do
a mutual `and' using the varmasks on the keys and  compare  the  result.
If  equal  a  good  chance  for a possible unification exists, otherwise
unification will definitely fail.  See matchIndex() and findClause().

Care has been taken to get this code as fast as  possible,  notably  for
indexing only on the first argument as this is default.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* 1 <= c <= 4 */

#define SHIFT(c, a)	((WORDBITSIZE/(c)) * a)
#define IDX_MASK(c)	(c == 1 ? ~(word)0 : (((word)1 << (WORDBITSIZE/(c))) - 1))
#define VM(c, a)	((word)((IDX_MASK(c) << SHIFT(c, a))))

#define Shift(c, a)	(mask_shift[c][a])
#define Mask(c)		(mask_mask[c])
#define varMask(c, a)	(variable_mask[c][a])

#define matchIndex(i1, i2)	(((i1).key & (i2).varmask) ==\
				 ((i2).key & (i1).varmask))

static uintptr_t variable_mask[][4] =
  { { 0,        0,        0,        0 },
#ifdef DONOT_AVOID_SHIFT_WARNING
    { VM(1, 0), 0,        0,        0 },
#else
    { ~(word)0, 0,        0,        0 },
#endif
    { VM(2, 0), VM(2, 1), 0,        0 },
    { VM(3, 0), VM(3, 1), VM(3, 2), 0 },
    { VM(4, 0), VM(4, 1), VM(4, 2), VM(4, 3) }
  };

static int mask_shift[][4] =
  { { 0,           0,           0,           0 },
    { SHIFT(1, 0), 0,           0,           0 },
    { SHIFT(2, 0), SHIFT(2, 1), 0,           0 },
    { SHIFT(3, 0), SHIFT(3, 1), SHIFT(3, 2), 0 },
    { SHIFT(4, 0), SHIFT(4, 1), SHIFT(4, 2), SHIFT(4, 3) }
  };

static word mask_mask[] =
  { 0,
#ifdef DONOT_AVOID_SHIFT_WARNING
    IDX_MASK(1),
#else
    ~(word)0,
#endif
    IDX_MASK(2), IDX_MASK(3), IDX_MASK(4)
  };


int
cardinalityPattern(unsigned long pattern)
{ int result = 0;

  for(; pattern; pattern >>= 1)
    if ( pattern & 0x1UL )
      result++;

  return result;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Compute the index in the hash-array from   a machine word and the number
of buckets. This used to be simple, but now that our tag bits are on the
left side, simply masking will put most things on the same hash-entry as
it is very common for all clauses of   a predicate to have the same type
of object. Hence, we now use exclusive or of the real value part and the
tag-bits.

NOTE: this function must be kept consistent with arg1Key() in pl-comp.c!
NOTE: This function returns 0 on non-indexable   fields, which is why we
guarantee that the value is non-0 for indexable values.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static inline int
hashIndex(word key, int buckets)
{ word k = key >> LMASK_BITS;

  return (int)((key^k) & (buckets-1));
}


static inline word
indexOfWord(word w ARG_LD)
{ for(;;)
  { switch(tag(w))
    { case TAG_VAR:
      case TAG_ATTVAR:
      case TAG_STRING:
	return 0L;
      case TAG_INTEGER:
	if ( storage(w) != STG_INLINE )
	{ Word p = valIndirectP(w);
	  word key;

#if SIZEOF_VOIDP == 4
          DEBUG(9, Sdprintf("Index for " INT64_FORMAT " = 0x%x\n",
			    valBignum(w), p[0]^p[1]));
	  key = p[0]^p[1];
#else
 	  key = p[0];
#endif
	  if ( !key )
	    key++;
          return key;
	}
        /*FALLTHROUGH*/
      case TAG_ATOM:
	break;				/* atom_t */
      case TAG_FLOAT:
      { Word p = valIndirectP(w);
	word key;

	switch(WORDS_PER_DOUBLE)
	{ case 2:
	    key = p[0]^p[1];
	    break;
	  case 1:
	    key = p[0];
	    break;
	  default:
	    assert(0);
	    return 0L;
	}

	if ( !key )
	  key++;
	return key;
      }
      case TAG_COMPOUND:
	w = *valPtr(w);			/* functor_t */
	break;
      case TAG_REFERENCE:
	w = *unRef(w);
	continue;
    }

    return w;
  }
}


void
getIndex(Word argv, unsigned long pattern, int card, struct index *index
	 ARG_LD)
{ if ( pattern == 0x1L )
  { index->key     = indexOfWord(*argv PASS_LD);
    index->varmask = (index->key ? ~(word)0 : (word)0);

    return;
  } else
  { word key;
    int a;

    index->key = 0;
    index->varmask = ~(word)0;			/* no variables */

    for(a = 0; a < card; a++, pattern >>= 1, argv++)
    { for(;(pattern & 0x1) == 0; pattern >>= 1)
	argv++;

      key = indexOfWord(*argv PASS_LD);
      if ( !key )
      { index->varmask &= ~varMask(card, a);
      } else
      { key = key ^ (key >> LMASK_BITS);	/* see hashIndex() */
	index->key |= ((key & Mask(card)) << Shift(card, a) );
      }
    }
  }

  return;
}


word
getIndexOfTerm(term_t t)
{ GET_LD
  word w = *valTermRef(t);

  return indexOfWord(w PASS_LD);
}


static ClauseRef
nextClauseMultiIndexed(ClauseRef cref, uintptr_t generation,
		       Word argv, Definition def,
		       ClauseRef *next ARG_LD)
{ struct index idx;

  getIndex(argv, def->indexPattern, def->indexCardinality, &idx PASS_LD);

  DEBUG(2, Sdprintf("Multi-argument indexing on %s ...",
		    cref ? procedureName(cref->clause->procedure) : "?"));

  for(; cref; cref = cref->next)
  { if ( matchIndex(idx, cref->clause->index) &&
	 visibleClause(cref->clause, generation))
    { ClauseRef result = cref;
      int maxsearch = MAXSEARCH;

      for( cref = cref->next; cref; cref = cref->next )
      { if ( (matchIndex(idx, cref->clause->index) &&
	      visibleClause(cref->clause, generation)) ||
	     --maxsearch == 0 )
	{ *next = cref;

	  DEBUG(2, Sdprintf("ndet\n"));
	  return result;
	}
      }
      DEBUG(2, Sdprintf("det\n"));
      *next = NULL;

      return result;
    }
  }
  DEBUG(2, Sdprintf("NULL\n"));

  return NULL;
}


static inline ClauseRef
nextClauseArg1(ClauseRef cref, uintptr_t generation,
	       ClauseRef *next, word key)
{ for(;cref ; cref = cref->next)
  { Clause clause = cref->clause;

    if ( (key & clause->index.varmask) == clause->index.key &&
	 visibleClause(clause, generation))
    { ClauseRef result = cref;
      int maxsearch = MAXSEARCH;

      for( cref = cref->next; cref; cref = cref->next )
      { clause = cref->clause;
	if ( ((key&clause->index.varmask) == clause->index.key &&
	      visibleClause(clause, generation)) ||
	     --maxsearch == 0 )
	{ *next = cref;

	  return result;
	}
      }
      *next = NULL;

      return result;
    }
  }

  return NULL;
}


ClauseRef
firstClause(Word argv, LocalFrame fr, Definition def, ClauseRef *next ARG_LD)
{ ClauseRef cref;

#ifdef O_LOGICAL_UPDATE
# define gen (fr->generation)
#else
# define gen 0L
#endif

again:
  if ( def->indexPattern == 0x0L )
  {
  noindex:
    for(cref = def->definition.clauses; cref; cref = cref->next)
    { if ( visibleClause(cref->clause, gen) )
      { *next = cref->next;
        return cref;
      }
    }
    return NULL;
  } else if ( def->indexPattern == 0x1L )
  { word key = indexOfWord(*argv PASS_LD);

    if ( key == 0L )
      goto noindex;

    if ( def->hash_info )
    { int hi = hashIndex(key, def->hash_info->buckets);

      cref = def->hash_info->entries[hi].head;
    } else
      cref = def->definition.clauses;

    return nextClauseArg1(cref, gen, next, key);
  } else if ( def->indexPattern & NEED_REINDEX )
  { reindexDefinition(def);
    goto again;
  } else
  { return nextClauseMultiIndexed(def->definition.clauses,
				  gen,
				  argv,
				  def,
				  next
				  PASS_LD);
  }

#undef gen
}


ClauseRef
findClause(ClauseRef cref, Word argv,
	   LocalFrame fr, Definition def, ClauseRef *next ARG_LD)
{
#ifdef O_LOGICAL_UPDATE
  #define gen (fr->generation)
#else
  #define gen 0L
#endif

  if ( def->indexPattern == 0x0L )	/* not indexed */
  { noindex:
    for(;;cref = cref->next)
    { if ( cref )
      { if ( visibleClause(cref->clause, gen) )
	{ *next = cref->next;
	  return cref;
	}
      } else
	return NULL;
    }
  } else if ( def->indexPattern == 0x1L ) /* first-argument indexing */
  { word key = indexOfWord(*argv PASS_LD);

    if ( !key )
      goto noindex;

    return nextClauseArg1(cref, gen, next, key);
  } else if ( def->indexPattern & NEED_REINDEX )
  { reindexDefinition(def);
    return findClause(cref, argv, fr, def, next PASS_LD);
  } else
  { return nextClauseMultiIndexed(cref, gen, argv, def, next PASS_LD);
  }

#undef gen
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Recalculate the index of  a  clause  after  the  index  pattern  on  the
predicate  has been changed.  The head of the clause is decompiled.  The
resulting term is simply discarded as it cannot have links to any  other
part of the stacks (e.g. backtrailing is not needed).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool
reindexClause(Clause clause, Definition def, unsigned long pattern)
{ if ( pattern == 0x0 )
    succeed;
  if ( false(clause, ERASED) )
  { if ( pattern == 0x1 )		/* the 99.9% case.  Speedup a little */
    { word key;

      if ( arg1Key(clause, FALSE, &key) )
      { clause->index.key     = key;
	clause->index.varmask = (uintptr_t)~0L;
      } else
      { clause->index.key     = 0L;
	clause->index.varmask = 0L;
      }
    } else
    { GET_LD

      fid_t fid = PL_open_foreign_frame();
      term_t head = PL_new_term_ref();

      decompileHead(clause, head);
      getIndex(argTermP(*valTermRef(head), 0),
	       pattern,
	       def->indexCardinality,
	       &clause->index
	       PASS_LD);
      PL_discard_foreign_frame(fid);
    }
  }

  succeed;
}


bool
unify_index_pattern(Procedure proc, term_t value)
{ GET_LD
  Definition def = proc->definition;
  uintptr_t pattern = (def->indexPattern & ~NEED_REINDEX);
  int n, arity = def->functor->arity;

  if ( pattern == 0 )
    fail;

  if ( PL_unify_functor(value, def->functor->functor) )
  { term_t a = PL_new_term_ref();

    for(n=0; n<arity; n++, pattern >>= 1)
    { if ( !PL_get_arg(n+1, value, a) ||
	   !PL_unify_integer(a, (pattern & 0x1) ? 1 : 0) )
	fail;
    }

    succeed;
  }

  fail;
}

		 /*******************************
		 *	   HASH SUPPORT		*
		 *******************************/

static ClauseIndex
newClauseIndexTable(int buckets)
{ GET_LD
  ClauseIndex ci = allocHeap(sizeof(struct clause_index));
  ClauseChain ch;
  int m = 4;

  while(m<buckets)
    m *= 2;
  buckets = m;

  ci->buckets  = buckets;
  ci->size     = 0;
  ci->alldirty = FALSE;
  ci->entries  = allocHeap(sizeof(struct clause_chain) * buckets);

  for(ch = ci->entries; buckets; buckets--, ch++)
  { ch->head = ch->tail = NULL;
    ch->dirty = 0;
  }

  return ci;
}


void
unallocClauseIndexTable(ClauseIndex ci)
{ GET_LD
  ClauseChain ch;
  int buckets = ci->buckets;

  for(ch = ci->entries; buckets; buckets--, ch++)
  { ClauseRef cr, next;

    for(cr = ch->head; cr; cr = next)
    { next = cr->next;
      freeHeap(cr, sizeof(*cr));
    }
  }

  freeHeap(ci->entries, ci->buckets * sizeof(struct clause_chain));
  freeHeap(ci, sizeof(struct clause_index));
}


static void
appendClauseChain(ClauseChain ch, Clause cl, int where ARG_LD)
{ ClauseRef cr = newClauseRef(cl PASS_LD);

  if ( !ch->tail )
    ch->head = ch->tail = cr;
  else
  { if ( where != CL_START )
    { ch->tail->next = cr;
      ch->tail = cr;
    } else
    { cr->next = ch->head;
      ch->head = cr;
    }
  }
}


static void
deleteClauseChain(ClauseChain ch, Clause clause)
{ ClauseRef prev = NULL;
  ClauseRef c;

  for(c = ch->head; c; prev = c, c = c->next)
  { if ( c->clause == clause )
    { if ( !prev )
      { ch->head = c->next;
	if ( !c->next )
	  ch->tail = NULL;
      } else
      { prev->next = c->next;
	if ( !c->next)
	  ch->tail = prev;
      }
    }
  }
}


static int
gcClauseChain(ClauseChain ch, int dirty ARG_LD)
{ ClauseRef cref = ch->head, prev = NULL;
  int deleted = 0;

  while( cref && dirty != 0 )
  { if ( true(cref->clause, ERASED) )
    { ClauseRef c = cref;

      if ( dirty > 0 )
      { assert(c->clause->index.varmask != 0); /* must be indexed */
	deleted++;
	dirty--;
      }

      cref = cref->next;
      if ( !prev )
      { ch->head = c->next;
	if ( !c->next )
	  ch->tail = NULL;
      } else
      { prev->next = c->next;
	if ( c->next == NULL)
	  ch->tail = prev;
      }

      freeClauseRef(c PASS_LD);
    } else
    { prev = cref;
      cref = cref->next;
    }
  }

  ch->dirty = 0;

  return deleted;
}


#define INFINT (~(1<<(INTBITSIZE-1)))

void
gcClauseIndex(ClauseIndex ci ARG_LD)
{ ClauseChain ch = ci->entries;
  int n = ci->buckets;

  if ( ci->alldirty )
  { for(; n; n--, ch++)
      ci->size -= gcClauseChain(ch, -1 PASS_LD); /* do them all */
  } else
  { for(; n; n--, ch++)
    { if ( ch->dirty )
	ci->size -= gcClauseChain(ch, ch->dirty PASS_LD);
    }
  }
}


void
markDirtyClauseIndex(ClauseIndex ci, Clause cl)
{ if ( cl->index.varmask == 0 )
    ci->alldirty = TRUE;
  else
  { int hi = hashIndex(cl->index.key, ci->buckets);
    ci->entries[hi].dirty++;
  }
}


/* MT: caller must have predicate locked */

void
addClauseToIndex(Definition def, Clause cl, int where ARG_LD)
{ ClauseIndex ci = def->hash_info;
  ClauseChain ch = ci->entries;

  if ( cl->index.varmask == 0 )		/* a non-indexable field */
  { int n = ci->buckets;

    SECURE({ word k;
	     assert(!arg1Key(cl, FALSE, &k));
	   });

    DEBUG(1,
	  if ( def->indexPattern == 0x1 )
	    Sdprintf("*** Adding unindexed clause to index of %s\n",
		     predicateName(def)));

    for(; n; n--, ch++)
      appendClauseChain(ch, cl, where PASS_LD);
  } else
  { int hi = hashIndex(cl->index.key, ci->buckets);

    DEBUG(4, Sdprintf("Storing in bucket %d\n", hi));
    appendClauseChain(&ch[hi], cl, where PASS_LD);
    ci->size++;
  }
}


void
delClauseFromIndex(Definition def, Clause cl)
{ ClauseIndex ci = def->hash_info;
  ClauseChain ch = ci->entries;

  if ( cl->index.varmask == 0 )		/* a non-indexable field */
  { int n = ci->buckets;

    for(; n; n--, ch++)
      deleteClauseChain(ch, cl);
  } else
  { int hi = hashIndex(cl->index.key, ci->buckets);

    deleteClauseChain(&ch[hi], cl);
    ci->size--;
    if ( false(def, NEEDSREHASH) && ci->size*4 < ci->buckets )
    { set(def, NEEDSREHASH);
      if ( true(def, DYNAMIC) && def->references == 0 )
      { DEBUG(0, Sdprintf("Should clean %s\n", predicateName(def)));
        /* TBD: need to clear right away if dynamic and not referenced */
        /* see assertProcedure() for similar case.  To do that locking */
        /* needs to be sorted out */
      }
    }
  }
}


/* MT: Caller must have predicate locked
*/

bool
hashDefinition(Definition def, int buckets)
{ GET_LD
  ClauseRef cref;

  DEBUG(2, Sdprintf("hashDefinition(%s, %d)\n", predicateName(def), buckets));

  def->hash_info = newClauseIndexTable(buckets);

  for(cref = def->definition.clauses; cref; cref = cref->next)
  { if ( false(cref->clause, ERASED) )
      addClauseToIndex(def, cref->clause, CL_END PASS_LD);
  }

  succeed;
}

word
pl_hash(term_t pred)
{ Procedure proc;

  if ( get_procedure(pred, &proc, 0, GP_CREATE) )
  { GET_LD
    Definition def = getProcDefinition(proc);
    int size, minsize;

    if ( def->hash_info )		/* already hashed; won't change */
      succeed;

    if ( true(def, FOREIGN) )
      return PL_error(NULL, 0, NULL, ERR_PERMISSION_PROC,
		      ATOM_hash, ATOM_foreign, proc);

    LOCKDEF(def);
    indexDefinition(def, 0x1L);		/* index in 1st argument */

    minsize = def->number_of_clauses / 4,
    size = 64;
    while (size < minsize)
      size *= 2;

					/* == reindexDefinition(), but */
					/* we cannot call this as it would */
					/* deadlock */
    if ( def->indexPattern & NEED_REINDEX )
    { ClauseRef cref;

      def->indexCardinality = 1;
      for(cref = def->definition.clauses; cref; cref = cref->next)
	reindexClause(cref->clause, def, 0x1L);
      def->indexPattern = 0x1L;
    }

    hashDefinition(def, size);
    UNLOCKDEF(def);

    succeed;
  }

  fail;
}
