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

#ifdef SECURE_GC
#define O_SECURE 1			/* include checkData() */
#endif
#include "pl-incl.h"


		/********************************
		*    CALLING THE INTERPRETER    *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Starts a new Prolog toplevel.  Resets I/O to point to the user and stops
the debugger.  Restores I/O and debugger on exit.  The Prolog  predicate
`$break' is called to actually built the break environment.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
pl_break()
{ fid_t wake = saveWakeup(PASS_LD1);
  fid_t cid = PL_open_foreign_frame();
  term_t goal = PL_new_term_ref();
  word rval;

  PL_put_atom_chars(goal, "$break");
  rval = pl_break1(goal);
  PL_discard_foreign_frame(cid);
  restoreWakeup(wake PASS_LD);

  return rval;
}


word
pl_break1(term_t goal)
{ bool rval;

  IOSTREAM *inSave  = Scurin;
  IOSTREAM *outSave = Scurout;
  intptr_t skipSave     = debugstatus.skiplevel;
  int  suspSave     = debugstatus.suspendTrace;
  int  traceSave;
  debug_type debugSave;

  tracemode(FALSE, &traceSave);
  debugmode(DBG_OFF, &debugSave);

  Scurin  = Sinput;
  Scurout = Soutput;

  resetTracer();

  for(;;)
  { fid_t cid = PL_open_foreign_frame();
    term_t ex;

    rval = callProlog(MODULE_user, goal,
		      PL_Q_NORMAL|PL_Q_CATCH_EXCEPTION, &ex);
    if ( !rval && ex )
    { printMessage(ATOM_error,
		   PL_FUNCTOR_CHARS, "unhandled_exception", 1,
		     PL_TERM, ex);
      tracemode(FALSE, NULL);
      debugmode(DBG_OFF, NULL);
      PL_put_atom(goal, ATOM_prolog);
    } else
    { break;
    }

    PL_discard_foreign_frame(cid);
  }

  debugstatus.suspendTrace = suspSave;
  debugstatus.skiplevel    = skipSave;
  tracemode(traceSave, NULL);
  debugmode(debugSave, NULL);

  Scurout = outSave;
  Scurin  = inSave;

  return rval;
}


word
pl_notrace1(term_t goal)
{ bool rval;

  intptr_t	     skipSave  = debugstatus.skiplevel;
  bool	     traceSave = debugstatus.tracing;

  rval = callProlog(NULL, goal, PL_Q_NODEBUG, NULL);

  debugstatus.skiplevel    = skipSave;
  debugstatus.tracing      = traceSave;

  return rval;
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Call a prolog goal from C. The argument must  be  an  instantiated  term
like for the Prolog predicate call/1.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
callProlog(Module module, term_t goal, int flags, term_t *ex)
{ term_t g = PL_new_term_ref();
  functor_t fd;
  Procedure proc;

  if ( ex )
    *ex = 0;

  PL_strip_module(goal, &module, g);
  if ( !PL_get_functor(g, &fd) )
  { PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, goal);
    if ( ex )
      *ex = exception_term;

    fail;
  }

  proc = lookupProcedure(fd, module);

  { int arity = arityFunctor(fd);
    term_t args = PL_new_term_refs(arity);
    qid_t qid;
    int n, rval;

    for(n=0; n<arity; n++)
      _PL_get_arg(n+1, g, args+n);

    qid  = PL_open_query(module, flags, proc, args);
    rval = PL_next_solution(qid);
    if ( !rval && ex )
      *ex = PL_exception(qid);
    PL_cut_query(qid);

    return rval;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Abort and toplevel. At the  moment,   prologToplevel()  sets a longjmp()
context and pl_abort() jumps to this   context and resets the SWI-Prolog
engine.

Using the multi-threaded version, this is   not  acceptable. Each thread
needs such a context, but worse  is   that  we cannot properly reset the
reference count and ensure locks are all in a sane state.

A cleaner solution is to  map  an   abort  onto  a Prolog exception. The
exception-handling   code   should    ensure     proper    handling   of
reference-counts and locks anyhow. Small disadvantage   is  that the old
abort()   mechanism   was   capable   of     recovering   from   serious
data-inconsistencies, while the throw-based requires   the Prolog engine
to be in a sane state.  Anyhow,   in  the multi-threaded version we have
little choice.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef O_PLMT
#define O_ABORT_WITH_THROW 1
#endif

static word
pl_throw_abort()
{ pl_notrace();
  Sreset();

  if ( LD->critical > 0 )		/* abort in critical region: delay */
  { LD->aborted = TRUE;
    succeed;
  } else
  { fid_t fid = PL_open_foreign_frame();
    term_t ex = PL_new_term_ref();

    clearSegStack(&LD->cycle.stack);	/* can do no harm */

    PL_put_atom(ex, ATOM_aborted);
    PL_throw(ex);			/* use longjmp() to ensure */

    PL_close_foreign_frame(fid);	/* should not be reached */
    fail;
  }
}


#ifdef O_ABORT_WITH_THROW

word
pl_abort(abort_type type)
{ return pl_throw_abort();
}

