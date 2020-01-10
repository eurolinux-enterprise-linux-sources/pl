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

#include "pl-incl.h"
#include "pl-ctype.h"

#define WFG_TRACE	0x01000
#define WFG_TRACING	0x02000
#define WFG_BACKTRACE	0x04000
#define WFG_CHOICE	0x08000

#define TRACE_FIND_NONE	0
#define TRACE_FIND_ANY	1
#define TRACE_FIND_NAME	2
#define TRACE_FIND_TERM	3

typedef struct find_data_tag
{ int	 port;				/* Port to find */
  bool	 searching;			/* Currently searching? */
  int	 type;				/* TRACE_FIND_* */
  union
  { atom_t	name;			/* Name of goal to find */
    struct
    { functor_t	functor;		/* functor of the goal */
      Record	term;			/* Goal to find */
    } term;
  } goal;
} find_data;


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Convert between integer frame reference and LocalFrame pointer.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
PL_unify_frame(term_t t, LocalFrame fr)
{ if ( fr )
  { assert(fr >= lBase && fr < lTop);

    return PL_unify_integer(t, (Word)fr - (Word)lBase);
  } else
    return PL_unify_atom(t, ATOM_none);
}


void
PL_put_frame(term_t t, LocalFrame fr)
{ if ( fr )
  { assert(fr >= lBase && fr < lTop);

    PL_put_intptr(t, (Word)fr - (Word)lBase);
  } else
    PL_put_atom(t, ATOM_none);
}


static int
PL_get_frame(term_t r, LocalFrame *fr)
{ long i;
  atom_t a;

  if ( PL_get_long(r, &i) )
  { LocalFrame f = ((LocalFrame)((Word)lBase + i));

    if ( !(f >= lBase && f < lTop) )
      fail;
    *fr = f;

    succeed;
  } else if ( PL_get_atom(r, &a) && a == ATOM_none )
  { *fr = NULL;

    succeed;
  }

  fail;
}


static void
PL_put_choice(term_t t, Choice ch)
{ if ( ch )
  { assert(ch >= (Choice)lBase && ch < (Choice)lTop);

    PL_put_intptr(t, (Word)ch - (Word)lBase);
  } else
    PL_put_atom(t, ATOM_none);
}


static int
PL_unify_choice(term_t t, Choice ch)
{ if ( ch )
  { assert(ch >= (Choice)lBase && ch < (Choice)lTop);

    return PL_unify_integer(t, (Word)ch - (Word)lBase);
  } else
    return PL_unify_atom(t, ATOM_none);
}


static int
PL_get_choice(term_t r, Choice *chp)
{ long i;

  if ( PL_get_long(r, &i) )
  { Choice ch = ((Choice)((Word)lBase + i));

    assert(ch >= (Choice)lBase && ch < (Choice)lTop);
    *chp = ch;

    succeed;
  } else
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_choice, r);
}


#ifdef O_DEBUGGER

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
isDebugFrame(LocalFrame FR) is true if this call  must be visible in the
tracer. `No-debug' code has HIDE_CHILDS. Calls to  it must be visible if
the parent is a debug frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
isDebugFrame(LocalFrame FR)
{ if ( false(FR->predicate, TRACE_ME) )
    return FALSE;			/* hidden predicate */

  if ( false(FR->predicate, HIDE_CHILDS) )
    return TRUE;			/* user pred */

  if ( FR->parent )
  { LocalFrame parent = FR->parent;

    if ( levelFrame(FR) == levelFrame(parent)+1 )
    {					/* not last-call optimized */
      if ( false(parent->predicate, HIDE_CHILDS) )
	return TRUE;			/* user calls system */
      return FALSE;			/* system cals system */
    } else
    { if ( false(parent, FR_HIDE_CHILDS) )
	return TRUE;
      return FALSE;
    }
  } else
  { QueryFrame qf = queryOfFrame(FR);

    return (qf->flags & PL_Q_NODEBUG) ? FALSE : TRUE;
  }
}


static void
exitFromDebugger(int status)
{
#ifdef O_PLMT
  if ( PL_thread_self() > 1 )
    pthread_exit(NULL);
#endif
  PL_halt(status);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module defines the tracer and interrupt  handler  that  allows  the
user  to break the normal Prolog execution.  The tracer is written in C,
but before taking action it calls Prolog.   This  mechanism  allows  the
user to intercept and redefine the tracer.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

					/* Frame <-> Prolog integer */
forwards LocalFrame	redoFrame(LocalFrame, Code *PC);
forwards void		helpTrace(void);
#ifdef O_INTERRUPT
forwards void		helpInterrupt(void);
#endif
forwards bool		hasAlternativesFrame(LocalFrame);
static void		alternatives(Choice);
static void		exceptionDetails(void);
forwards void		listGoal(LocalFrame frame);
forwards int		traceInterception(LocalFrame, Choice, int, Code);
static int		traceAction(char *cmd,
				    int port,
				    LocalFrame frame,
				    Choice bfr,
				    bool interactive);
forwards void		interruptHandler(int sig);
static void		writeFrameGoal(LocalFrame frame, Code PC,
				       unsigned int flags);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
redoFrame() returns the latest skipped frame or NULL if  no  such  frame
exists.   This  is used to give the redo port of the goal skipped rather
than the redo port of some subgoal of this port.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static LocalFrame
redoFrame(LocalFrame fr, Code *PC)
{ while( fr && false(fr, FR_SKIPPED))
  { *PC = fr->programPointer;
    fr = parentFrame(fr);
  }

  return fr;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
canUnifyTermWithGoal() is used to check whether the given frame satisfies
the /search specification.  This function cannot use the `neat' interface
as the record is not in the proper format.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool
canUnifyTermWithGoal(LocalFrame fr)
{ find_data *find = LD->trace.find;

  switch(find->type)
  { case TRACE_FIND_ANY:
      succeed;
    case TRACE_FIND_NAME:
      return find->goal.name == fr->predicate->functor->name;
    case TRACE_FIND_TERM:
    { if ( find->goal.term.functor == fr->predicate->functor->functor )
      { fid_t cid = PL_open_foreign_frame();
	term_t t = PL_new_term_ref();
	Word a, b;
	int arity = fr->predicate->functor->arity;
	int rval = TRUE;
	term_t ex;

	copyRecordToGlobal(t, find->goal.term.term PASS_LD);
	a = valTermRef(t);
	deRef(a);
	a = argTermP(*a, 0);
	b = argFrameP(fr, 0);
	while( arity-- > 0 )
	{ if ( !can_unify(a++, b++, &ex) )
	  { rval = FALSE;
	    break;
	  }
	}

	PL_discard_foreign_frame(cid);
	return rval;
      }

      fail;
    }
    default:
      assert(0);
      fail;
  }
}