#else /*O_ABORT_WITH_THROW*/

static jmp_buf abort_context;		/* jmp buffer for abort() */
static int can_abort;			/* embeded code can't abort */

word
pl_abort(abort_type type)
{ if ( !can_abort ||
       (truePrologFlag(PLFLAG_EX_ABORT) && type == ABORT_NORMAL) )
    return pl_throw_abort();

  if ( LD->critical > 0 )		/* abort in critical region: delay */
  { pl_notrace();
    LD->aborted = TRUE;
    succeed;
  }

  if ( !truePrologFlag(PLFLAG_READLINE) )
    PopTty(Sinput, &ttytab);
  LD->outofstack = NULL;
  clearSegStack(&LD->cycle.stack);
  closeFiles(FALSE);
  resetReferences();
#ifdef O_PROFILE
  resetProfiler();
#endif
  resetStacks();
  resetTracer();
  resetSignals();
  resetForeign();
  resetAtoms();
  updateAlerted(LD);

  longjmp(abort_context, 1);
  /*NOTREACHED*/
  fail;
}

#endif /*O_ABORT_WITH_THROW*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
prologToplevel(): Initial entry point from C to start the Prolog engine.
Saves abort context, clears the  stack   and  finally starts the virtual
machine interpreter with the toplevel goal.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
resetProlog()
{
  if ( !LD->gvar.nb_vars )		/* we would loose nb_setval/2 vars */
    emptyStacks();

#ifdef O_LIMIT_DEPTH
  depth_limit   = (uintptr_t)DEPTH_NO_LIMIT;
#endif

  gc_status.blocked    = 0;
#if O_SHIFT_STACKS
  LD->shift_status.blocked = 0;
#endif
  LD->in_arithmetic    = 0;

  tracemode(FALSE, NULL);
  debugmode(DBG_OFF, NULL);
  debugstatus.suspendTrace = 0;

#ifndef O_ABORT_WITH_THROW
  can_abort = TRUE;
#endif

  updateAlerted(LD);
}