static const char *
portPrompt(int port)
{ switch(port)
  { case CALL_PORT:	 return " Call:  ";
    case REDO_PORT:	 return " Redo:  ";
    case FAIL_PORT:	 return " Fail:  ";
    case EXIT_PORT:	 return " Exit:  ";
    case UNIFY_PORT:	 return " Unify: ";
    case BREAK_PORT:	 return " Break: ";
    case EXCEPTION_PORT: return " Exception: ";
    case CUT_CALL_PORT:	 return " Cut call: ";
    case CUT_EXIT_PORT:	 return " Cut exit: ";
    default:		 return "";
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Toplevel  of  the  tracer.   This  function  is  called  from  the   WAM
interpreter.   It  can  take  care of most of the tracer actions itself,
except if the execution path is to  be  changed.   For  this  reason  it
returns to the WAM interpreter how to continue the execution:

    ACTION_CONTINUE:	Continue normal
    ACTION_FAIL:	Go to the fail port of this goal
    ACTION_RETRY:	Redo the current goal
    ACTION_IGNORE:	Go to the exit port of this goal
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#undef LD
#define LD LOCAL_LD

int
tracePort(LocalFrame frame, Choice bfr, int port, Code PC ARG_LD)
{ int action = ACTION_CONTINUE;
  Definition def = frame->predicate;
  LocalFrame fr;
  fid_t wake;

  if ( !bfr )
    bfr = LD->choicepoints;

  if ( (!isDebugFrame(frame) && !SYSTEM_MODE) || /* hidden */
       debugstatus.suspendTrace )	        /* called back */
    return ACTION_CONTINUE;

  if ( port == EXCEPTION_PORT )		/* do not trace abort */
  { Word p = valTermRef(LD->exception.pending);

    deRef(p);
    if ( *p == ATOM_aborted )
      return ACTION_CONTINUE;
  }
							/* trace/[1,2] */
  if ( true(def, TRACE_CALL|TRACE_REDO|TRACE_EXIT|TRACE_FAIL) )
  { int doit = FALSE;

    switch(port)
    { case CALL_PORT: doit = true(def, TRACE_CALL); break;
      case EXIT_PORT: doit = true(def, TRACE_EXIT); break;
      case FAIL_PORT: doit = true(def, TRACE_FAIL); break;
      case REDO_PORT: doit = true(def, TRACE_REDO); break;
    }

    if ( doit )
      writeFrameGoal(frame, PC, port|WFG_TRACE);
  }

  if ( port & BREAK_PORT )
    goto ok;				/* always do break-points */

  if ( !debugstatus.tracing &&
       (false(def, SPY_ME) || (port & CUT_PORT)) )
    return ACTION_CONTINUE;		/* not tracing and no spy-point */
  if ( debugstatus.skiplevel < levelFrame(frame) )
    return ACTION_CONTINUE;		/* skipped */
  if ( debugstatus.skiplevel == levelFrame(frame) &&
       (port & (REDO_PORT|CUT_PORT)) )
    return ACTION_CONTINUE;		/* redo or ! in skipped predicate */
  if ( false(def, TRACE_ME) )
    return ACTION_CONTINUE;		/* non-traced predicate */
  if ( (!(debugstatus.visible & port)) )
    return ACTION_CONTINUE;		/* wrong port */
  if ( (true(def, HIDE_CHILDS) && !SYSTEM_MODE) &&
       (port & (/*REDO_PORT|*/CUT_PORT)) )
    return ACTION_CONTINUE;		/* redo or ! in system predicates */
ok:

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Give a trace on the skipped goal for a redo.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  { Code pc2 = NULL;

    if ( port == REDO_PORT && debugstatus.skiplevel == VERY_DEEP &&
	 (fr = redoFrame(frame, &pc2)) != NULL )
    { debugstatus.skiplevel--;				   /* avoid a loop */
      switch( tracePort(fr, bfr, REDO_PORT, pc2 PASS_LD) )
      { case ACTION_CONTINUE:
	  if ( debugstatus.skiplevel < levelFrame(frame) )
	    return ACTION_CONTINUE;
	  break;
	case ACTION_RETRY:
	case ACTION_IGNORE:
	case ACTION_FAIL:
	  Sfputs("Action not yet implemented here\n", Sdout);
	  break;
      }
    }
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
We are in searching mode; should we actually give this port?
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  if ( LD->trace.find &&  LD->trace.find->searching )
  { DEBUG(2, Sdprintf("Searching\n"));

    if ( (port & LD->trace.find->port) && canUnifyTermWithGoal(frame) )
    { LD->trace.find->searching = FALSE; /* Got you */
    } else
    { return ACTION_CONTINUE;		/* Continue the search */
    }
  }


  wake = saveWakeup(PASS_LD1);
  blockGC(PASS_LD1);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Do the Prolog trace interception.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  action = traceInterception(frame, bfr, port, PC);
  if ( action >= 0 )
    goto out;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
All failed.  Things now are upto the normal Prolog tracer.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  action = ACTION_CONTINUE;

again:
  writeFrameGoal(frame, PC, port|WFG_TRACING);

  if (debugstatus.leashing & port)
  { char buf[LINESIZ];

    debugstatus.skiplevel = VERY_DEEP;
    debugstatus.tracing   = TRUE;

    Sfputs(" ? ", Sdout);
    Sflush(Sdout);
    if ( !truePrologFlag(PLFLAG_TTY_CONTROL) )
    { buf[0] = EOS;
      if ( !readLine(Sdin, Sdout, buf) )
      { Sfputs("EOF: exit\n", Sdout);
	exitFromDebugger(0);
      }
    } else
    { int c = getSingleChar(Sdin, FALSE);

      if ( c == EOF )
      { Sfputs("EOF: exit\n", Sdout);
	exitFromDebugger(0);
      }
      buf[0] = c;
      buf[1] = EOS;
      if ( isDigit(buf[0]) || buf[0] == '/' )
      { Sfputs(buf, Sdout);
	readLine(Sdin, Sdout, buf);
      }
    }
    action = traceAction(buf, port, frame, bfr,
			 truePrologFlag(PLFLAG_TTY_CONTROL));
    if ( action == ACTION_AGAIN )
      goto again;
  } else
    Sputcode('\n', Sdout);

out:
  unblockGC(PASS_LD1);
  restoreWakeup(wake PASS_LD);
  if ( action == ACTION_ABORT )
    pl_abort(ABORT_NORMAL);

  return action;
}


#undef LD
#define LD GLOBAL_LD


static int
setupFind(char *buf)
{ char *s;
  int port = 0;

  for(s = buf; *s && isBlank(*s); s++)	/* Skip blanks */
    ;
  if ( *s == EOS )			/* No specification: repeat */
  { if ( !LD->trace.find || !LD->trace.find->port )
    { Sfputs("[No previous search]\n", Sdout);
      fail;
    }
    LD->trace.find->searching = TRUE;
    succeed;
  }
  for( ; *s && !isBlank(*s); s++ )	/* Parse the port specification */
  { switch( *s )
    { case 'c':	port |= CALL_PORT;  continue;
      case 'e':	port |= EXIT_PORT;  continue;
      case 'r':	port |= REDO_PORT;  continue;
      case 'f':	port |= FAIL_PORT;  continue;
      case 'u':	port |= UNIFY_PORT; continue;
      case 'a':	port |= CALL_PORT|REDO_PORT|FAIL_PORT|EXIT_PORT|UNIFY_PORT;
				    continue;
      default:  Sfputs("[Illegal port specification]\n", Sdout);
		fail;
    }
  }
  for( ; *s && isBlank(*s); s++)	/* Skip blanks */
    ;

  if ( *s == EOS )			/* Nothing is a variable */
  { s = buf;
    buf[0] = '_',
    buf[1] = EOS;
  }

  { fid_t cid = PL_open_foreign_frame();
    term_t t = PL_new_term_ref();
    FindData find;

    if ( !(find = LD->trace.find) )
      find = LD->trace.find = allocHeap(sizeof(find_data));

    if ( !PL_chars_to_term(s, t) )
    { PL_discard_foreign_frame(cid);
      fail;
    }

    if ( find->type == TRACE_FIND_TERM && find->goal.term.term )
      freeRecord(find->goal.term.term);

    if ( PL_is_variable(t) )
    { find->type = TRACE_FIND_ANY;
    } else if ( PL_get_atom(t, &find->goal.name) )
    { find->type = TRACE_FIND_NAME;
    } else if ( PL_get_functor(t, &find->goal.term.functor) )
    { find->type = TRACE_FIND_TERM;
      find->goal.term.term = compileTermToHeap(t, 0);
    } else
    { Sfputs("[Illegal goal specification]\n", Sdout);
      fail;
    }

    find->port      = port;
    find->searching = TRUE;

    DEBUG(2,
	  Sdprintf("setup ok, port = 0x%x, goal = ", port);
	  PL_write_term(Serror, t, 1200, 0);
	  Sdprintf("\n") );

    PL_discard_foreign_frame(cid);
  }

  succeed;
}


static void
setPrintOptions(word t)
{ fid_t fid        = PL_open_foreign_frame();
  term_t av        = PL_new_term_ref();
  predicate_t pred = PL_predicate("$set_debugger_print_options", 1, "system");

  _PL_put_atomic(av, t);
  PL_call_predicate(NULL, PL_Q_NODEBUG, pred, av);

  PL_discard_foreign_frame(fid);
}


static int
traceAction(char *cmd, int port, LocalFrame frame, Choice bfr, bool interactive)
{ int num_arg;				/* numeric argument */
  char *s;

#define FeedBack(msg)	{ if (interactive) { if (cmd[1] != EOS) \
					       Sputcode('\n', Sdout); \
					     else \
					       Sfputs(msg, Sdout); } }
#define Warn(msg)	{ if (interactive) \
			    Sfputs(msg, Sdout); \
			  else \
			    warning(msg); \
			}
#define Default		(-1)

  for(s=cmd; *s && isBlank(*s); s++)
    ;
  if ( isDigit(*s) )
  { num_arg = strtol(s, &s, 10);

    while(isBlank(*s))
      s++;
  } else
    num_arg = Default;

  switch( *s )
  { case 'a':	FeedBack("abort\n");
    		return ACTION_ABORT;
    case 'b':	FeedBack("break\n");
		pl_break();
		return ACTION_AGAIN;
    case '/': 	FeedBack("/");
    		Sflush(Suser_output);
    		if ( setupFind(&s[1]) )
		{ clear(frame, FR_SKIPPED);
		  return ACTION_CONTINUE;
		}
		return ACTION_AGAIN;
    case '.':   if ( LD->trace.find &&
		     LD->trace.find->type != TRACE_FIND_NONE )
      	        { FeedBack("repeat search\n");
		  LD->trace.find->searching = TRUE;
		  clear(frame, FR_SKIPPED);
		  return ACTION_CONTINUE;
		} else
		{ Warn("No previous search\n");
		}
		return ACTION_AGAIN;
    case EOS:
    case ' ':
    case '\n':
    case '\r':
    case 'c':	FeedBack("creep\n");
		clear(frame, FR_SKIPPED);
		if ( !(port & EXIT_PORT) )
		  clear(frame, FR_SKIPPED);
		return ACTION_CONTINUE;
    case '\04':
    case EOF:	FeedBack("EOF: ");
    case 'e':	FeedBack("exit\n");
		exitFromDebugger(0);
    case 'f':	FeedBack("fail\n");
		return ACTION_FAIL;
    case 'i':	if (port & (CALL_PORT|REDO_PORT|FAIL_PORT))
		{ FeedBack("ignore\n");
		  return ACTION_IGNORE;
		} else
		  Warn("Can't ignore goal at this port\n");
		return ACTION_CONTINUE;
    case 'r':	if (port & (REDO_PORT|FAIL_PORT|EXIT_PORT|EXCEPTION_PORT))
		{ FeedBack("retry\n[retry]\n");
		  return ACTION_RETRY;
		} else
		  Warn("Can't retry at this port\n");
		return ACTION_CONTINUE;
    case 's':	FeedBack("skip\n");
		set(frame, FR_SKIPPED);
		debugstatus.skiplevel = levelFrame(frame);
		return ACTION_CONTINUE;
    case 'u':	FeedBack("up\n");
		debugstatus.skiplevel = levelFrame(frame) - 1;
		return ACTION_CONTINUE;
    case 'd':   FeedBack("depth\n");
                setPrintOptions(consInt(num_arg));
		return ACTION_AGAIN;
    case 'w':   FeedBack("write\n");
                setPrintOptions(ATOM_write);
		return ACTION_AGAIN;
    case 'p':   FeedBack("print\n");
		setPrintOptions(ATOM_print);
		return ACTION_AGAIN;
    case 'l':	FeedBack("leap\n");
    		tracemode(FALSE, NULL);
		return ACTION_CONTINUE;
    case 'n':	FeedBack("no debug\n");
		tracemode(FALSE, NULL);
    		debugmode(DBG_OFF, NULL);
		return ACTION_CONTINUE;
    case 'g':	FeedBack("goals\n");
		backTrace(frame, num_arg == Default ? 5 : num_arg);
		return ACTION_AGAIN;
    case 'A':	FeedBack("alternatives\n");
		alternatives(bfr);
		return ACTION_AGAIN;
    case 'C':	debugstatus.showContext = 1 - debugstatus.showContext;
		if ( debugstatus.showContext == TRUE )
		{ FeedBack("Show context\n");
		} else
		{ FeedBack("No show context\n");
		}
		return ACTION_AGAIN;
    case 'm':	FeedBack("Exception details");
    	        if ( port & EXCEPTION_PORT )
		{ exceptionDetails();
		} else
		   Warn("No exception\n");
		return ACTION_AGAIN;
    case 'L':	FeedBack("Listing");
		listGoal(frame);
		return ACTION_AGAIN;
    case '+':	FeedBack("spy\n");
		set(frame->predicate, SPY_ME);
		return ACTION_AGAIN;
    case '-':	FeedBack("no spy\n");
		clear(frame->predicate, SPY_ME);
		return ACTION_AGAIN;
    case '?':
    case 'h':	helpTrace();
		return ACTION_AGAIN;
    case 'D':   GD->debug_level = num_arg;
		FeedBack("Debug level\n");
		return ACTION_AGAIN;
    default:	Warn("Unknown option (h for help)\n");
		return ACTION_AGAIN;
  }
}