bool
prologToplevel(volatile atom_t goal)
{ bool rval;
  volatile int aborted = FALSE;

#ifndef O_ABORT_WITH_THROW
  if ( setjmp(abort_context) != 0 )
  { if ( LD->current_signal )
      unblockSignal(LD->current_signal);

    aborted = TRUE;
  } else
#endif
  { debugstatus.debugging = DBG_OFF;
  }

  for(;;)
  { fid_t fid;
    qid_t qid;
    term_t except = 0;
    Procedure p;
    word gn;

    resetProlog();
    fid = PL_open_foreign_frame();

    if ( aborted )
    { aborted = FALSE;
      gn = PL_new_atom("$abort");
    } else
      gn = goal;

    p = lookupProcedure(lookupFunctorDef(gn, 0), MODULE_system);

    qid = PL_open_query(MODULE_system, PL_Q_NORMAL, p, 0);
    rval = PL_next_solution(qid);
    if ( !rval && (except = PL_exception(qid)) )
    { atom_t a;

      tracemode(FALSE, NULL);
      debugmode(DBG_OFF, NULL);
      setPrologFlagMask(PLFLAG_LASTCALL);
      if ( PL_get_atom(except, &a) && a == ATOM_aborted )
      { aborted = TRUE;
      } else if ( !PL_is_functor(except, FUNCTOR_error2) )
      { printMessage(ATOM_error,
		     PL_FUNCTOR_CHARS, "unhandled_exception", 1,
		       PL_TERM, except);
      }
    }
    PL_close_query(qid);
    PL_discard_foreign_frame(fid);
    if ( !except )
      break;
  }
#ifndef O_ABORT_WITH_THROW
  can_abort = FALSE;
#endif

  return rval;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Cut (!) as called via the  meta-call  mechanism has no effect.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
pl_metacut(void)
{ succeed;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Just for debugging now and then.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
trap_gdb()
{ return 0;
}

#if O_SECURE || O_DEBUG || defined(O_MAINTENANCE)

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
checkData(p) verifies p points to valid  Prolog  data  and  generates  a
system  error  otherwise.  The checks performed are much more rigid than
those during normal execution.  Arity of terms is limited to  100  as  a
kind of heuristic.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define onGlobal(p) onStack(global, p)
#define onLocal(p) onStack(local, p)
#define onHeap(p) ((char *)p >= (char *)hBase && (char *)p <= (char *)hTop)

static void
printk(char *fm, ...)
{ va_list args;

  va_start(args, fm);
  Sfprintf(Serror, "[DATA INCONSISTENCY: ");
  Svfprintf(Serror, fm, args);
  Sfprintf(Serror, "]\n");
  va_end(args);

  trap_gdb();
}

static intptr_t check_marked;

#define mark(p)		(*(p) |= MARK_MASK, check_marked++)
#define unmark(p)	(*(p) &= ~MARK_MASK, check_marked--)

static void
unmark_data(Word p)
{
last_arg:
  deRef(p);

  if ( isAttVar(*p) && is_marked(p) )
  { unmark(p);
    p = valPAttVar(*p);
    goto last_arg;
  } else if ( isTerm(*p) && is_marked(p) )
  { Functor f;
    int n, arity;

    unmark(p);
    f = valueTerm(*p);
    arity = arityFunctor(f->definition);
    for(n=0; n<arity-1; n++)
      unmark_data(&f->arguments[n]);
    p = &f->arguments[n];
    goto last_arg;
  }
}


static int				/* avoid false alarm in CHR */
is_ht_capacity(int arity)
{ int cap = 89;				/* chr_hashtable_store.pl */

  while(cap < arity)
    cap = cap*2+1;

  return cap == arity;
}


static word
check_data(Word p, int *recursive)
{ int arity; int n;
  Word p2;
  word key = 0L;

last_arg:

  while(isRef(*p))
  { assert(!is_marked(p));
    p2 = unRef(*p);
    if ( p2 > p )
    {
#ifdef O_ATTVAR
      if ( !isAttVar(*p2) )
#endif
         printk("Reference to higher address");
    }
    if ( p2 == p )
      printk("Reference to same address");
    if ( !onLocal(p2) && !onGlobal(p2) )
      printk("Illegal reference pointer at 0x%x --> 0x%x", p, p2);

    p = p2;
  }

  if ( isVar(*p) )
    return key+0x737473;		/* just a random number */

#ifdef O_ATTVAR
  if ( isAttVar(*p) )
  { if ( is_marked(p) )			/* loop */
    { (*recursive)++;
      return key;
    }

    key += 0x427e8ac;			/* another random number */
    p2 = valPAttVar(*p);
    mark(p);

    if ( !onGlobal(p) )
      printk("attvar: not on global stack: 0x%x", p);
    if ( !onGlobal(p2) )
      printk("attvar: attribute not on global stack: 0x%x --> 0x%x", p, p2);
    if ( p == p2 )
      printk("attvar: self-reference: 0x%x", p);

    p = p2;
    goto last_arg;
  }
#endif

  if ( isTaggedInt(*p) )
  { assert(!is_marked(p));
    return key + *p;
  }

  if ( isIndirect(*p) )
  { Word a = addressIndirect(*p);

    assert(!is_marked(p));

    if ( !onGlobal(a) )
      printk("Indirect at %p not on global stack", a);
    if ( storage(*p) != STG_GLOBAL )
      printk("Indirect data not on global");
    if ( isBignum(*p) )
      return key+(word) valBignum(*p);
    if ( isFloat(*p) )
      return key+(word) valFloat(*p);
    if ( isString(*p) )
    { if ( isBString(*p) )
      { size_t sz, len;
	char *s;

	s = getCharsString(*p, &sz);

	if ( sz != (len=strlen(s)) )
	{ if ( sz < len )
	    printk("String has inconsistent length: 0x%x", *p);
	  else if ( s[sz] )
	    printk("String not followed by NUL-char: 0x%x", *p);
/*	else
	    printf("String contains NUL-chars: 0x%x", *p);
*/
	}
      } else
      { size_t sz, len;
	pl_wchar_t *s;

	s = getCharsWString(*p, &sz);

	if ( sz != (len=wcslen(s)) )
	{ if ( sz < len )
	    printk("String has inconsistent length: 0x%x", *p);
	  else if ( s[sz] )
	    printk("String not followed by NUL-char: 0x%x", *p);
	}
      }
      return key + *addressIndirect(*p);
    }
#ifdef O_GMP
    if ( isMPZNum(*p) )
      return 0x62f8da3c;		/* TBD: make key from MPZ */
#endif
    printk("Illegal indirect datatype");
    return key;
  }

  if ( isAtom(*p) )
  { uintptr_t idx;
    uintptr_t mx = GD->atoms.count;

    assert(!is_marked(p));
    if ( storage(*p) != STG_STATIC )
      printk("Atom doesn't have STG_STATIC");

    idx = indexAtom(*p);
    if ( idx > mx )
      printk("Atom index out of range (%ld > %ld)", idx, mx);
    return key + *p;
  }
					/* now it should be a term */
  if ( tag(*p) != TAG_COMPOUND ||
       storage(*p) != STG_GLOBAL )
    printk("Illegal term at: %p: 0x%x", p, *p);

  if ( is_marked(p) )
  { (*recursive)++;
    return key;				/* recursive */
  }

  { Functor f = valueTerm(*p);
    mark(p);

    if ( !onGlobal(f) )
      printk("Term at %p not on global stack", f);

    if ( tag(f->definition) != TAG_ATOM ||
         storage(f->definition) != STG_GLOBAL )
      printk("Illegal term: 0x%x", *p);
    arity = arityFunctor(f->definition);
    if ( arity < 0 )
      printk("Illegal arity (%d)", arity);
    else if ( arity > 256 && !is_ht_capacity(arity) )
      printk("Dubious arity (%d)", arity);
    for(n=0; n<arity-1; n++)
      key += check_data(&f->arguments[n], recursive);

    p = &f->arguments[n];
    goto last_arg;
  }
}


word
checkData(Word p)
{ int recursive = 0;
  word key;

  key = check_data(p, &recursive);
  unmark_data(p);

  return key;
}

#endif /* TEST */