static void
helpTrace(void)
{ Sfputs("Options:\n"
	 "+:                  spy        -:              no spy\n"
	 "/c|e|r|f|u|a goal:  find       .:              repeat find\n"
	 "a:                  abort      A:              alternatives\n"
	 "b:                  break      c (ret, space): creep\n"
	 "[depth] d:          depth      e:              exit\n"
	 "f:                  fail       [ndepth] g:     goals (backtrace)\n"
	 "h (?):              help       i:              ignore\n"
	 "l:                  leap       L:              listing\n"
	 "n:                  no debug   p:              print\n"
	 "r:                  retry      s:              skip\n"
	 "u:                  up         w:              write\n"
	 "m:		      exception details\n"
	 "C:                  toggle show context\n"
#if O_DEBUG
	 "[level] D:	      set system debug level\n"
#endif
	 "", Sdout);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Write goal of stack frame.  First a term representing the  goal  of  the
frame  is  constructed.  Trail and global stack are marked and undone to
avoid garbage on the global stack.

Trick, trick, O big trick ... In order to print the  goal  we  create  a
term  for  it  (otherwise  we  would  have to write a special version of
write/1, etc.  for stack frames).  A small problem arises: if the  frame
holds a variable we will make a reference to the new term, thus printing
the wrong variable: variables sharing in a clause does not seem to share
any  longer  in  the  tracer  (Anjo  Anjewierden discovered this ackward
feature of the tracer).  The solution is simple: we make  the  reference
pointer  the other way around.  Normally references should never go from
the global to the local stack as the local stack frame  might  cease  to
exists  before  the  global frame.  In this case this does not matter as
the local stack frame definitely survives the tracer (measuring does not
always mean influencing in computer science :-).

Unfortunately the garbage collector doesn't like   this. It violates the
assumptions  in  offset_cell()  where  a    local  stack  reference  has
TAG_REFERENCE and storage STG_LOCAL. It   also violates assumptions made
in mark_variable(). Hence we can only play   this trick if GC is blocked
and the data is destroyed using PL_discard_foreign_frame().

For the above reason, the code  below uses low-level manipulation rather
than normal unification, etc.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
put_frame_goal(term_t goal, LocalFrame frame)
{ Definition def = frame->predicate;
  int argc = def->functor->arity;
  Word argv = argFrameP(frame, 0);

  PL_unify_functor(goal, def->functor->functor);
  if ( argc > 0 )
  { Word argp = valTermRef(goal);
    int i;

    deRef(argp);
    argp = argTermP(*argp, 0);

    for(i=0; i<argc; i++)
    { Word a;

      deRef2(argv+i, a);
      *argp++ = (needsRef(*a) ? makeRef(a) : *a);
    }
  }
  if ( def->module != MODULE_user &&
       (false(def->module, SYSTEM) || SYSTEM_MODE))
  { term_t a = PL_new_term_ref();

    PL_put_atom(a, def->module->name);
    PL_cons_functor(goal, FUNCTOR_colon2, a, goal);
  }
}


typedef struct
{ unsigned int flags;			/* flag mask */
  atom_t name;				/* name */
} portname;

static const portname portnames[] =
{ { WFG_BACKTRACE,  ATOM_backtrace },
  { WFG_CHOICE,     ATOM_choice },
  { CALL_PORT,	    ATOM_call },
  { EXIT_PORT,	    ATOM_exit },
  { FAIL_PORT,	    ATOM_fail },
  { REDO_PORT,	    ATOM_redo },
  { UNIFY_PORT,	    ATOM_unify },
  { BREAK_PORT,	    ATOM_break },
  { CUT_CALL_PORT,  ATOM_cut_call },
  { CUT_EXIT_PORT,  ATOM_cut_exit },
  { EXCEPTION_PORT, ATOM_exception },
  { 0,		    NULL_ATOM }
};


static void
writeFrameGoal(LocalFrame frame, Code PC, unsigned int flags)
{ fid_t wake = saveWakeup(PASS_LD1);
  fid_t cid = PL_open_foreign_frame();
  Definition def = frame->predicate;

  blockGC(PASS_LD1);

  if ( gc_status.active )
  { Sfprintf(Serror, " (%d): %s\n",
	     levelFrame(frame), predicateName(frame->predicate));
  } else if ( !GD->bootsession && GD->initialised && GD->debug_level == 0 )
  { term_t fr   = PL_new_term_ref();
    term_t port = PL_new_term_ref();
    term_t pc   = PL_new_term_ref();
    const portname *pn = portnames;

    if ( true(def, FOREIGN) )
      PL_put_atom(pc, ATOM_foreign);
    else if ( PC && frame->clause )
      PL_put_intptr(pc, PC-frame->clause->clause->codes);
    else
      PL_put_nil(pc);

    PL_put_frame(fr, frame);

    for(; pn->flags; pn++)
    { if ( flags & pn->flags )
      { PL_put_atom(port, pn->name);
	break;
      }
    }
    if ( flags & WFG_TRACE )
      PL_cons_functor(port, FUNCTOR_trace1, port);

    printMessage(ATOM_debug,
		 PL_FUNCTOR, FUNCTOR_frame3,
		   PL_TERM, fr,
		   PL_TERM, port,
		   PL_TERM, pc);
  } else
  { debug_type debugSave = debugstatus.debugging;
    term_t goal    = PL_new_term_ref();
    term_t options = PL_new_term_ref();
    term_t tmp     = PL_new_term_ref();
    char msg[3];
    const char *pp = portPrompt(flags&PORT_MASK);
    struct foreign_context ctx;

    put_frame_goal(goal, frame);
    debugstatus.debugging = DBG_OFF;
    PL_put_atom(tmp, ATOM_debugger_print_options);
    ctx.context = 0;
    ctx.control = FRG_FIRST_CALL;
    ctx.engine  = LD;
    if ( !pl_prolog_flag(tmp, options, &ctx) )
      PL_put_nil(options);
    PL_put_atom(tmp, ATOM_user_output);

    msg[0] = true(def, P_TRANSPARENT) ? '^' : ' ';
    msg[1] = (flags&WFG_TRACE) ? 'T' : true(def, SPY_ME) ? '*' : ' ';
    msg[2] = EOS;

    Sfprintf(Sdout, "%s%s(%d) ", msg, pp, levelFrame(frame));
    if ( debugstatus.showContext )
      Sfprintf(Sdout, "[%s] ", stringAtom(contextModule(frame)->name));
#ifdef O_LIMIT_DEPTH
    if ( levelFrame(frame) > depth_limit )
      Sfprintf(Sdout, "[deth-limit exceeded] ");
#endif

    pl_write_term3(tmp, goal, options);
    if ( flags & (WFG_BACKTRACE|WFG_CHOICE) )
      Sfprintf(Sdout, "\n");

    debugstatus.debugging = debugSave;
  }

  unblockGC(PASS_LD1);

  PL_discard_foreign_frame(cid);
  restoreWakeup(wake PASS_LD);
}

/*  Write those frames on the stack that have alternatives left.

 ** Tue May 10 23:23:11 1988  jan@swivax.UUCP (Jan Wielemaker)  */

static void
alternatives(Choice ch)
{ for(; ch; ch = ch->parent)
  { if ( (isDebugFrame(ch->frame) || SYSTEM_MODE) )
      writeFrameGoal(ch->frame, NULL, WFG_CHOICE);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
messageToString() is a  wrapper   around  $messages:message_to_string/2,
translating a message-term as used for exceptions into a C-string.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static char *
messageToString(term_t msg)
{ fid_t fid = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(2);
  predicate_t pred = PL_predicate("message_to_string", 2, "$messages");
  char *s;

  PL_put_term(av+0, msg);
  PL_call_predicate(MODULE_system, PL_Q_NODEBUG, pred, av);
  PL_get_chars(av+1, &s, CVT_ALL|BUF_RING);
  PL_discard_foreign_frame(fid);

  return s;
}


static void
exceptionDetails()
{ term_t except = LD->exception.pending;
  fid_t cid = PL_open_foreign_frame();

  Sflush(Suser_output);			/* make sure to stay `in sync' */
  Sfputs("\n\tException term: ", Sdout);
  PL_write_term(Sdout, except, 1200, PL_WRT_QUOTED);
  Sfprintf(Sdout, "\n\t       Message: %s\n", messageToString(except));

  PL_discard_foreign_frame(cid);
}


static void
listGoal(LocalFrame frame)
{ fid_t cid = PL_open_foreign_frame();
  term_t goal = PL_new_term_ref();
  predicate_t pred = PL_predicate("$prolog_list_goal", 1, "system");
  IOSTREAM *old = Scurout;

  Scurout = Sdout;
  blockGC(PASS_LD1);
  put_frame_goal(goal, frame);
  PL_call_predicate(MODULE_system, PL_Q_NODEBUG, pred, goal);
  unblockGC(PASS_LD1);
  Scurout = old;

  PL_discard_foreign_frame(cid);
}


void
backTrace(LocalFrame frame, int depth)
{ LocalFrame same_proc_frame = NULL;
  Definition def = NULL;
  int same_proc = 0;
  int alien = FALSE;
  Code PC = NULL;

  if ( frame == NULL )
     frame = environment_frame;

  for(; depth > 0 && frame;
        alien = (frame->parent == NULL),
        PC = frame->programPointer,
        frame = parentFrame(frame))
  { if ( alien )
      Sfputs("    <Alien goal>\n", Sdout);

    if ( frame->predicate == def )
    { if ( ++same_proc >= 10 )
      { if ( same_proc == 10 )
	  Sfputs("    ...\n    ...\n", Sdout);
	same_proc_frame = frame;
	continue;
      }
    } else
    { if ( same_proc_frame != NULL )
      { if ( isDebugFrame(same_proc_frame) || SYSTEM_MODE )
        { writeFrameGoal(same_proc_frame, PC, WFG_BACKTRACE);
	  depth--;
	}
	same_proc_frame = NULL;
	same_proc = 0;
      }
      def = frame->predicate;
    }

    if ( isDebugFrame(frame) || SYSTEM_MODE)
    { writeFrameGoal(frame, PC, WFG_BACKTRACE);
      depth--;
    }
  }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Trace interception mechanism.  Whenever the tracer wants to perform some
action   it   will   first   call   the    users'    Prolog    predicate
prolog_trace_interception/4, allowing the user to define his/her action.
If  this procedure succeeds the tracer assumes the trace action has been
done and returns, otherwise the  default  C-defined  trace  actions  are
performed.

This predicate is supposed to return one of the following atoms:

	continue			simply continue (creep)
	fail				fail this goal
	retry				retry this goal
	ignore				pretend this call succeeded
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
traceInterception(LocalFrame frame, Choice bfr, int port, Code PC)
{ int rval = -1;			/* Default C-action */
  predicate_t proc;

  proc = _PL_predicate("prolog_trace_interception", 4, "user",
		       &GD->procedures.prolog_trace_interception4);
  if ( !getProcDefinition(proc)->definition.clauses )
    return rval;

  if ( !GD->bootsession && GD->debug_level == 0 )
  { fid_t cid = PL_open_foreign_frame();
    qid_t qid;
    term_t argv = PL_new_term_refs(4);
    term_t rarg = argv+3;
    atom_t portname = NULL_ATOM;
    functor_t portfunc = 0;
    int nodebug = FALSE;
    term_t ex;

    switch(port)
    { case CALL_PORT:	   portname = ATOM_call;         break;
      case REDO_PORT:	   portname = ATOM_redo;         break;
      case EXIT_PORT:	   portname = ATOM_exit;         break;
      case FAIL_PORT:	   portname = ATOM_fail;         break;
      case UNIFY_PORT:	   portname = ATOM_unify;	 break;
      case EXCEPTION_PORT:
        PL_unify_term(argv,
		      PL_FUNCTOR, FUNCTOR_exception1,
		        PL_TERM, LD->exception.pending);
	break;
      case BREAK_PORT:     portfunc = FUNCTOR_break1;	 break;
      case CUT_CALL_PORT:  portfunc = FUNCTOR_cut_call1; break;
      case CUT_EXIT_PORT:  portfunc = FUNCTOR_cut_exit1; break;
      default:
	assert(0);
        return rval;
    }

    if ( portname )
      PL_put_atom(argv, portname);
    else if ( portfunc )
    { int pcn;

      if ( PC && false(frame->predicate, FOREIGN) && frame->clause )
	pcn = (int)(PC - frame->clause->clause->codes);
      else
	pcn = 0;

      PL_unify_term(argv,
		    PL_FUNCTOR, portfunc,
		    PL_INT, pcn);
    }

    PL_put_frame(argv+1, frame);
    PL_put_choice(argv+2, bfr);
    PL_put_variable(rarg);

    if ( exception_term )
      ex = PL_copy_term_ref(exception_term);
    else
      ex = 0;
    qid = PL_open_query(MODULE_user, PL_Q_NODEBUG, proc, argv);
    if ( PL_next_solution(qid) )
    { atom_t a;

      if ( PL_get_atom(rarg, &a) )
      { if ( a == ATOM_continue )
	  rval = ACTION_CONTINUE;
	else if ( a == ATOM_nodebug )
	{ rval = ACTION_CONTINUE;
	  nodebug = TRUE;
	} else if ( a == ATOM_fail )
	  rval = ACTION_FAIL;
	else if ( a == ATOM_retry )
	  rval = ACTION_RETRY;
	else if ( a == ATOM_ignore )
	  rval = ACTION_IGNORE;
	else if ( a == ATOM_abort )
	  rval = ACTION_ABORT;
	else
	  PL_warning("Unknown trace action: %s", stringAtom(a));
      } else if ( PL_is_functor(rarg, FUNCTOR_retry1) )
      { LocalFrame fr;
	term_t arg = PL_new_term_ref();

	if ( PL_get_arg(1, rarg, arg) && PL_get_frame(arg, &fr) )
	{ debugstatus.retryFrame = fr;
	  rval = ACTION_RETRY;
	} else
	  PL_warning("prolog_trace_interception/3: bad argument to retry/1");
      }
    }
    PL_close_query(qid);
    if ( ex )
    { PL_put_term(exception_bin, ex);
      exception_term = exception_bin;
    }
    PL_discard_foreign_frame(cid);

    if ( nodebug )
    { tracemode(FALSE, NULL);
      debugmode(DBG_OFF, NULL);
    }
  }

  return rval;
}

#endif /*O_DEBUGGER*/

#ifndef offset
#define offset(s, f) ((size_t)(&((struct s *)NULL)->f))
#endif

static QueryFrame
findQuery(LocalFrame fr)
{ while(fr && fr->parent)
    fr = fr->parent;

  if ( fr )
    return queryOfFrame(fr);
  return NULL;
}


static bool
hasAlternativesFrame(LocalFrame frame)
{ QueryFrame qf;
  LocalFrame fr = environment_frame;
  Choice ch = LD->choicepoints;

  for(;;)
  { for( ; ch; ch = ch->parent )
    { if ( (void *)ch < (void *)frame )
	return FALSE;

      if ( ch->frame == frame )
      { switch( ch->type )
	{ case CHP_CLAUSE:
	  case CHP_JUMP:
	    return TRUE;
	  case CHP_TOP:			/* no default to get warning */
	  case CHP_CATCH:
	  case CHP_DEBUG:
	    continue;
	}
      }
    }
    if ( (qf = findQuery(fr)) )
    { fr = qf->saved_environment;
      ch = qf->saved_bfr;
    } else
      return FALSE;
  }
}


#ifdef O_DEBUG
static intptr_t
loffset(void *p)
{ if ( p == NULL )
    return 0;

  assert((intptr_t)p % sizeof(word) == 0);
  return (Word)p-(Word)lBase;
}

extern char *chp_chars(Choice ch);
#endif

static LocalFrame
alternativeFrame(LocalFrame frame)
{ QueryFrame qf;
  LocalFrame fr = environment_frame;
  Choice ch = LD->choicepoints;

  DEBUG(3, Sdprintf("Looking for choice of #%d\n", loffset(frame)));

  for(;;)
  { for( ; ch; ch = ch->parent )
    { if ( (void *)ch < (void *)frame )
	return NULL;

      if ( ch->frame == frame )
      { DEBUG(3, Sdprintf("First: %s\n", chp_chars(ch)));

	for(ch = ch->parent; ch; ch = ch->parent )
	{ if ( ch->frame == frame )
	  { DEBUG(3, Sdprintf("\tSkipped: %s\n", chp_chars(ch)));
	    continue;
	  }

	  switch( ch->type )
	  { case CHP_CLAUSE:
	    case CHP_JUMP:
	      DEBUG(3, Sdprintf("\tReturning: %s\n", chp_chars(ch)));
	      return ch->frame;
	    default:
	      break;
	  }
	}

        return NULL;
      }
    }

    if ( (qf = findQuery(fr)) )
    { fr = qf->saved_environment;
      ch = qf->saved_bfr;
    } else
      return NULL;
  }
}


void
resetTracer(void)
{
#ifdef O_INTERRUPT
  if ( truePrologFlag(PLFLAG_SIGNALS) )
    PL_signal(SIGINT, interruptHandler);
#endif

  debugstatus.tracing      = FALSE;
  debugstatus.debugging    = DBG_OFF;
  debugstatus.suspendTrace = 0;
  debugstatus.skiplevel    = 0;
  debugstatus.retryFrame   = NULL;

  setPrologFlagMask(PLFLAG_LASTCALL);
}


#ifdef O_INTERRUPT

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Handling  interrupts.   We  know  we  are  not  in  critical  code  (see
startCritical()  and endCritical(), so the heap is consistent.  The only
problem can be that we are currently writing the arguments of  the  next
goal  above  the  local  stack  top  pointer.  To avoid problems we just
increment the top pointer to point above the furthest argument.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
helpInterrupt(void)
{ Sfputs("Options:\n"
        "a:                 abort      b:                 break\n"
        "c:                 continue   e:                 exit\n"
#ifdef O_DEBUGGER
        "g:                 goals      t:                 trace\n"
#endif
        "h (?):             help\n", Sdout);
}

static void
interruptHandler(int sig)
{ int c;
  int safe;

  if ( !GD->initialised )
  { Sfprintf(Serror, "Interrupt during startup. Cannot continue\n");
    PL_halt(1);
  }

#ifdef O_PLMT
  if ( !LD )				/* we can't handle this; main thread */
  { PL_thread_raise(1, sig);		/* should try to do this */
    return;
  }

  if ( LD->exit_requested )
  { term_t rval = PL_new_term_ref();
    PL_put_atom(rval, ATOM_true);
    pl_thread_exit(rval);
    assert(0);				/* should not return */
  }
#endif

#if __unix__				/* actually, asynchronous signal handling */
  if ( !LD->sync_signal )
  { if ( PL_pending(sig) )
    { PL_clearsig(sig);
      safe = FALSE;
    } else
    { DEBUG(1, Sdprintf("Reposting as synchronous\n"));
      PL_raise(sig);
      return;
    }
  } else
#endif					/* no async signals; always safe */
  { safe = TRUE;
  }

  Sreset();
again:
  if ( safe )
  { printMessage(ATOM_debug, PL_FUNCTOR, FUNCTOR_interrupt1, PL_ATOM, ATOM_begin);
  } else
  { Sfprintf(Sdout, "\n%sAction (h for help) ? ", safe ? "" : "[forced] ");
    Sflush(Sdout);
  }
  ResetTty();                           /* clear pending input -- atoenne -- */
  c = getSingleChar(Sdin, FALSE);

  switch(c)
  { case 'a':	Sfputs("abort\n", Sdout);
		unblockSignal(sig);
    		pl_abort(ABORT_NORMAL);
		break;
    case 'b':	Sfputs("break\n", Sdout);
		unblockSignal(sig);	/* into pl_break() itself */
		pl_break();
		goto again;
    case 'c':	if ( safe )
		{ printMessage(ATOM_debug, PL_FUNCTOR, FUNCTOR_interrupt1, PL_ATOM, ATOM_end);
		} else
		{ Sfputs("continue\n", Sdout);
		}
		break;
    case 04:
    case EOF:	Sfputs("EOF: ", Sdout);
    case 'e':	Sfputs("exit\n", Sdout);
		exitFromDebugger(0);
		break;
#ifdef O_DEBUGGER
    case 'g':	Sfputs("goals\n", Sdout);
		backTrace(environment_frame, 5);
		goto again;
#endif /*O_DEBUGGER*/
    case 'h':
    case '?':	helpInterrupt();
		goto again;
#ifdef O_DEBUGGER
    case 't':	Sfputs("trace\n", Sdout);
		if ( safe )
		  printMessage(ATOM_debug, PL_FUNCTOR, FUNCTOR_interrupt1, PL_ATOM, ATOM_trace);
		pl_trace();
		break;
#endif /*O_DEBUGGER*/
    default:	Sfputs("Unknown option (h for help)\n", Sdout);
		goto again;
  }
}

#endif /*O_INTERRUPT*/


void
PL_interrupt(int sig)
{
#ifdef O_INTERRUPT
   interruptHandler(sig);
#endif
}


void
initTracer(void)
{ debugstatus.visible      =
  debugstatus.leashing     = CALL_PORT|FAIL_PORT|REDO_PORT|EXIT_PORT|
			     BREAK_PORT|EXCEPTION_PORT;
  debugstatus.showContext  = FALSE;

  resetTracer();
}

		/********************************
		*       PROLOG PREDICATES       *
		*********************************/

#if O_DEBUGGER

int
tracemode(int doit, int *old)
{ if ( doit )
  { debugmode(DBG_ON, NULL);
    doit = TRUE;
  }

  if ( old )
    *old = debugstatus.tracing;

  if ( debugstatus.tracing != doit )
  { debugstatus.tracing = doit;
    printMessage(ATOM_silent,
		 PL_FUNCTOR_CHARS, "trace_mode", 1,
		   PL_ATOM, doit ? ATOM_on : ATOM_off);
  }
  if ( doit )				/* make sure trace works inside skip */
  { debugstatus.skiplevel = VERY_DEEP;
    if ( LD->trace.find )
      LD->trace.find->searching = FALSE;
  }

  succeed;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
debugmode(debug_type new, debug_type *old)

Set the current debug mode. If DBG_ALL,  debugging in switched on in all
queries. This behaviour is intended to allow   using  spy and debug from
PceEmacs that runs its Prolog work in non-debug mode.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
debugmode(debug_type doit, debug_type *old)
{ if ( old )
    *old = debugstatus.debugging;

  if ( debugstatus.debugging != doit )
  { if ( doit )
    { debugstatus.skiplevel = VERY_DEEP;
      clearPrologFlagMask(PLFLAG_LASTCALL);
      if ( doit == DBG_ALL )
      { LocalFrame fr = environment_frame;

	while( fr )
	{ if ( fr->parent )
	    fr = fr->parent;
	  else
	  { QueryFrame qf = queryOfFrame(fr);
	    qf->debugSave = DBG_ON;
	    fr = qf->saved_environment;
	  }
	}
	doit = DBG_ON;
      }
    } else
    { setPrologFlagMask(PLFLAG_LASTCALL);
    }
    debugstatus.debugging = doit;
    updateAlerted(LD);
    printMessage(ATOM_silent,
		 PL_FUNCTOR_CHARS, "debug_mode", 1,
		   PL_ATOM, doit ? ATOM_on : ATOM_off);
  }

  succeed;
}

#else /*O_DEBUGGER*/

int
tracemode(int doit, int *old)
{ succeed;
}

int
debugmode(debug_type doit, debug_type *old)
{ succeed;
}

#endif

word
pl_trace()
{ return tracemode(TRUE, NULL);
}

word
pl_notrace()
{ return tracemode(FALSE, NULL);
}

word
pl_tracing()
{ return debugstatus.tracing;
}

word
pl_skip_level(term_t old, term_t new)
{ atom_t a;
  long sl;

  if ( debugstatus.skiplevel == VERY_DEEP )
  { TRY(PL_unify_atom(old, ATOM_very_deep));
  } else
  { TRY(PL_unify_integer(old, debugstatus.skiplevel));
  }

  if ( PL_get_long(new, &sl) )
  { debugstatus.skiplevel = sl;
    succeed;
  }
  if ( PL_get_atom(new, &a) && a == ATOM_very_deep)
  { debugstatus.skiplevel = VERY_DEEP;
    succeed;
  }

  fail;
}

word
pl_spy(term_t p)
{ Procedure proc;

  if ( get_procedure(p, &proc, 0, GP_FIND) )
  { Definition def = getProcDefinition(proc);

    if ( false(def, SPY_ME) )
    { LOCKDEF(def);
      set(def, SPY_ME);
      UNLOCKDEF(def);
      printMessage(ATOM_informational,
		   PL_FUNCTOR_CHARS, "spy", 1,
		     PL_TERM, p);
    }
    debugmode(DBG_ALL, NULL);
    succeed;
  }

  fail;
}

word
pl_nospy(term_t p)
{ Procedure proc;

  if ( get_procedure(p, &proc, 0, GP_FIND|GP_EXISTENCE_ERROR) )
  { Definition def = getProcDefinition(proc);

    if ( true(def, SPY_ME) )
    { LOCKDEF(def);
      clear(def, SPY_ME);
      UNLOCKDEF(def);
      printMessage(ATOM_informational,
		   PL_FUNCTOR_CHARS, "nospy", 1,
		     PL_TERM, p);
    }
    succeed;
  }

  fail;
}

word
pl_leash(term_t old, term_t new)
{ return setInteger(&debugstatus.leashing, old, new);
}

word
pl_visible(term_t old, term_t new)
{ return setInteger(&debugstatus.visible, old, new);
}


word
pl_debuglevel(term_t old, term_t new)
{ return setInteger(&GD->debug_level, old, new);
}


word
pl_prolog_current_frame(term_t frame)
{ LocalFrame fr = environment_frame;

  if ( fr->predicate->definition.function == pl_prolog_current_frame )
    fr = parentFrame(fr);		/* thats me! */

  return PL_unify_frame(frame, fr);
}


word
pl_prolog_frame_attribute(term_t frame, term_t what,
			  term_t value)
{ LocalFrame fr;
  atom_t key;
  int arity;
  term_t result = PL_new_term_ref();

  if ( !PL_get_frame(frame, &fr) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_frame_reference, frame);
  if ( !fr )
    fail;				/* frame == 'none' */
  if ( !PL_get_name_arity(what, &key, &arity) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, what);

  set(fr, FR_WATCHED);			/* explicit call to do this? */

  if ( key == ATOM_argument && arity == 1 )
  { term_t arg = PL_new_term_ref();
    int argn;
    Word p = valTermRef(value);

    if ( !PL_get_arg_ex(1, what, arg) || !PL_get_integer_ex(arg, &argn) )
      fail;
    if ( argn < 1 )
      return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_natural, arg);

    if ( true(fr->predicate, FOREIGN) || !fr->clause )
    { if ( argn > (int)fr->predicate->functor->arity )
	fail;
    } else
    { if ( argn > fr->clause->clause->prolog_vars )
	fail;
    }

#ifdef O_DEBUGLOCAL			/* see pl-wam.c */
    assert( *argFrameP(fr, argn-1) != (word)(((char*)ATOM_nil) + 1) );
    checkData(argFrameP(fr, argn-1));
#endif

   deRef(p);
   if ( isVar(*p) )
   { *p = makeRef(argFrameP(fr, argn-1));
     Trail(p);
     succeed;
   }

   fail;
  }

  if ( arity != 0 )
  { unknown_key:
    return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_frame_attribute, what);
  }

  if (        key == ATOM_level)
  { PL_put_integer(result, levelFrame(fr));
  } else if (key == ATOM_has_alternatives)
  { PL_put_atom(result, hasAlternativesFrame(fr) ? ATOM_true : ATOM_false);
  } else if (key == ATOM_alternative)
  { LocalFrame alt;

    if ( (alt = alternativeFrame(fr)) )
      PL_put_frame(result, alt);
    else
      fail;
  } else if (key == ATOM_parent)
  { LocalFrame parent;

    if ( fr->parent )
      clearUninitialisedVarsFrame(fr->parent, fr->programPointer);

    if ( (parent = parentFrame(fr)) )
      PL_put_frame(result, parent);
    else
      fail;
  } else if (key == ATOM_top)
  { PL_put_atom(result, fr->parent ? ATOM_false : ATOM_true);
  } else if (key == ATOM_context_module)
  { PL_put_atom(result, contextModule(fr)->name);
  } else if (key == ATOM_clause)
  { if ( false(fr->predicate, FOREIGN) &&
	 fr->clause &&
	 fr->predicate != PROCEDURE_dc_call_prolog->definition )
      PL_put_pointer(result, fr->clause->clause);
    else
      fail;
  } else if (key == ATOM_goal)
  { int arity, n;
    term_t arg = PL_new_term_ref();

    if (fr->predicate->module != MODULE_user)
    { PL_put_functor(result, FUNCTOR_colon2);
      PL_get_arg(1, result, arg);
      PL_unify_atom(arg, fr->predicate->module->name);
      PL_get_arg(2, result, arg);
    } else
      PL_put_term(arg, result);

    if ((arity = fr->predicate->functor->arity) == 0)
    { PL_unify_atom(arg, fr->predicate->functor->name);
    } else			/* see put_frame_goal(); must be merged */
    { Word argv = argFrameP(fr, 0);
      Word argp;

      PL_unify_functor(arg, fr->predicate->functor->functor);
      argp = valTermRef(arg);
      deRef(argp);
      argp = argTermP(*argp, 0);

      for(n=0; n < arity; n++, argp++)
      { Word a;

	deRef2(argv+n, a);
	if ( isVar(*a) && onStack(local, a) && !gc_status.blocked )
	{ *a = makeRef(argp);
	  Trail(a);
	} else
	  *argp = (needsRef(*a) ? makeRef(a) : *a);
      }
    }
  } else if ( key == ATOM_predicate_indicator )
  { unify_definition(result, fr->predicate, 0, GP_NAMEARITY);
  } else if ( key == ATOM_parent_goal )
  { Procedure proc;
    term_t head = PL_new_term_ref();

    if ( !get_procedure(value, &proc, head, GP_FIND) )
      fail;
    while(fr)
    { while(fr && fr->predicate != proc->definition)
	fr = parentFrame(fr);

      if ( fr )
      { term_t a  = PL_new_term_ref();
	term_t fa = argFrameP(fr, 0) - (Word)lBase;
	int i, arity = fr->predicate->functor->arity;

	for(i=0; i<arity; i++, fa++)
	{ _PL_get_arg(i+1, head, a);
	  if ( !PL_unify(a, fa) )
	    break;
	}
        if ( i == arity )
	  succeed;
      } else
	fail;

      fr = parentFrame(fr);
    }
  } else if ( key == ATOM_pc )
  { if ( fr->programPointer &&
	 fr->parent &&
	 false(fr->parent->predicate, FOREIGN) &&
	 fr->parent->clause &&
	 fr->parent->predicate != PROCEDURE_dcall1->definition )
    { intptr_t pc = fr->programPointer - fr->parent->clause->clause->codes;

      if ( pc < 0 )
	trap_gdb();

      PL_put_intptr(result, pc);
    } else
    { fail;
    }
  } else if ( key == ATOM_hidden )
  { atom_t a;

    if ( SYSTEM_MODE )
    { a = ATOM_true;
    } else
    { if ( isDebugFrame(fr) )
	a = ATOM_false;
      else
	a = ATOM_true;
    }

    PL_put_atom(result, a);
  } else if ( key == ATOM_depth_limit_exceeded )
  { atom_t a;				/* get limit from saved query */

#ifdef O_LIMIT_DEPTH
    QueryFrame qf = findQuery(environment_frame);

    if ( qf && (uintptr_t)levelFrame(fr) > qf->saved_depth_limit )
      a = ATOM_true;
    else
#endif
      a = ATOM_false;

    PL_put_atom(result, a);
  } else
    goto unknown_key;

  return PL_unify(value, result);
}


		 /*******************************
		 *	 CHOICEPOINT STACK	*
		 *******************************/

foreign_t
pl_prolog_choice_attribute(term_t choice, term_t what, term_t value)
{ Choice ch = NULL;
  atom_t key;

  if ( !PL_get_choice(choice, &ch) ||
       !PL_get_atom_ex(what, &key) )
    fail;

  if ( key == ATOM_parent )
  { if ( ch->parent )
      return PL_unify_choice(value, ch->parent);
    fail;
  } else if ( key == ATOM_frame )
  { return PL_unify_frame(value, ch->frame);
  } else if ( key == ATOM_type )
  { static const atom_t types[] =
    { ATOM_jump,
      ATOM_clause,
      ATOM_foreign,
      ATOM_top,
      ATOM_catch,
      ATOM_debug,
      ATOM_none
    };

    return PL_unify_atom(value, types[ch->type]);
  } else
    return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_key, what);

}

#if O_DEBUGGER

		 /*******************************
		 *	  PROLOG EVENT HOOK	*
		 *******************************/

void
callEventHook(int ev, ...)
{ if ( !PROCEDURE_event_hook1 )
    PROCEDURE_event_hook1 = PL_predicate("prolog_event_hook", 1, "user");

  if ( PROCEDURE_event_hook1->definition->definition.clauses )
  { va_list args;
    fid_t fid, wake;
    term_t arg;
    term_t ex;

    blockGC(PASS_LD1);
    wake = saveWakeup(PASS_LD1);
    fid = PL_open_foreign_frame();
    arg = PL_new_term_ref();

    if ( exception_term )
    { ex = PL_copy_term_ref(exception_term);
      exception_term = 0;
    } else
      ex = 0;

    va_start(args, ev);
    switch(ev)
    { case PLEV_ERASED:
      {	void *ptr = va_arg(args, void *); 	/* object erased */

	PL_unify_term(arg, PL_FUNCTOR, FUNCTOR_erased1,
		           PL_POINTER, ptr);
	break;
      }
      case PLEV_DEBUGGING:
      { int dbg = va_arg(args, int);

	PL_unify_term(arg, PL_FUNCTOR, FUNCTOR_debugging1,
			   PL_ATOM, dbg ? ATOM_true : ATOM_false);
	break;
      }
      case PLEV_TRACING:
      { int trc = va_arg(args, int);

	PL_unify_term(arg, PL_FUNCTOR, FUNCTOR_tracing1,
			   PL_ATOM, trc ? ATOM_true : ATOM_false);
	break;
      }
      case PLEV_BREAK:
      case PLEV_NOBREAK:
      { Clause clause = va_arg(args, Clause);
	int offset = va_arg(args, int);

	PL_unify_term(arg, PL_FUNCTOR, FUNCTOR_break3,
		           PL_POINTER, clause,
		           PL_INT, offset,
			   PL_ATOM, ev == PLEV_BREAK ? ATOM_true
					             : ATOM_false);
	break;
      }
      case PLEV_FRAMEFINISHED:
      { LocalFrame fr = va_arg(args, LocalFrame);
	term_t ref = PL_new_term_ref();

	PL_put_frame(ref, fr);
	PL_unify_term(arg, PL_FUNCTOR, FUNCTOR_frame_finished1,
		           PL_TERM, ref);
	break;
      }
#ifdef O_PLMT
      case PL_EV_THREADFINISHED:
      { PL_thread_info_t *info = va_arg(args, PL_thread_info_t*);
	term_t id = PL_new_term_ref();

	unify_thread_id(id, info);
	PL_unify_term(arg, PL_FUNCTOR_CHARS, "thread_finished", 1,
		             PL_TERM, id);
	break;
      }
#endif
      default:
	warning("callEventHook(): unknown event: %d", ev);
        goto out;
    }

    PL_call_predicate(MODULE_user, FALSE, PROCEDURE_event_hook1, arg);
  out:

    if ( ex )
    { PL_put_term(exception_bin, ex);
      exception_term = exception_bin;
    }

    PL_discard_foreign_frame(fid);
    restoreWakeup(wake PASS_LD);
    unblockGC(PASS_LD1);
    va_end(args);
  }
}

#endif /*O_DEBUGGER*/
