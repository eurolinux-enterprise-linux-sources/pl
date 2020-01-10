/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemak@uva.nl
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

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Implementation of the Virtual Machine Instructions (VMI).

Each VMI has the structure below. Using this structure we are completely
flexible in how we implement the instruction   and  we can easily create
the derived tables, which is done by the program mkvmi.c.

	VMI(Name, Flags, #Args, (ArgType, ...))
	{
	}

Within the scope of this file,   the following virtual machine variables
are available.

	* FR
	Current environment frame

	* NFR
	Next frame (used to share in various calling instructions)

	* BFR
	Backtrack frame: current choicepoint

	* PC
	Program Counter

	* ARGP
	Argument pointer

	* CL
	Running clause (= FR->clause)

	* DEF
	Running definition

Virtual machine instructions can return with one of:

	* NEXT_INSTRUCTION
	Proceed

	* CLAUSE_FAILED
	Failed unifying the head: backtrack to next clause

	* BODY_FAILED
	Failure of in-body instructions (I_FAIL, B_UNIFY_EXIT, ...)

	* FRAME_FAILED
	Other failures: deep backtracking.

	* VMI_GOTO(VMI)
	Continue executing another virtual instruction.  Note this is
	called GOTO as it is a jump rather than a call.

Virtual machine instruction names.  Prefixes:

  I_	General instructions
  B_	Body specific version
  H_	Head specific versin
  A_	Arithmetic compilation specific
  C_	Control (compilation of ;/2, etc.)
  S_   Supervisor instructions.  See pl-supervisor.c
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
				ISSUES


Sometime multiple instructions share variables. This   is done using the
code below, so we can at  least   identify  them easily. Ultimately, the
dependencies must be removed, probably  mostly   by  moving the reusable
code into functions.

	BEGIN_SHAREDVARS
	Decls
	VMI(...)
	VMI(...)
	END_SHAREDVARS
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define BEGIN_SHAREDVARS {
#define END_SHAREDVARS   }


		 /*******************************
		 *	 ATTRIBUTED VARS	*
		 *******************************/

#ifdef O_ATTVAR
#define CHECK_WAKEUP \
	if ( LD->alerted & ALERT_WAKEUP ) \
	{ LD->alerted &= ~ALERT_WAKEUP; \
	  if ( *valTermRef(LD->attvar.head) ) \
	    goto wakeup; \
	}
#else
#define CHECK_WAKEUP (void)0
#endif


		 /*******************************
		 *	    DEBUGGING		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
D_BREAK implements break-points in the  code.   A  break-point is set by
replacing  an  instruction  by  a   D_BREAK  instruction.  The  orininal
instruction is saved in a table. replacedBreak() fetches it.

Note that we must  be  careful  that   the  user  may  have  removed the
break-point in the debugger, so we must check for it.

We might be in a state where  we   are  writing  the arguments above the
current lTop, and therefore with higher this  with the maximum number of
arguments.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(D_BREAK, 0, 0, ())
{
#if O_DEBUGGER
  if ( debugstatus.debugging )
  { int action;
    LocalFrame lSave = lTop;

    lTop = (LocalFrame)argFrameP(lTop, MAXARITY);
    clearUninitialisedVarsFrame(FR, PC-1);
    action = tracePort(FR, BFR, BREAK_PORT, PC-1 PASS_LD);
    lTop = lSave;

    switch(action)
    { case ACTION_RETRY:
	goto retry;
    }

    if ( PC[-1] != encode(D_BREAK) )
    { PC--;
      NEXT_INSTRUCTION;
    }
  }
#if VMCODE_IS_ADDRESS
  { void *c = (void *)replacedBreak(PC-1);

    goto *c;
  }
#else
  thiscode = replacedBreak(PC-1);
  goto resumebreak;
#endif
#endif /*O_DEBUGGER*/
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_NOP
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_NOP, 0, 0, ())
{ NEXT_INSTRUCTION;
}

		 /*******************************
		 *	     HEAD UNIFY		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_CONST is used for an atomic constant in   the head of the clause. ARGP
points to the current argument to  be   matched.  ARGP is derefenced and
unified with a constant argument.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_CONST, 0, 1, (CA1_DATA))
{ word c;
  Word k;

  IF_WRITE_MODE_GOTO(B_CONST);

  c = (word)*PC++;
  deRef2(ARGP++, k);
  if ( *k == c )
    NEXT_INSTRUCTION;
  if ( canBind(*k) )
  { bindConst(k, c);
    NEXT_INSTRUCTION;
  }
  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_NIL is used for [] in the head.  See H_CONST for details.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_NIL, 0, 0, ())
{ word c;
  Word k;

  IF_WRITE_MODE_GOTO(B_NIL);

  c = ATOM_nil;
  deRef2(ARGP++, k);
  if ( *k == c )
    NEXT_INSTRUCTION;
  if ( canBind(*k) )
  { bindConst(k, c);
    NEXT_INSTRUCTION;
  }
  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_INTEGER: Long integer in  the  head.   Note  that  small  integers are
handled through H_CONST. Copy to the  global   stack  if the argument is
variable, compare the numbers otherwise.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_INTEGER, 0, 1, (CA1_INTEGER))
{ Word k;

  IF_WRITE_MODE_GOTO(B_INTEGER);

  deRef2(ARGP++, k);
  if ( canBind(*k) )
  { Word p = allocGlobal(2+WORDS_PER_INT64);
    word c = consPtr(p, TAG_INTEGER|STG_GLOBAL);
    union
    { int64_t val;
      word w[WORDS_PER_INT64];
    } cvt;
    Word vp = cvt.w;

    cvt.val = (int64_t)(intptr_t)*PC++;
    *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
    cpInt64Data(p, vp);
    *p = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
    bindConst(k, c);
    NEXT_INSTRUCTION;
  } else if ( isBignum(*k) && valBignum(*k) == (intptr_t)*PC++ )
    NEXT_INSTRUCTION;

  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_INT64: 64-bit integer in the head. Only applicable for 32-bit hardware
as this is the same as H_INTEGER on 64-bit hardware.

TBD: Compile conditionally
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_INT64, 0, WORDS_PER_INT64, (CA1_INT64))
{ Word k;

  IF_WRITE_MODE_GOTO(B_INT64);

  deRef2(ARGP++, k);
  if ( canBind(*k) )
  { Word p = allocGlobal(2+WORDS_PER_INT64);
    word c = consPtr(p, TAG_INTEGER|STG_GLOBAL);
    size_t i;

    *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
    for(i=0; i<WORDS_PER_INT64; i++)
      *p++ = (word)*PC++;
    *p = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
    bindConst(k, c);
    NEXT_INSTRUCTION;
  } else if ( isBignum(*k) )
  { Word vk = valIndirectP(*k);
    size_t i;

    for(i=0; i<WORDS_PER_INT64; i++)
    { if ( *vk++ != (word)*PC++ )
	CLAUSE_FAILED;
    }
    NEXT_INSTRUCTION;
  }

  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_FLOAT: Float in the head. The  float   follows  the instruction and is
represented as a native C-double.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_FLOAT, 0, WORDS_PER_DOUBLE, (CA1_FLOAT))
{ Word k;

  IF_WRITE_MODE_GOTO(B_FLOAT);

  deRef2(ARGP++, k);
  if ( canBind(*k) )
  { Word p = allocGlobal(2+WORDS_PER_DOUBLE);
    word c = consPtr(p, TAG_FLOAT|STG_GLOBAL);

    *p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
    cpDoubleData(p, PC);
    *p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
    bindConst(k, c);
    NEXT_INSTRUCTION;
  } else if ( isFloat(*k) )
  { Word p = valIndirectP(*k);

    switch(WORDS_PER_DOUBLE) /* depend on compiler to clean up */
    { case 2:
	if ( *p++ != *PC++ )
	  CLAUSE_FAILED;
      case 1:
	if ( *p++ == *PC++ )
	  NEXT_INSTRUCTION;
	CLAUSE_FAILED;
      default:
	assert(0);
    }
  }

  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_MPZ and H_STRING are used for an mpz   number  and string in the head.
They are both implemented using the generic  code for indirects, but the
decompiler must be able to recognise  the   instruction  which is why we
have two instructions.

TBD:	Deal with multiple identical instructions
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_MPZ, 0, VM_DYNARGC, (CA1_MPZ))
{ SEPERATE_VMI;
  VMI_GOTO(H_STRING)
}


VMI(H_STRING, 0, VM_DYNARGC, (CA1_STRING))
{ Word k;

  IF_WRITE_MODE_GOTO(B_STRING);

  deRef2(ARGP++, k);
  if ( canBind(*k) )
  { word c = globalIndirectFromCode(&PC);
    bindConst(k, c);
    NEXT_INSTRUCTION;
  }
  if ( isIndirect(*k) && equalIndirectFromCode(*k, &PC) )
    NEXT_INSTRUCTION;
  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_VOID: A singleton variable in the   head.  Just increment the argument
pointer. Also generated for non-singleton   variables appearing on their
own in the head and  encountered  for   the  first  time.  Note that the
compiler suppresses H_VOID when there are   no other instructions before
I_ENTER or I_EXIT.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_VOID, 0, 0, ())
{ ARGP++;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_ARGVOID: Singleton inside an term in the head. If we are in write mode
we must make a variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_ARGVOID, 0, 0, ())
{ if ( umode == uwrite )
  { setVar(*ARGP);
  }

  ARGP++;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_VAR: A variable in the head which is   not an anonymous one and is not
used for the first time. Invoke general unification between the argument
pointer and the variable, whose offset is given relative to the frame.

When in write-mode, ARGP points  to   uninitialised  data  on the global
stack that must be treated as a variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_VAR, 0, 1, (CA1_VAR))
{ Word k = varFrameP(FR, (int)*PC++);

  if ( umode == uwrite )
  { if ( LD->prolog_flag.occurs_check == OCCURS_CHECK_FALSE )
    { deRef(k);
      if ( isVar(*k) )
      { if ( k > ARGP )			/* k on local stack */
	{ setVar(*ARGP);
	  *k = makeRefG(ARGP);
	  Trail(k);
	} else
	{ *ARGP = makeRefG(k);		/* ARGP on global, so k also */
	}
      } else if ( isAttVar(*k) )
      { *ARGP = makeRefG(k);
      } else
      { *ARGP = *k;
      }

      ARGP++;
      NEXT_INSTRUCTION;
    } else
    { setVar(*ARGP);
    }
  }

  if ( raw_unify_ptrs(k, ARGP++ PASS_LD) )
    NEXT_INSTRUCTION;
  if ( exception_term )
    goto b_throw;
  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_FIRSTVAR: A variable  in  the  head,   which  is  not  anonymous,  but
encountered for the first time. So we know  that the variable is still a
variable. Copy or make a reference.  Trailing   is  not needed as we are
writing in this frame. As ARGP is pointing   in the argument list, it is
on the local stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_FIRSTVAR, 0, 1, (CA1_VAR))
{ if ( umode == uwrite )
  { setVar(*ARGP);
    varFrame(FR, *PC++) = makeRefG(ARGP);
  } else
  { varFrame(FR, *PC++) = (needsRef(*ARGP) ? makeRef(ARGP) : *ARGP);
  }
  ARGP++;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_FUNCTOR: A functor in the head. If  the current argument is a variable
we instantiate it with a  new  term,   all  whose  arguments  are set to
variables. Otherwise we check the functor  definition. In both case ARGP
is pushed on the  argument  stack  and   set  to  point  to the leftmost
argument of the  term.  Note  that   the  instantiation  is  trailed  as
dereferencing might have caused we are now pointing in a parent frame or
the global stack (should we check? Saves trail! How often?).

H_RFUNCTOR: Right-most functor.  Achieves right-argument optimization of
the argument stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_FUNCTOR, 0, 1, (CA1_FUNC))
{ requireStack(argument, sizeof(Word));
  *aTop++ = (Word)((intptr_t)(ARGP + 1)|umode);
  VMI_GOTO(H_RFUNCTOR);
}

VMI(H_RFUNCTOR, 0, 1, (CA1_FUNC))
{ functor_t f;

  IF_WRITE_MODE_GOTO(B_RFUNCTOR);

  f = (functor_t) *PC++;
  deRef(ARGP);
  if ( canBind(*ARGP) )
  { int arity = arityFunctor(f);
    Word ap;
    word c;

#ifdef O_SHIFT_STACKS
    if ( gTop + 1 + arity > gMax )
    { if ( !growStacks(FR, BFR, PC, 0, sizeof(word)*(1+arity), 0) )
	goto b_throw;
    }
#else
    requireStack(global, sizeof(word)*(1+arity));
#endif

    ap = gTop;
    gTop += 1+arity;
    c = consPtr(ap, TAG_COMPOUND|STG_GLOBAL);
    bindConst(ARGP, c);
    *ap++ = f;
    ARGP = ap;
    umode = uwrite;
    NEXT_INSTRUCTION;
  }
  if ( hasFunctor(*ARGP, f) )
  { ARGP = argTermP(*ARGP, 0);
    NEXT_INSTRUCTION;
  }
  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_LIST:  As H_FUNCTOR, but using ./2 as predefined functor.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_LIST, 0, 0, ())
{ requireStack(argument, sizeof(Word));
  *aTop++ = (Word)((intptr_t)(ARGP + 1)|umode);

  VMI_GOTO(H_RLIST);
}


VMI(H_RLIST, 0, 0, ())
{ IF_WRITE_MODE_GOTO(B_RLIST);

  for(;;)
  { word w = *ARGP;

    switch(tag(w))
    { case TAG_COMPOUND:
	if ( valueTerm(w)->definition == FUNCTOR_dot2 )
	{ ARGP = argTermP(w, 0);
	  NEXT_INSTRUCTION;
	}
	CLAUSE_FAILED;
      case TAG_VAR:
      case TAG_ATTVAR:
      { Word ap = gTop;
	word c;

#if O_SHIFT_STACKS
        if ( ap + 3 > gMax )
	{ if ( !growStacks(FR, BFR, PC, 0, 3*sizeof(word), 0) )
	    goto b_throw;
	  ap = gTop;
	}
#else
	if ( ap + 3 > gMax )
	  ensureRoomStack(global, 3*sizeof(word));
#endif
        gTop += 3;
	c = consPtr(ap, TAG_COMPOUND|STG_GLOBAL);
	*ap++ = FUNCTOR_dot2;
	bindConst(ARGP, c);
	ARGP = ap;
	umode = uwrite;
	NEXT_INSTRUCTION;
      }
      case TAG_REFERENCE:
	ARGP = unRef(w);
        continue;
      default:
	CLAUSE_FAILED;
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_POP: Pop the saved argument pointer pushed by H_FUNCTOR and H_LIST.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_POP, 0, 0, ())
{ ARGP = *--aTop;
  umode = ((int)(uintptr_t)ARGP & uwrite);
  ARGP = (Word)((intptr_t)ARGP&~uwrite);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
H_LIST_FF: [Var1|Var2] in the head where both   Var1 and Var2 appear for
the  first  time.  This  appears    quite  commonly  in  list-processing
predicates:

	pred([], ...).
	pred([H|T], ...) :-
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(H_LIST_FF, 0, 2, (CA1_VAR,CA1_VAR))
{ Word p;

  if ( umode == uwrite )
  { p = ARGP++;
    goto write;
  } else
  { deRef2(ARGP, p);
    ARGP++;

    if ( isList(*p) )
    { p = argTermP(*p, 0);
      varFrame(FR, *PC++) = (needsRef(*p) ? makeRef(p) : *p);
      p++;
      varFrame(FR, *PC++) = (needsRef(*p) ? makeRef(p) : *p);
    } else if ( canBind(*p) )
    { word c;
      Word ap;

    write:
      requireStack(global, 3*sizeof(word));
      ap = gTop;
      gTop = ap+3;
      c = consPtr(ap, TAG_COMPOUND|STG_GLOBAL);
      *ap++ = FUNCTOR_dot2;
      setVar(*ap); varFrame(FR, *PC++) = makeRefG(ap); ap++;
      setVar(*ap); varFrame(FR, *PC++) = makeRefG(ap);
      if ( umode == uwrite )
	*p = c;
      else
	bindConst(p, c);
    } else
    { CLAUSE_FAILED;
    }
  }

  NEXT_INSTRUCTION;
}


		 /*******************************
		 *	 BODY UNIFICATION	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_CONST, B_NIL: An atomic constant in the body of a clause. We know that
ARGP is pointing to a not yet   instantiated  argument of the next frame
and therefore can just fill the argument. Trailing is not needed as this
is above the stack anyway.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_CONST, 0, 1, (CA1_DATA))
{ *ARGP++ = (word)*PC++;
  NEXT_INSTRUCTION;
}

VMI(B_NIL, 0, 0, ())
{ *ARGP++ = ATOM_nil;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_INTEGER: Long following PC for integers   that  cannot be expressed as
tagged integer.

TBD:	Merge the code writing longs to the stack
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_INTEGER, 0, 1, (CA1_INTEGER))
{ Word p = allocGlobal(2+WORDS_PER_INT64);
  union
  { int64_t val;
    word w[WORDS_PER_INT64];
  } cvt;
  Word vp = cvt.w;

  cvt.val = (int64_t)(intptr_t)*PC++;
  *ARGP++ = consPtr(p, TAG_INTEGER|STG_GLOBAL);
  *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
  cpInt64Data(p, vp);
  *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_INT64: 64-bit (int64_t) in the body.  See H_INT64
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_INT64, 0, WORDS_PER_INT64, (CA1_INT64))
{ Word p = allocGlobal(2+WORDS_PER_INT64);
  size_t i;

  *ARGP++ = consPtr(p, TAG_INTEGER|STG_GLOBAL);
  *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
  for(i=0; i<WORDS_PER_INT64; i++)
    *p++ = (word)*PC++;
  *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_FLOAT: Float in the  body.  PC  is   followed  by  a  double in native
representation.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_FLOAT, 0, WORDS_PER_DOUBLE, (CA1_FLOAT))
{ Word p = allocGlobal(2+WORDS_PER_DOUBLE);

  *ARGP++ = consPtr(p, TAG_FLOAT|STG_GLOBAL);
  *p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
  cpDoubleData(p, PC);
  *p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_MPZ: MPZ number in body
B_STRING: string in body

Both copy following indirect to the  global   stack.  See also H_MPZ and
H_STRING.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_MPZ, 0, VM_DYNARGC, (CA1_MPZ))
{ SEPERATE_VMI;
  VMI_GOTO(B_STRING);
}

VMI(B_STRING, 0, VM_DYNARGC, (CA1_STRING))
{ *ARGP++ = globalIndirectFromCode(&PC);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_ARGVAR: A variable in the body which is   not an anonymous one, is not
used for the first time and is nested in a term (with B_FUNCTOR). We now
know that *ARGP is a variable, so  we   either  copy the value or make a
reference. The difference between this one and B_VAR is the direction of
the reference link in case *k turns out to be variable.

ARGP is pointing into the term on the global stack we are creating.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_ARGVAR, 0, 1, (CA1_VAR))
{ Word k;

  deRef2(varFrameP(FR, *PC++), k);
  if ( isVar(*k) )
  { if ( ARGP < k )
    { setVar(*ARGP);
      *k = makeRefG(ARGP++);
      Trail(k);
      NEXT_INSTRUCTION;
    }
    *ARGP++ = makeRefG(k);	/* both on global stack! */
#ifdef O_ATTVAR
  } else if ( isAttVar(*k) )
  { *ARGP++ = makeRefG(k);
#endif
  } else
  { *ARGP++ = *k;
  }

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_VAR, B_VAR<N>: A variable in the body   which  is not an anonymous one
and is not used for  the  first  time.   We  now  know  that  *ARGP is a
variable, so we either copy the value   or make a reference. Trailing is
not needed as we are writing above the stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_VAR0, 0, 0, ())
{ *ARGP++ = linkVal(varFrameP(FR, VAROFFSET(0)));
  NEXT_INSTRUCTION;
}

VMI(B_VAR1, 0, 0, ())
{ *ARGP++ = linkVal(varFrameP(FR, VAROFFSET(1)));
  NEXT_INSTRUCTION;
}

VMI(B_VAR2, 0, 0, ())
{ *ARGP++ = linkVal(varFrameP(FR, VAROFFSET(2)));
  NEXT_INSTRUCTION;
}

VMI(B_VAR, 0, 1, (CA1_VAR))
{ int n = (int)*PC++;

  *ARGP++ = linkVal(varFrameP(FR, n));
  NEXT_INSTRUCTION;
}


#ifdef O_COMPILE_IS
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_UNIFY_VAR, B_UNIFY_EXIT: Unification in the body. We compile A = Term
into one of the following:

    Normal:			    A is firstvar:
	B_UNIFY_VAR <A>			B_UNIFY_FIRSTVAR <A>
	<head unify instructions>	<body unify instructions>
	B_UNIFY_EXIT			B_UNIFY_EXIT

We  need  B_UNIFY_FIRSTVAR  for   the   debugger    as   well   as   for
clearUninitialisedVarsFrame() in pl-gc.c. When in   debug mode we simply
create a frame for =/2 and call it.

TBD: B_UNIFY_CONST <var>, <const>
     B_UNIFY_VAR <var1>, <var2>
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_UNIFY_FIRSTVAR, 0, 1, (CA1_VAR))
{ SEPERATE_VMI;
  VMI_GOTO(B_UNIFY_VAR);
}


VMI(B_UNIFY_VAR, 0, 1, (CA1_VAR))
{ ARGP = varFrameP(FR, (int)*PC++);

#if O_DEBUGGER
  if ( debugstatus.debugging )
  { Word k = ARGP;

    if ( decode(PC[-2]) == B_UNIFY_FIRSTVAR )
      setVar(*k);
    ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(k);
    setVar(*ARGP);
    umode = uread;
    NEXT_INSTRUCTION;
  }
#endif

  umode = uread;			/* needed? */
  NEXT_INSTRUCTION;
}


VMI(B_UNIFY_EXIT, VIF_BREAK, 0, ())
{ ARGP = argFrameP(lTop, 0);

#if O_DEBUGGER
  if ( debugstatus.debugging )
  { NFR = lTop;
    DEF = GD->procedures.equals2->definition;
    setNextFrameFlags(NFR, FR);
    goto normal_call;
  }
#endif

  CHECK_WAKEUP;				/* only for non-first-var */
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	B_UNIFY_[FV][FV] VAR1 VAR2
Unify two variables.  F stands for a first-var; V for any other var
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_UNIFY_FF, VIF_BREAK, 2, (CA1_VAR,CA1_VAR))
{ Word v1 = varFrameP(FR, (int)*PC++);
  Word v2 = varFrameP(FR, (int)*PC++);

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { setVar(*v1);
    setVar(*v2);
    ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(v1);
    *ARGP++ = linkVal(v2);
    goto debug_equals2;
  }
#endif

  setVar(*v1);
  *v2 = makeRefL(v1);

  NEXT_INSTRUCTION;
}


VMI(B_UNIFY_FV, VIF_BREAK, 2, (CA1_VAR,CA1_VAR))
{ Word v1 = varFrameP(FR, (int)*PC++);
  Word v2 = varFrameP(FR, (int)*PC++);

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { setVar(*v1);
    ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(v1);
    *ARGP++ = linkVal(v2);
    goto debug_equals2;
  }
#endif

  *v1 = linkVal(v2);

  NEXT_INSTRUCTION;
}


VMI(B_UNIFY_VV, VIF_BREAK, 2, (CA1_VAR,CA1_VAR))
{ Word v1 = varFrameP(FR, (int)*PC++);
  Word v2 = varFrameP(FR, (int)*PC++);

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(v1);
    *ARGP++ = linkVal(v2);
  debug_equals2:
    NFR = lTop;
    DEF = GD->procedures.equals2->definition;
    setNextFrameFlags(NFR, FR);
    goto normal_call;
  }
#endif

  if ( raw_unify_ptrs(v1, v2 PASS_LD) )
  { CHECK_WAKEUP;
    NEXT_INSTRUCTION;
  }
  if ( exception_term )
    goto b_throw;

  BODY_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_UNIFY_FC: Unify first variable with a constant.  Always succeeds, no
need for wakeup.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_UNIFY_FC, VIF_BREAK, 2, (CA1_VAR, CA1_DATA))
{ Word v1 = varFrameP(FR, (int)*PC++);
  word c = (word)*PC++;

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { setVar(*v1);
    ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(v1);
    *ARGP++ = c;
    goto debug_equals2;
  }
#endif

  *v1 = c;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_UNIFY_VC: Unify a variable (not first) with a constant in the body.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_UNIFY_VC, VIF_BREAK, 2, (CA1_VAR, CA1_DATA))
{ Word k = varFrameP(FR, (int)*PC++);
  word c = (word)*PC++;

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(k);
    *ARGP++ = c;
    goto debug_equals2;
  }
#endif

  deRef(k);
  if ( *k == c )
    NEXT_INSTRUCTION;
  if ( canBind(*k) )
  { bindConst(k, c);
    CHECK_WAKEUP;
    NEXT_INSTRUCTION;
  }
  CLAUSE_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_EQ_VV: translation of	Var1 == Var2
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_EQ_VV, VIF_BREAK, 2, (CA1_VAR,CA1_VAR))
{ Word v1 = varFrameP(FR, (int)*PC++);
  Word v2 = varFrameP(FR, (int)*PC++);

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(v1);
    *ARGP++ = linkVal(v2);
  debug_eq_vv:
    NFR = lTop;
    DEF = GD->procedures.strict_equal2->definition;
    setNextFrameFlags(NFR, FR);
    goto normal_call;
  }
#endif

  if ( compareStandard(v1, v2, TRUE PASS_LD) == 0 )
    NEXT_INSTRUCTION;

  BODY_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_EQ_VC Var == constant
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_EQ_VC, VIF_BREAK, 2, (CA1_VAR,CA1_DATA))
{ Word v1 = varFrameP(FR, (int)*PC++);
  word c  = (word)*PC++;

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { ARGP = argFrameP(lTop, 0);
    *ARGP++ = linkVal(v1);
    *ARGP++ = c;
    goto debug_eq_vv;
  }
#endif

  deRef(v1);
  if ( *v1 == c )
    NEXT_INSTRUCTION;

  BODY_FAILED;
}


#endif /*O_COMPILE_IS*/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_ARGFIRSTVAR: A variable in the body nested  in a term, encountered for
the first time. We now know both   *ARGP and the variable are variables.
ARGP points to the argument of a term on the global stack. The reference
should therefore go from k to ARGP.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_ARGFIRSTVAR, 0, 1, (CA1_VAR))
{ setVar(*ARGP);
  varFrame(FR, *PC++) = makeRefG(ARGP++);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_FIRSTVAR: A variable in the body, encountered   for the first time. We
now know both *ARGP and the variable  are variables. We set the variable
to be a variable (it is uninitialised   memory) and make a reference. No
trailing needed as we are writing in this and the next frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_FIRSTVAR, 0, 1, (CA1_VAR))
{ Word k = varFrameP(FR, *PC++);

  setVar(*k);
  *ARGP++ = makeRefL(k);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_VOID: A singleton variable in  the  body.   Ensure  the  argument is a
variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_VOID, 0, 0, ())
{ setVar(*ARGP++);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_FUNCTOR: A functor in the body. As we   don't  expect ARGP to point to
initialised memory while in body mode  we   just  allocate the term, but
don't initialise the arguments to variables. Allocation is done in place
to avoid a function call.

B_RFUNCTOR: right-argument recursive version of B_FUNCTOR
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_FUNCTOR, 0, 1, (CA1_FUNC))
{ requireStack(argument, sizeof(Word));
  *aTop++ = ARGP+1;
  VMI_GOTO(B_RFUNCTOR);
}


VMI(B_RFUNCTOR, 0, 1, (CA1_FUNC))
{ functor_t f = (functor_t) *PC++;
  int arity = arityFunctor(f);

  requireStack(global, sizeof(word) * (1+arity));
  *ARGP = consPtr(gTop, TAG_COMPOUND|STG_GLOBAL);
  ARGP = gTop;
  *ARGP++ = f;
  gTop = ARGP+arity;

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_LIST: Same as B_FUNCTOR for ./2
B_RLIST: Right-argument recursive B_LIST
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_LIST, 0, 0, ())
{ requireStack(argument, sizeof(Word));
  *aTop++ = ARGP+1;
  VMI_GOTO(B_RLIST);
}


VMI(B_RLIST, 0, 0, ())
{ requireStack(global, sizeof(word) * 3);
  *ARGP = consPtr(gTop, TAG_COMPOUND|STG_GLOBAL);
  ARGP = gTop;
  *ARGP++ = FUNCTOR_dot2;
  gTop = ARGP+2;

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_POPF: Pop the saved argument pointer (see H_FUNCTOR and B_FUNCTOR).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_POP, 0, 0, ())
{ ARGP = *--aTop;
  NEXT_INSTRUCTION;
}


		 /*******************************
		 *	       ENTER		*
		 *******************************/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_ENTER: Enter the body of the clause.   This instruction is left out if
the clause has no body. The basic task   of  this instruction is to move
ARGP from the argument part of this frame  into the argument part of the
child frame to be built. `BFR' (the last frame with alternatives) is set
to  this  frame  if  this  frame  has  alternatives,  otherwise  to  the
backtrackFrame of this frame.

If this frame has no alternatives it is possible to  put  the  backtrack
frame  immediately  on  the backtrack frame of this frame.  This however
makes debugging much more  difficult  as  the  system  will  do  a  deep
backtrack without showing the fail ports explicitely.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_ENTER, VIF_BREAK, 0, ())
{ ARGP = argFrameP(lTop, 0);

  if ( LD->alerted )
  {
#if O_DEBUGGER
    if ( debugstatus.debugging )
    { clearUninitialisedVarsFrame(FR, PC);
      switch(tracePort(FR, BFR, UNIFY_PORT, PC PASS_LD))
      { case ACTION_RETRY:
	  goto retry;
	case ACTION_FAIL:
	  FRAME_FAILED;
      }
    }
#endif /*O_DEBUGGER*/

    CHECK_WAKEUP;
  }
  NEXT_INSTRUCTION;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CONTEXT is used by  non-meta  predicates   that  are  compiled  into a
different  module  using  <module>:<head>  :-    <body>.  The  I_CONTEXT
instruction immediately follows the I_ENTER. The argument is the module.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_CONTEXT, 0, 1, (CA1_MODULE))
{ Module m = (Module)*PC++;

  setContextModule(FR, m);

  NEXT_INSTRUCTION;
}

		 /*******************************
		 *	       CALLING		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CALL forms the normal  code  generated   by  the  compiler for calling
predicates. The arguments are already written   in the frame starting at
`lTop'.

The task of I_CALL is to  save  necessary  information  in  the  current
frame,  fill  the next frame and initialise the machine registers.  Then
execution can continue at `next_instruction'
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_CALL, VIF_BREAK, 1, (CA1_PROC))
{ Procedure proc = (Procedure) *PC++;

  NFR = lTop;
  setNextFrameFlags(NFR, FR);
  DEF = proc->definition;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This is the common part of the call variations.  By now the following is
true:

  - NFR			Points to new frame
  - arguments		filled
  - DEF			filled with predicate to call
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

normal_call:
					/* ensure room for next args */
  requireStack(local, (size_t)argFrameP((LocalFrame)NULL, MAXARITY));

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Initialise those slots of the frame that are common to Prolog predicates
and foreign ones.  There might be some possibilities for optimisation by
delaying these initialisations till they are really  needed  or  because
the information they are calculated from is destroyed.  This probably is
not worthwile.

Note: we are working above `lTop' here!!   We restore this as quickly as
possible to be able to call-back to Prolog.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  NFR->parent         = FR;
  NFR->predicate      = DEF;		/* TBD */
  NFR->programPointer = PC;		/* save PC in child */
  NFR->clause         = NULL;		/* for save atom-gc */
  environment_frame = FR = NFR;		/* open the frame */

depart_continue:
retry_continue:
#ifdef O_LOGICAL_UPDATE
  FR->generation     = GD->generation;
#endif
#ifdef O_PROFILE
  FR->prof_node = NULL;
#endif
  LD->statistics.inferences++;

#ifdef O_DEBUGLOCAL
{ Word ap = argFrameP(FR, DEF->functor->arity);
  int n;

  for(n=50; --n; )
    *ap++ = (word)(((char*)ATOM_nil) + 1);
}
#endif

  if ( LD->alerted )
  {					/* play safe */
    lTop = (LocalFrame) argFrameP(FR, DEF->functor->arity);

    if ( LD->outofstack )
    { enterDefinition(DEF);		/* exception will lower! */
      outOfStack(LD->outofstack, STACK_OVERFLOW_RAISE);
      goto b_throw;
    }

    if ( is_signalled(PASS_LD1) )
    { SAVE_REGISTERS(qid);
      handleSignals(NULL);
      LOAD_REGISTERS(qid);
      if ( exception_term )
      { CL = NULL;

	enterDefinition(DEF);
					/* The catch is not yet installed, */
					/* so we ignore it */
	if ( FR->predicate == PROCEDURE_catch3->definition )
	  set(FR, FR_CATCHED);

	goto b_throw;
      }
    }

#ifdef O_PROFILE
    if ( LD->profile.active )
      FR->prof_node = profCall(DEF PASS_LD);
#endif

#ifdef O_LIMIT_DEPTH
    { uintptr_t depth = levelFrame(FR);

      if ( depth > depth_reached )
	depth_reached = depth;
      if ( depth > depth_limit )
      { DEBUG(2, Sdprintf("depth-limit\n"));

	if ( debugstatus.debugging )
	  newChoice(CHP_DEBUG, FR PASS_LD);
	FRAME_FAILED;
      }
    }
#endif

#if O_DEBUGGER
    if ( debugstatus.debugging )
    { DEF = getProcDefinedDefinition(&FR, NULL, DEF PASS_LD);
      FR->predicate = DEF;
      if ( false(DEF, FOREIGN) )
	FR->clause = DEF->definition.clauses;
      set(FR, FR_INBOX);

      switch(tracePort(FR, BFR, CALL_PORT, NULL PASS_LD))
      { case ACTION_FAIL:   FRAME_FAILED;
	case ACTION_IGNORE: VMI_GOTO(I_EXIT);
      }
    }
#endif /*O_DEBUGGER*/
  }

  PC = DEF->codes;
  assert(PC);
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_DEPART: implies it is the last subclause   of  the clause. This is the
entry point for last call optimisation.

(*) Handling undefined predicates here is very tricky because we need to
destroy de clause pointer before   leaveDefinition() to get asynchronous
atom-gc ok, but this destroys  the  environment   for  normal  GC if the
undefined predicate trapping code starts a GC. Therefore, undefined code
runs normal I_CALL. This isn't too  bad,   as  it only affects the first
call.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_DEPART, VIF_BREAK, 1, (CA1_PROC))
{ if ( (void *)BFR <= (void *)FR && truePrologFlag(PLFLAG_LASTCALL) )
  { Procedure proc = (Procedure) *PC++;

    if ( !proc->definition->definition.clauses &&	/* see (*) */
	 false(proc->definition, PROC_DEFINED) )
    { PC--;
      VMI_GOTO(I_CALL);
    }

    if ( true(FR, FR_WATCHED) )
    { LocalFrame lSave = lTop;
      lTop = (LocalFrame)ARGP;		/* just pushed arguments, so top */
      frameFinished(FR, FINISH_EXIT PASS_LD);
      lTop = lSave;
    }

    FR->clause = NULL;			/* for save atom-gc */
    leaveDefinition(DEF);
    DEF = proc->definition;
    if ( true(DEF, P_TRANSPARENT) )
    { FR->context = contextModule(FR);
      FR->flags = (((FR->flags+FR_LEVEL_STEP) | FR_CONTEXT) &
                   ~(FR_SKIPPED|FR_WATCHED|FR_CATCHED));
    } else
    { setNextFrameFlags(FR, FR);
    }
    if ( true(DEF, HIDE_CHILDS) )
      set(FR, FR_HIDE_CHILDS);

    FR->predicate = DEF;
    copyFrameArguments(lTop, FR, DEF->functor->arity PASS_LD);

    END_PROF();
    START_PROF(DEPART_CONTINUE, "DEPART_CONTINUE");
    goto depart_continue;
  }

  VMI_GOTO(I_CALL);
}


VMI(I_CALLM, VIF_BREAK, 2, (CA1_MODULE, CA1_PROC))
{ Module m = (Module)*PC++;
  Procedure proc = (Procedure)*PC++;
  DEF = getProcDefinedDefinition(&lTop, NULL, proc->definition PASS_LD);

  NFR = lTop;
  setNextFrameFlags(NFR, FR);
  if ( true(DEF, P_TRANSPARENT) )
    setContextModule(NFR, m);

  goto normal_call;
}


VMI(I_DEPARTM, VIF_BREAK, 2, (CA1_MODULE, CA1_PROC))
{ if ( (void *)BFR > (void *)FR || !truePrologFlag(PLFLAG_LASTCALL) )
  { VMI_GOTO(I_CALLM);
  } else
  { Module m = (Module)*PC++;

    setContextModule(FR, m);
    VMI_GOTO(I_DEPART);
  }
}


		 /*******************************
		 *	       EXIT		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Leave the clause:

  - update reference of current clause
    If there are no alternatives left and BFR  <=  frame  we  will
    never  return  at  this clause and can decrease the reference count.
    If BFR > frame the backtrack frame is a child of  this  frame,
    so  this frame can become active again and we might need to continue
    this clause.

  - update BFR
    `BFR' will become the backtrack frame of other childs  of  the
    parent  frame  in which we are going to continue.  If this frame has
    alternatives and is newer than the old backFrame `BFR'  should
    become this frame.

    If there are no alternatives and  the  BFR  is  this  one  the
    BFR can become this frame's backtrackframe.

  - Update `lTop'.
    lTop can be set to this frame if there are no alternatives  in  this
    frame  and  BFR  is  older  than this frame (e.g. there are no
    frames with alternatives that are newer).

  - restore machine registers from parent frame
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_EXIT: End of clauses for normal Prolog clauses.

TBD: Insert a layer in between, so   this  never has to handle call-back
from C.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_EXIT, VIF_BREAK, 0, ())
{ LocalFrame leave;

  if ( LD->alerted )
  {
#if O_DEBUGGER
    if ( debugstatus.debugging )
    { int action = tracePort(FR, BFR, EXIT_PORT, PC PASS_LD);

      switch( action )
      { case ACTION_RETRY:
	  goto retry;
	case ACTION_FAIL:
	  discardChoicesAfter(FR PASS_LD);
	  FRAME_FAILED;
      }

      if ( BFR && BFR->type == CHP_DEBUG && BFR->frame == FR )
	BFR = BFR->parent;
    }
#endif /*O_DEBUGGER*/
  }

  if ( (void *)BFR <= (void *)FR )	/* deterministic */
  { leave = true(FR, FR_WATCHED) ? FR : NULL;
    FR->clause = NULL;			/* leaveDefinition() destroys clause */
    leaveDefinition(DEF);		/* dynamic pred only */
    lTop = FR;
    DEBUG(3, Sdprintf("Deterministic exit of %s, lTop = #%ld\n",
		      predicateName(FR->predicate), loffset(lTop)));
  } else
  { leave = NULL;
    clear(FR, FR_INBOX);
  }

  PC = FR->programPointer;
  environment_frame = FR = FR->parent;
  DEF = FR->predicate;
  ARGP = argFrameP(lTop, 0);
  Profile(profExit(FR->prof_node PASS_LD));
  if ( leave )
    frameFinished(leave, FINISH_EXIT PASS_LD);

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_EXITFACT: generated to close a fact. The   reason for not generating a
plain I_EXIT is first of all that the actual sequence should be I_ENTER,
I_EXIT,  and  just  optimising   to    I_EXIT   looses   the  unify-port
interception. Second, there should be some room for optimisation here.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_EXITFACT, 0, 0, ())
{ if ( LD->alerted )
  {
#if O_DEBUGGER
    if ( debugstatus.debugging )
    { switch(tracePort(FR, BFR, UNIFY_PORT, PC PASS_LD))
      { case ACTION_RETRY:
	  goto retry;
      }
    }
#endif /*O_DEBUGGER*/
  exit_checking_wakeup:
#ifdef O_ATTVAR
    if ( LD->alerted & ALERT_WAKEUP )
    { LD->alerted &= ~ALERT_WAKEUP;

      if ( *valTermRef(LD->attvar.head) )
      { PC = SUPERVISOR(exit);
	goto wakeup;
      }
    }
#endif
  }

  VMI_GOTO(I_EXIT);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Created for the return of the toplevel query.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_EXITQUERY, 0, 0, ())
{ assert(!FR->parent);

  QF = QueryFromQid(qid);		/* may be shifted: recompute */
  QF->solutions++;

  assert(FR == &QF->top_frame);

  if ( BFR == &QF->choice )		/* No alternatives */
  { set(QF, PL_Q_DETERMINISTIC);
    lTop = (LocalFrame)argFrameP(FR, DEF->functor->arity);

    if ( true(FR, FR_WATCHED) )
      frameFinished(FR, FINISH_EXIT PASS_LD);
  }

#ifdef O_PROFILE
  if ( LD->profile.active )
  { LocalFrame parent = parentFrame(FR);

    if ( parent )
      profExit(parent->prof_node PASS_LD);
    else
      profExit(NULL PASS_LD);
  }
#endif
  QF->foreign_frame = PL_open_foreign_frame();

  succeed;
}



		 /*******************************
		 *	      CONTROL		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CUT: !. Task is to detroy  all   choicepoints  newer  then the current
frame. If we are in  debug-mode  we   create  a  new  CHP_DEBUG frame to
provide proper debugger output.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_CUT, VIF_BREAK, 0, ())
{
#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { Choice ch;
    mark m;

    switch(tracePort(FR, BFR, CUT_CALL_PORT, PC PASS_LD))
    { case ACTION_RETRY:
	goto retry;
      case ACTION_FAIL:
	FRAME_FAILED;
    }

    if ( (ch = findStartChoice(FR, BFR)) )
    { m = ch->mark;
      dbg_discardChoicesAfter(FR PASS_LD);
      lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
      ch = newChoice(CHP_DEBUG, FR PASS_LD);
      ch->mark = m;
    } else
    { dbg_discardChoicesAfter(FR PASS_LD);
      lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
    }
    ARGP = argFrameP(lTop, 0);
    if ( exception_term )
      goto b_throw;

    switch(tracePort(FR, BFR, CUT_EXIT_PORT, PC PASS_LD))
    { case ACTION_RETRY:
	goto retry;
      case ACTION_FAIL:
	FRAME_FAILED;
    }
  } else
#endif
  { discardChoicesAfter(FR PASS_LD);
    lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
    ARGP = argFrameP(lTop, 0);
    if ( exception_term )
      goto b_throw;
  }

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_JMP skips the amount stated in the pointed argument. The PC++ could be
compiled out, but this is a bit more neath.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_JMP, 0, 1, (CA1_JUMP))
{ PC += *PC;
  PC++;

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_OR: Create choice-point in the clause.  Argument is the amount to skip
if the choice-point needs to be activated.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_OR, 0, 1, (CA1_JUMP))
{ size_t skip = *PC++;
  Choice ch = newChoice(CHP_JUMP, FR PASS_LD);
  ch->value.PC = PC+skip;
  ARGP = argFrameP(lTop, 0);

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_IFTHEN saves the value of BFR  (current   backtrack  frame) into a local
frame slot reserved by the compiler.  Note that the variable to hold the
local-frame pointer is  *not*  reserved   in  clause->variables,  so the
garbage collector won't see it.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_IFTHEN, 0, 1, (CA1_CHP))
{ varFrame(FR, *PC++) = (word) BFR;

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_IFTHENELSE: contraction of C_IFTHEN and C_OR.  This contraction has been
made to help the decompiler distinguis between (a ; b) -> c and a -> b ;
c,  which  would  otherwise  only  be    possible  to  distinguis  using
look-ahead.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_NOT, 0, 2, (CA1_CHP,CA1_JUMP))
{ SEPERATE_VMI;
  VMI_GOTO(C_IFTHENELSE);
}


VMI(C_IFTHENELSE, 0, 2, (CA1_CHP,CA1_JUMP))
{ varFrame(FR, *PC++) = (word) BFR; /* == C_IFTHEN */

  VMI_GOTO(C_OR);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_VAR is generated by the compiler to ensure the  instantiation  pattern
of  the  variables  is  the  same after finishing both paths of the `or'
wired in the clause.  Its task is to make the n-th variable slot of  the
current frame to be a variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_VAR, 0, 1, (CA1_VAR))
{ setVar(varFrame(FR, *PC++));

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_CUT will  destroy  all  backtrack  points  created  after  the  C_IFTHEN
instruction in this clause.  It assumes the value of BFR has been stored
in the nth-variable slot of the current local frame.

We can dereference all frames that are older that the old backtrackframe
and older than this frame.

All frames created since what becomes now the  backtrack  point  can  be
discarded.

C_LCUT  results  from  !'s  encountered  in    the   condition  part  of
if->then;else and \+ (which  is  (g->fail;true)).   It  should  cut  all
choices created since the mark, but not   the mark itself. The test-case
is  a  :-  \+  (b,  !,  fail),    which   should  succeed.  The  current
implementation  walks  twice  over  the    choice-points,  but  cuts  in
conditions should be rare (I hope :-).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
BEGIN_SHAREDVARS
  Choice och;
  LocalFrame fr;
  Choice ch;

VMI(C_LCUT, 0, 1, (CA1_CHP))
{ och = (Choice) varFrame(FR, *PC);
  PC++;

  for(ch=BFR; ch; ch = ch->parent)
  { if ( ch->parent == och )
    { och = ch;
      goto c_cut;
    }
  }
  assert(BFR == och);			/* no choicepoint yet */
  NEXT_INSTRUCTION;
}

VMI(C_CUT, 0, 1, (CA1_CHP))
{ och = (Choice) varFrame(FR, *PC);
  PC++;					/* cannot be in macro! */
c_cut:
  if ( !och || FR > och->frame )	/* most recent frame to keep */
    fr = FR;
  else
    fr = och->frame;

  for(ch=BFR; ch && ch > och; ch = ch->parent)
  { LocalFrame fr2;

    DEBUG(3, Sdprintf("Discarding %s\n", chp_chars(ch)));
    for(fr2 = ch->frame;
	fr2 && fr2->clause && fr2 > fr;
	fr2 = fr2->parent)
    { discardFrame(fr2, FINISH_CUT PASS_LD);
      if ( exception_term )
	goto b_throw;
    }
  }
  assert(och == ch);
  BFR = och;

  if ( (void *)och > (void *)fr )
  { lTop = addPointer(och, sizeof(*och));
  } else
  { int nvar = (true(fr->predicate, FOREIGN)
			? fr->predicate->functor->arity
			: fr->clause->clause->variables);
    lTop = (LocalFrame) argFrameP(fr, nvar);
  }

  ARGP = argFrameP(lTop, 0);

  DEBUG(3, Sdprintf(" --> BFR = #%ld, lTop = #%ld\n",
		    loffset(BFR), loffset(lTop)));
  NEXT_INSTRUCTION;
}
END_SHAREDVARS


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_SOFTIF: A *-> B ; C is translated to C_SOFIF <A> C_SOFTCUT <B> C_JMP
end <C>. See pl-comp.c and C_SOFTCUT implementation for details.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
VMI(C_SOFTIF, 0, 2, (CA1_CHP,CA1_JUMP))
{ varFrame(FR, *PC++) = (word) lTop; /* see C_SOFTCUT */

  VMI_GOTO(C_OR);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_SOFTCUT: Handle the commit-to of A *-> B; C. Simply invalidate the
choicepoint.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
VMI(C_SOFTCUT, 0, 1, (CA1_CHP))
{ Choice ch = (Choice) varFrame(FR, *PC);
  Choice bfr = BFR;

  PC++;
  if ( bfr == ch )
  { BFR = bfr->parent;
  } else
  { for(; bfr; bfr=bfr->parent)
    { if ( bfr->parent == ch )
      { bfr->parent = ch->parent;
	break;
      }
    }
  }
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_END is a dummy instruction to help the decompiler to find the end of A
-> B. (Note that a :- (b ->  c),  d  ==   a  :-  (b  ->  c, d) as far as
semantics. They are different terms however.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_END, 0, 0, ())
{ NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_FAIL is equivalent to fail/0. Used to implement \+/1.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(C_FAIL, 0, 0, ())
{ BODY_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_FAIL: Translation of fail/0. Same as C_FAIL, but we make a normal call
when in debug-mode, so we can trace the call.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_FAIL, VIF_BREAK, 0, ())
{
#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { NFR = lTop;
    setNextFrameFlags(NFR, FR);
    DEF = lookupProcedure(FUNCTOR_fail0, MODULE_system)->definition;

    goto normal_call;
  }
#endif
  BODY_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_TRUE: Translation of true/0.  See also I_FAIL.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_TRUE, VIF_BREAK, 0, ())
{
#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { NFR = lTop;
    setNextFrameFlags(NFR, FR);
    DEF = lookupProcedure(FUNCTOR_true0, MODULE_system)->definition;

    goto normal_call;
  }
#endif
  NEXT_INSTRUCTION;
}


		 /*******************************
		 *	    SUPERVISORS		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_VIRGIN: Fresh, unused predicate. Any new   predicate  is created using
this supervisor (see resetProcedure()). The task of this is to

	* Resolve the definition (i.e. auto-import or auto-load if
	not defined).
	* Check the indexing opportunities and install the proper
        supervisor (see reindexDefinition()).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_VIRGIN, 0, 0, ())
{ DEF = getProcDefinedDefinition(&FR, NULL, DEF PASS_LD);

  if ( FR->predicate != DEF )		/* auto imported/loaded */
  { FR->predicate = DEF;
    if ( FR->prof_node )
      profSetHandle(FR->prof_node, DEF);
    goto retry_continue;
  }

  if ( createSupervisor(DEF) )
  { PC = DEF->codes;
    NEXT_INSTRUCTION;
  } else				/* TBD: temporary */
  { assert(0);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_UNDEF: Undefined predicate. We could make   two  instructions, one for
trapping and one not, but the disadvantage of that is that switching the
unknown flag of the module has no   immediate consequences. Hence we use
one instruction and dynamic checking.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_UNDEF, 0, 0, ())
{ if ( true(DEF->module, UNKNOWN_ERROR) )
  { fid_t fid;
    Definition caller;

    lTop = (LocalFrame)argFrameP(FR, DEF->functor->arity);
    fid = PL_open_foreign_frame();
    if ( FR->parent )
      caller = FR->parent->predicate;
    else
      caller = NULL;

    PL_error(NULL, 0, NULL, ERR_UNDEFINED_PROC, DEF, caller);
    PL_close_foreign_frame(fid);
    enterDefinition(DEF);		/* will be left in exception code */

    goto b_throw;
  }

  FRAME_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Generic calling code.  This needs to be specialised.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_STATIC, 0, 0, ())
{ ClauseRef nextcl;
  ARGP = argFrameP(FR, 0);
  lTop = (LocalFrame)ARGP+DEF->functor->arity;

  DEBUG(9, Sdprintf("Searching clause ... "));

  if ( !(CL = firstClause(ARGP, FR, DEF, &nextcl PASS_LD)) )
  { DEBUG(9, Sdprintf("No clause matching index.\n"));
    if ( debugstatus.debugging )
      newChoice(CHP_DEBUG, FR PASS_LD);

    FRAME_FAILED;
  }
  DEBUG(9, Sdprintf("Clauses found.\n"));

  PC = CL->clause->codes;
  lTop = (LocalFrame)(ARGP + CL->clause->variables);
  requireStack(local, (size_t)argFrameP((LocalFrame)NULL, MAXARITY));

  if ( nextcl )
  { Choice ch = newChoice(CHP_CLAUSE, FR PASS_LD);
    ch->value.clause = nextcl;
  } else if ( debugstatus.debugging )
    newChoice(CHP_DEBUG, FR PASS_LD);

  SECURE(
  int argc; int n;
  argc = DEF->functor->arity;
  for(n=0; n<argc; n++)
    checkData(argFrameP(FR, n));
  );

  umode = uread;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_DYNAMIC: Dynamic predicate. Dynamic predicates   must  use the dynamic
indexing and need to lock the predicate. This VMI can also handle static
code.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_DYNAMIC, 0, 0, ())
{ enterDefinition(DEF);

  VMI_GOTO(S_STATIC);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_THREAD_LOCAL: Get thread-local definition
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_THREAD_LOCAL, 0, 0, ())
{ FR->predicate = DEF = getProcDefinition__LD(DEF PASS_LD);

  assert(DEF->codes);
  PC = DEF->codes;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_MULTIFILE: Multifile predicate.  These need to be aware of new
clauses that can be added at runtime.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_MULTIFILE, 0, 0, ())
{ VMI_GOTO(S_STATIC);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_TRUSTME: Trust this clause. Generated   for  single-clause supervisors
and for the last one of a disjunction.

TBD: get rid of clause-references
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_TRUSTME, 0, 1, (CA1_CLAUSEREF))
{ ClauseRef cref = (ClauseRef)*PC++;

  ARGP = argFrameP(FR, 0);
  TRUST_CLAUSE(cref);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_ALLCLAUSES: Simply try the clauses one-by-one. This works for all code
and is the ultimate fallback of the indexing code.  The supervisor code
is

	S_ALLCLAUSES
	S_NEXTCLAUSE
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

BEGIN_SHAREDVARS
ClauseRef cref;

VMI(S_ALLCLAUSES, 0, 0, ())
{ cref = DEF->definition.clauses;

next_clause:
  ARGP = argFrameP(FR, 0);
  for(; cref; cref = cref->next)
  { if ( visibleClause(cref->clause, FR->generation) )
    { TRY_CLAUSE(cref, cref->next, PC);
    }
  }

  FRAME_FAILED;
}


VMI(S_NEXTCLAUSE, 0, 0, ())
{ cref = CL->next;

  if ( debugstatus.debugging && !debugstatus.suspendTrace )
  { ARGP = argFrameP(FR, 0);
    lTop = (LocalFrame)ARGP + FR->predicate->functor->arity;

    for(; cref; cref = cref->next)
    { if ( visibleClause(cref->clause, FR->generation) )
      {	LocalFrame fr;
	CL = cref;

	if ( (fr = dbgRedoFrame(FR PASS_LD)) )
	{ switch( tracePort(fr, BFR, REDO_PORT, NULL PASS_LD) )
	  { case ACTION_FAIL:
	      FRAME_FAILED;
	    case ACTION_IGNORE:
	      VMI_GOTO(I_EXIT);
	    case ACTION_RETRY:
	      goto retry_continue;
	  }
	}

        break;
      }
    }
  }

  PC--;
  goto next_clause;
}
END_SHAREDVARS


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
S_LIST: Predicate consisting of two clauses, one of them using [] and
the other [_|_].
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_LIST, 0, 2, (CA1_CLAUSEREF, CA1_CLAUSEREF))
{ ClauseRef cref;
  Word k;

  ARGP = argFrameP(FR, 0);
  deRef2(ARGP, k);
  if ( isList(*k) )
    cref = (ClauseRef)PC[1];
  else if ( isNil(*k) )
    cref = (ClauseRef)PC[0];
  else if ( canBind(*k) )
  { PC = SUPERVISOR(next_clause);
    VMI_GOTO(S_ALLCLAUSES);
  } else
    FRAME_FAILED;

  PC += 2;

  TRUST_CLAUSE(cref);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Meta-predicate  argument  qualification.  S_MQUAL    qualifies  the  Nth
argument. S_LMQUAL does the same and resets   the  context module of the
frame to be definition module of   the  predicate, such that unqualified
calls refer again to  the  definition   module.  This  sequence  must be
processed as part of the supervisor,   notably before creating the first
choicepoint.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(S_MQUAL, 0, 1, (CA1_VAR))
{ Word k = varFrameP(FR, (int)*PC++);

  m_qualify_argument(FR, k PASS_LD);

  NEXT_INSTRUCTION;
}


VMI(S_LMQUAL, 0, 1, (CA1_VAR))
{ Word k = varFrameP(FR, (int)*PC++);

  m_qualify_argument(FR, k PASS_LD);
  setContextModule(FR, FR->predicate->module);

  NEXT_INSTRUCTION;
}


		 /*******************************
		 *	    ARITHMETIC		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Arithmic is compiled using a  stack  machine.    ARGP  is  used as stack
pointer and the arithmic stack is allocated   on top of the local stack,
starting at the argument field of the next slot of the stack (where ARGP
points to when processing the body anyway).

Arguments to functions are pushed on the stack  starting  at  the  left,
thus `add1(X, Y) :- Y is X + 1' translates to:

  I_ENTER	% enter body
  B_VAR1	% push Y via ARGP
  A_ENTER	% align the stack to prepare for writing doubles
  A_VAR0	% evaluate X and push numeric result
  A_INTEGER 1	% Push 1 as numeric value
  A_FUNC2 0	% Add top-two of the stack and push result
  A_IS 		% unify Y with numeric result
  I_EXIT	% leave the clause

  a_func0:	% executes arithmic function without arguments, pushing
	        % its value on the stack
  a_func1:	% unary function. Changes the top of the stack.
  a_func2:	% binary function. Pops two values and pushes one.

Note that we do not call `ar_func0(*PC++, &ARGP)' as ARGP is a register
variable.  Also, for compilers that do register allocation it is unwise
to give the compiler a hint to put ARGP not into a register.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_ENTER: Ensure the alignment of ARGP   allows  for efficient pushing of
the number structure.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_ENTER, 0, 0, ())
{ AR_BEGIN();
  NEXT_INSTRUCTION;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_INTEGER: Push long integer following PC
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_INTEGER, 0, 1, (CA1_INTEGER))
{ Number n = allocArithStack(PASS_LD1);

  n->value.i = (intptr_t) *PC++;
  n->type    = V_INTEGER;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_INT64: Push int64_t following PC
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_INT64, 0, WORDS_PER_INT64, (CA1_INT64))
{ Number n = allocArithStack(PASS_LD1);
  Word p = &n->value.w[0];

  cpInt64Data(p, PC);
  n->type    = V_INTEGER;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_MPZ: Push mpz integer following PC
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_MPZ, 0, VM_DYNARGC, (CA1_MPZ))
{
#ifdef O_GMP
  Number n = allocArithStack(PASS_LD1);
  Word p = (Word)PC;
  int size;

  p++;				/* skip indirect header */
  n->type = V_MPZ;
  n->value.mpz->_mp_size  = (int)*p++;
  n->value.mpz->_mp_alloc = 0;	/* avoid de-allocating */
  size = sizeof(mp_limb_t) * abs(n->value.mpz->_mp_size);
  n->value.mpz->_mp_d = (void*)p;

  p += (size+sizeof(word)-1)/sizeof(word);
  PC = (Code)p;
#endif
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_DOUBLE: Push double following PC
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_DOUBLE, 0, WORDS_PER_DOUBLE, (CA1_FLOAT))
{ Number n = allocArithStack(PASS_LD1);
  Word p = &n->value.w[0];

  cpDoubleData(p, PC);
  n->type       = V_FLOAT;
  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_VAR: Push a variable.  This can be any term
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

BEGIN_SHAREDVARS
  int offset;

VMI(A_VAR, 0, 1, (CA1_VAR))
{ Number n;
  Word p, p2;

  offset = (int)*PC++;

a_var_n:
  p = varFrameP(FR, offset);
  deRef2(p, p2);

					/* speedup common case a bit */
  if ( tagex(*p2) == (TAG_INTEGER|STG_INLINE) )
  { n = allocArithStack(PASS_LD1);
    n->value.i = valInt(*p2);
    n->type = V_INTEGER;
    NEXT_INSTRUCTION;
    /*NOTREACHED*/
  }

  switch(tag(*p2))
  { case TAG_INTEGER:
      n = allocArithStack(PASS_LD1);
      get_integer(*p2, n);
      NEXT_INSTRUCTION;
    case TAG_FLOAT:
      n = allocArithStack(PASS_LD1);
      n->value.f = valFloat(*p2);
      n->type = V_FLOAT;
      NEXT_INSTRUCTION;
    default:
    { intptr_t lsafe = (char*)lTop - (char*)lBase;
      fid_t fid;
      number result;
      int rc;

      lTop = (LocalFrame)argFrameP(lTop, 1); /* for is/2.  See below */
      fid = PL_open_foreign_frame();
      rc = valueExpression(consTermRef(p), &result PASS_LD);
      PL_close_foreign_frame(fid);
      lTop = addPointer(lBase, lsafe);

      if ( rc )
      { pushArithStack(&result PASS_LD);
	NEXT_INSTRUCTION;
      } else
      { resetArithStack(PASS_LD1);
	goto b_throw;
      }
    }
  }
}

VMI(A_VAR0, 0, 0, ())
{ offset = VAROFFSET(0);
  goto a_var_n;
}

VMI(A_VAR1, 0, 0, ())
{ offset = VAROFFSET(1);
  goto a_var_n;
}

VMI(A_VAR2, 0, 0, ())
{ offset = VAROFFSET(2);
  goto a_var_n;
}
END_SHAREDVARS



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_FUNC, function, #args
A_FUNC0, function
A_FUNC1, function
A_FUNC2, function

TBD: Keep knowledge on #argument in function!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

BEGIN_SHAREDVARS
  int an;
  code fn;

VMI(A_FUNC0, 0, 1, (CA1_AFUNC))
{ an = 0;
  fn = *PC++;
  goto common_an;
}

VMI(A_FUNC1, 0, 1, (CA1_AFUNC))
{ an = 1;
  fn = *PC++;
  goto common_an;
}

VMI(A_FUNC2, 0, 1, (CA1_AFUNC))
{ an = 2;
  fn = *PC++;
  goto common_an;
}

VMI(A_FUNC, 0, 2, (CA1_AFUNC, CA1_INTEGER))
{ fn = *PC++;
  an = (int) *PC++;

common_an:
  if ( !ar_func_n((int)fn, an PASS_LD) )
  { resetArithStack(PASS_LD1);
    goto b_throw;
  }

  NEXT_INSTRUCTION;
}
END_SHAREDVARS


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_ADD: Shorthand for A_FUNC2 pl_ar_add()
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_ADD, 0, 0, ())
{ Number argv = argvArithStack(2 PASS_LD);
  int rc;
  number r;

  rc = pl_ar_add(argv, argv+1, &r);
  popArgvArithStack(2 PASS_LD);
  if ( rc )
  { pushArithStack(&r PASS_LD);
    NEXT_INSTRUCTION;
  }

  resetArithStack(PASS_LD1);
  goto b_throw;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_MUL: Shorthand for A_FUNC2 ar_mul()
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_MUL, 0, 0, ())
{ Number argv = argvArithStack(2 PASS_LD);
  int rc;
  number r;

  rc = ar_mul(argv, argv+1, &r);
  popArgvArithStack(2 PASS_LD);
  if ( rc )
  { pushArithStack(&r PASS_LD);
    NEXT_INSTRUCTION;
  }

  resetArithStack(PASS_LD1);
  goto b_throw;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_ADD_FC: Simple case A is B + <int>, where   A is a firstvar and B is a
normal variable. This case is very   common,  especially with relatively
small integers.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_ADD_FC, VIF_BREAK, 3, (CA1_VAR, CA1_VAR, CA1_INTEGER))
{ Word rp  = varFrameP(FR, *PC++);
  Word np  = varFrameP(FR, *PC++);
  intptr_t add = (intptr_t)*PC++;

  deRef(np);

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { Word expr = allocGlobal(3);

    expr[0] = FUNCTOR_plus2;
    expr[1] = linkVal(np);
    expr[2] = makeNum(add);

    ARGP = argFrameP(lTop, 0);
    setVar(*rp);
    *ARGP++ = linkVal(rp);
    *ARGP++ = consPtr(expr, TAG_COMPOUND|STG_GLOBAL);
    NFR = lTop;
    DEF = GD->procedures.is2->definition;
    setNextFrameFlags(NFR, FR);
    goto normal_call;
  }
#endif

  if ( tagex(*np) == (TAG_INTEGER|STG_INLINE) )
  { intptr_t v = valInt(*np);
    int64_t r = v+add;			/* tagged ints never overflow */

    *rp = makeNum(r);
    NEXT_INSTRUCTION;
  } else
  { number n;
    fid_t fid;
    int rc;

    fid = PL_open_foreign_frame();
    rc = valueExpression(wordToTermRef(np), &n PASS_LD);
    PL_close_foreign_frame(fid);
    if ( !rc )
      goto b_throw;

    ensureWritableNumber(&n);
    if ( ar_add_ui(&n, add) )
    { *rp = put_number(&n);
      clearNumber(&n);

      NEXT_INSTRUCTION;
    } else
    { clearNumber(&n);
    }

    goto b_throw;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Translation of the arithmic comparison predicates (<, >, =<,  >=,  =:=).
Both sides are pushed on the stack, so we just compare the two values on
the  top  of  this  stack  and  backtrack  if  they  do  not suffice the
condition.  Example translation: `a(Y) :- b(X), X > Y'

	I_ENTER
	B_FIRSTVAR 1	% Link X from B's frame to a new var in A's frame
	I_CALL b/1	% call b/1
	A_ENTER		% Enter arithmetic mode
	A_VAR 1		% Push X
	A_VAR 0		% Push Y
	A_GT		% compare
	EXIT
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

BEGIN_SHAREDVARS
  Number n1, n2;
  int cmp;
  int rc;

#define CMP_FAST(op) \
  n1 = argvArithStack(2 PASS_LD); \
  n2 = n1 + 1; \
  if ( n1->type == n2->type ) \
  { switch(n1->type) \
    { case V_INTEGER: \
        rc = n1->value.i op n2->value.i; \
	goto a_cmp_out; \
      case V_FLOAT: \
        rc = n1->value.f op n2->value.f; \
	goto a_cmp_out; \
      default: \
        ; \
    } \
  }


VMI(A_LT, VIF_BREAK, 0, ())		/* A < B */
{ CMP_FAST(<)
  cmp = LT;
acmp:
  rc = ar_compare(n1, n2, cmp);
a_cmp_out:
  popArgvArithStack(2 PASS_LD);
  AR_END();
  if ( rc )
    NEXT_INSTRUCTION;
  BODY_FAILED;
}

VMI(A_LE, VIF_BREAK, 0, ())		/* A =< B */
{ CMP_FAST(<=);
  cmp = LE;
  goto acmp;
}

VMI(A_GT, VIF_BREAK, 0, ())		/* A > B */
{ CMP_FAST(>);
  cmp = GT;
  goto acmp;
}

VMI(A_GE, VIF_BREAK, 0, ())		/* A >= B */
{ CMP_FAST(>=);
  cmp = GE;
  goto acmp;
}

VMI(A_EQ, VIF_BREAK, 0, ())		/* A =:= B */
{ CMP_FAST(==);
  cmp = EQ;
  goto acmp;
}

VMI(A_NE, VIF_BREAK, 0, ())		/* A \=:= B */
{ CMP_FAST(!=);
  cmp = NE;
  goto acmp;
}
END_SHAREDVARS


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_IS: Translation of is/2. The stack has two pushed values: the variable
for the result (a word) and the number holding the result. For example:

   a(X) :- X is sin(3).

  I_ENTER
  B_VAR 0		push left argument of is/2
  A_INTEGER 3		push integer as number
  A_FUNC <sin>		run function on it
  A_IS			bind value
  I_EXIT
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_IS, VIF_BREAK, 0, ())		/* A is B */
{ Number n = argvArithStack(1 PASS_LD);
  Word k;

  ARGP = argFrameP(lTop, 0);	/* 1-st argument */
  deRef2(ARGP, k);

  if ( canBind(*k) )
  { word c = put_number(n);	/* can shift */

    popArgvArithStack(1 PASS_LD);
    AR_END();
#ifdef O_SHIFT_STACKS
    ARGP = argFrameP(lTop, 0);
#endif
    deRef2(ARGP, k);
    bindConst(k, c);
    CHECK_WAKEUP;
    NEXT_INSTRUCTION;
  } else
  { int rc;

    if ( isInteger(*k) && intNumber(n) )
    { number left;

      get_integer(*k, &left);
      rc = (cmpNumbers(&left, n) == 0);
      clearNumber(&left);
    } else if ( isFloat(*k) && floatNumber(n) )
    { rc = (valFloat(*k) == n->value.f);
    } else
    { rc = FALSE;
    }

    popArgvArithStack(1 PASS_LD);
    AR_END();
    if ( rc )
      NEXT_INSTRUCTION;
  }

  BODY_FAILED;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A_FIRSTVAR_IS: Deal with the very common case that the local variable is
a firstvar of the current frame. There   are numerous advantages to this
case: we know the left-side is a var, we  do not need to trail and we do
not need to check for attvar wakeup.

TBD: link with following B_VAR? How  frequent?   Likely  very: we are in
body mode and in many cases the result is used only once.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(A_FIRSTVAR_IS, VIF_BREAK, 1, (CA1_VAR)) /* A is B */
{ Word k = varFrameP(FR, *PC++);
  Number n = argvArithStack(1 PASS_LD);

  *k = put_number(n);
  popArgvArithStack(1 PASS_LD);
  AR_END();

  NEXT_INSTRUCTION;
}


		 /*******************************
		 *	     FOREIGN		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	I_FOPEN
	I_FCALLDET0-10 f/n
	I_FEXITDET
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef O_PROF_PENTIUM
#define PROF_FOREIGN \
	{ END_PROF(); \
	  START_PROF(DEF->prof_index, DEF->prof_name); \
	}
#else
#define PROF_FOREIGN (void)0
#endif

BEGIN_SHAREDVARS
volatile word rc;			/* make gcc quiet on non-initialised */
volatile fid_t ffr_id;

VMI(I_FOPEN, 0, 0, ())
{ FliFrame ffr;

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { lTop = (LocalFrame)argFrameP(FR, DEF->functor->arity);
    BFR = newChoice(CHP_DEBUG, FR PASS_LD);
    ffr = (FliFrame)lTop;
  } else
#endif
  { ffr = (FliFrame)argFrameP(FR, DEF->functor->arity);
  }

  lTop = (LocalFrame)(ffr+1);
  ffr->size = 0;
  Mark(ffr->mark);
  ffr->parent = fli_context;
  ffr->magic = FLI_MAGIC;
  fli_context = ffr;
  ffr_id = consTermRef(ffr);
  SAVE_REGISTERS(qid);

  NEXT_INSTRUCTION;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_FCALLDETVA:  Call  deterministic  foreign    function  using  P_VARARG
conventions.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_FCALLDETVA, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  struct foreign_context context;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  context.context = 0L;
  context.engine  = LD;
  context.control = FRG_FIRST_CALL;

  PROF_FOREIGN;
  rc = (*f)(h0, DEF->functor->arity, &context);
  VMI_GOTO(I_FEXITDET);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_FCALLDET0 .. I_FCALLDET10: Call deterministic   foreign function using
a1, a2, ... calling conventions.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_FCALLDET0, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;

  PROF_FOREIGN;
  rc = (*f)();
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET1, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET2, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET3, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET4, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET5, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET6, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET7, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET8, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, h0+7);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET9, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, h0+7, h0+8);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FCALLDET10, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, h0+7, h0+8, h0+9);
  VMI_GOTO(I_FEXITDET);
}


VMI(I_FEXITDET, 0, 0, ())
{ FliFrame ffr = (FliFrame)valTermRef(ffr_id);

  LOAD_REGISTERS(qid);
  fli_context = ffr->parent;

  switch(rc)
  { case TRUE:
      if ( exception_term )		/* false alarm */
      { exception_term = 0;
	setVar(*valTermRef(exception_bin));
      }
      goto exit_checking_wakeup;
    case FALSE:
      if ( exception_term )
	goto b_throw;
      FRAME_FAILED;
    default:
    { fid_t fid = PL_open_foreign_frame();
      term_t ex = PL_new_term_ref();

      PL_put_intptr(ex, rc);
      PL_error(NULL, 0, NULL, ERR_DOMAIN,
	       ATOM_foreign_return_value, ex);
      PL_close_foreign_frame(fid);
      goto b_throw;
    }
  }
}
END_SHAREDVARS

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Non-deterministic foreign calls. This  is   compiled  into the following
supervisor code:

	I_FOPENNDET
	I_FCALLNDETVA func
	I_FEXITNDET
	I_FREDO

On determistic success or failure I_FEXITNDET ends the clause. Otherwise
it writes the context in CL  and   creates  a jump-choicepoint that will
take it to I_FREDO. I_FREDO updates the context structure and jumps back
to the I_FCALLNDETVA (PC -= 4);
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

BEGIN_SHAREDVARS
volatile intptr_t rc;				/* make compiler happy */
struct foreign_context context;
volatile fid_t ffr_id;

VMI(I_FOPENNDET, 0, 0, ())
{ Choice ch;
  FliFrame ffr;

  context.context = 0L;
  context.engine  = LD;
  context.control = FRG_FIRST_CALL;

foreign_redo:
  lTop = (LocalFrame)argFrameP(FR, DEF->functor->arity);
  ch = newChoice(CHP_JUMP, FR PASS_LD);
  ch->value.PC = PC+3;

  ffr = (FliFrame)(ch+1);
  lTop = (LocalFrame)(ffr+1);
  ffr->size = 0;
  Mark(ffr->mark);
  ffr->parent = fli_context;
  ffr->magic = FLI_MAGIC;
  fli_context = ffr;
  ffr_id = consTermRef(ffr);
  SAVE_REGISTERS(qid);

  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDETVA, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, DEF->functor->arity, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET0, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;

  PROF_FOREIGN;
  rc = (*f)(&context);
  NEXT_INSTRUCTION;
}

VMI(I_FCALLNDET1, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET2, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  rc = (*f)(h0, h0+1, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET3, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET4, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET5, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET6, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET7, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET8, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, h0+7, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET9, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, h0+7, h0+8, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FCALLNDET10, 0, 1, (CA1_FOREIGN))
{ Func f = (Func)*PC++;
  term_t h0 = argFrameP(FR, 0) - (Word)lBase;

  PROF_FOREIGN;
  rc = (*f)(h0, h0+1, h0+2, h0+3, h0+4, h0+5, h0+6, h0+7, h0+8, h0+9, &context);
  NEXT_INSTRUCTION;
}


VMI(I_FEXITNDET, 0, 0, ())
{ FliFrame ffr = (FliFrame) valTermRef(ffr_id);

  LOAD_REGISTERS(qid);
  fli_context = ffr->parent;

  switch(rc)
  { case TRUE:
      if ( exception_term )		/* false alarm */
      { exception_term = 0;
	setVar(*valTermRef(exception_bin));
      }
      assert(BFR->value.PC == PC);
      BFR = BFR->parent;
      goto exit_checking_wakeup;
    case FALSE:
      if ( exception_term )
	goto b_throw;
      assert(BFR->value.PC == PC);
      BFR = BFR->parent;
      FRAME_FAILED;
    default:
    { /* TBD: call debugger */

      if ( (rc & FRG_REDO_MASK) == REDO_INT )
      { rc = (word)(((intptr_t)rc)>>FRG_REDO_BITS);
      } else
      { rc &= ~FRG_REDO_MASK;
      }
      CL = (ClauseRef)rc;
      lTop = (LocalFrame)(BFR+1);
      goto exit_checking_wakeup;
    }
  }
}

VMI(I_FREDO, 0, 0, ())
{ if ( is_signalled(PASS_LD1) )
  { SAVE_REGISTERS(qid);
    handleSignals(NULL);
    LOAD_REGISTERS(qid);
    if ( exception_term )
      goto b_throw;
  }

  context.context = (word)FR->clause;
  context.control = FRG_REDO;
  context.engine  = LD;
  PC -= 4;
  goto foreign_redo;
}

END_SHAREDVARS


		 /*******************************
		 *     EXCEPTIONS & CLEANUP	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CALLCLEANUP: Part of call_cleanup(:Goal, :Cleanup).  Simply set a flag
on the frame and call the 1-st argument. See also I_CATCH.

I_EXITCLEANUP  is  at  the  end  of   call_cleanup/3.  If  there  is  no
choice-point created this is the final exit. If this frame has no parent
(it is the entry of PL_next_solution()),

call_cleanup(:Goal, -Reason, :Cleanup) is tranalated into
   0 i_enter
   1 i_callcleanup
   2 i_exitcleanup
   3 i_exit

We set FR_WATCHED to get a cleanup call if the frame fails or is cutted.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_CALLCLEANUP, 0, 0, ())
{ newChoice(CHP_CATCH, FR PASS_LD);
  set(FR, FR_WATCHED);
				/* = B_VAR1 */
  *argFrameP(lTop, 0) = linkVal(argFrameP(FR, 1));

  VMI_GOTO(I_USERCALL0);
}


VMI(I_EXITCLEANUP, 0, 0, ())
{ while( BFR && BFR->type == CHP_DEBUG )
    BFR = BFR->parent;

  if ( BFR->frame == FR && BFR->type == CHP_CATCH )
  { DEBUG(3, Sdprintf(" --> BFR = #%ld\n", loffset(BFR->parent)));
    for(BFR = BFR->parent; BFR > (Choice)FR; BFR = BFR->parent)
    { if ( BFR->frame > FR )
	NEXT_INSTRUCTION;		/* choice from setup of setup_call_catcher_cleanup/4 */
      assert(BFR->type == CHP_DEBUG);
    }

    frameFinished(FR, FINISH_EXITCLEANUP PASS_LD);
    if ( exception_term )
      goto b_throw;
  }

  NEXT_INSTRUCTION;			/* goto i_exit? */
}


#if O_CATCHTHROW
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CATCH has to fake a choice-point to   make  it possible to undo before
starting the recover action. Otherwise it simply   has to call the first
argument.  Catch is defined as:

catch(Goal, Pattern, Recover) :-
  $catch.

which is translated to:
  I_ENTER
  I_CATCH
  I_EXITCATCH
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_CATCH, 0, 0, ())
{ if ( BFR->frame == FR && BFR == (Choice)argFrameP(FR, 3) )
  { assert(BFR->type == CHP_DEBUG);
    BFR->type = CHP_CATCH;
  } else
    newChoice(CHP_CATCH, FR PASS_LD);

				  /* = B_VAR0 */
  *argFrameP(lTop, 0) = linkVal(argFrameP(FR, 0));
  VMI_GOTO(I_USERCALL0);
}


VMI(I_EXITCATCH, VIF_BREAK, 0, ())
{ if ( BFR->frame == FR && BFR == (Choice)argFrameP(FR, 3) )
  { assert(BFR->type == CHP_CATCH);
    BFR = BFR->parent;
  }

  VMI_GOTO(I_EXIT);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The B_THROW code is the implementation for   throw/1.  The call walks up
the stack, looking for a frame running catch/3 on which it can unify the
exception code. It then cuts all  choicepoints created since throw/3. If
throw/3 is not found, it sets  the   query  exception  field and returns
failure. Otherwise, it will simulate an I_USERCALL0 instruction: it sets
the FR and lTop as it it  was   running  the  throw/3 predicate. Then it
pushes the recovery goal from throw/3 and jumps to I_USERCALL0.

Note that exceptions are placed on the stack using PL_raise_exception(),
which use duplicate_term() and freezeGlobal() to make the exception term
immune for undo operations.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_THROW, 0, 0, ())
{ Word catcher;
  word except;
  LocalFrame catchfr;

  PL_raise_exception(argFrameP(lTop, 0) - (Word)lBase);
b_throw:
  assert(exception_term);
  catcher = valTermRef(exception_term);
  deRef(catcher);

  SECURE(checkData(catcher));
  DEBUG(1, { fid_t fid = PL_open_foreign_frame();
	     Sdprintf("[%d] Throwing ", PL_thread_self());
	     PL_write_term(Serror, wordToTermRef(catcher), 1200, 0);
	     Sdprintf("\n");
	     PL_discard_foreign_frame(fid);
	   });

  except = *catcher;
  catchfr = findCatcher(FR, catcher PASS_LD);
  DEBUG(1, { if ( catchfr )
	     { Sdprintf("[%d]: found catcher at %d\n",
			PL_thread_self(), levelFrame(catchfr));
	     } else
	     { Sdprintf("[%d]: not caught\n", PL_thread_self());
	     }
	   });

  SECURE(checkData(catcher));	/* verify all data on stacks stack */
  SECURE(checkStacks(FR, LD->choicepoints));

  if ( debugstatus.suspendTrace == FALSE &&
       exception_hook(FR, catchfr PASS_LD) )
  { catcher = valTermRef(exception_term);
    deRef(catcher);
    except = *catcher;
  }

#if O_DEBUGGER
  if ( !catchfr &&
       hasFunctor(except, FUNCTOR_error2) &&
       *valTermRef(exception_printed) != except )
  { QF = QueryFromQid(qid);	/* reload for relocation */

    if ( truePrologFlag(PLFLAG_DEBUG_ON_ERROR) &&
	 false(QF, PL_Q_CATCH_EXCEPTION) &&
	 !isCatchedInOuterQuery(QF, catcher) )
    { printMessage(ATOM_error, PL_TERM, exception_term);
      pl_trace();
    }
  }

  if ( debugstatus.debugging )
  { blockGC(PASS_LD1);
    for( ; FR && FR > catchfr; FR = FR->parent )
    { Choice ch = findStartChoice(FR, LD->choicepoints);

      if ( ch )
      { int printed = (*valTermRef(exception_printed) == except);

	SECURE(checkStacks(FR, ch));
	dbg_discardChoicesAfter((LocalFrame)ch PASS_LD);
	Undo(ch->mark);
	*valTermRef(LD->exception.pending) = except;
	if ( printed )
	  *valTermRef(exception_printed) = except;

	environment_frame = FR;
	SECURE(checkStacks(FR, ch));
	switch(tracePort(FR, ch, EXCEPTION_PORT, PC PASS_LD))
	{ case ACTION_RETRY:
	    setVar(*valTermRef(exception_printed));
	    exception_term = 0;
	    Undo(ch->mark);
	    discardChoicesAfter(FR PASS_LD);
	    DEF = FR->predicate;
	    unblockGC(PASS_LD1);
	    goto retry_continue;
	}

	setVar(*valTermRef(LD->exception.pending));
      }

      exception_term = 0;		/* save exception over call-back */
      discardChoicesAfter(FR PASS_LD);
      discardFrame(FR, FINISH_EXCEPT PASS_LD);
      *valTermRef(exception_bin) = *catcher;
      exception_term = exception_bin;
    }
    unblockGC(PASS_LD1);
  } else
#endif /*O_DEBUGGER*/
  { DEBUG(3, Sdprintf("Unwinding for exception\n"));

    for( ; FR && FR > catchfr; FR = FR->parent )
    { Choice ch = findStartChoice(FR, LD->choicepoints);

      dbg_discardChoicesAfter(FR PASS_LD);
      if ( ch )
	Undo(ch->mark);
      discardFrame(FR, FINISH_EXCEPT PASS_LD);
      SECURE(checkData(catcher));
    }
  }

					/* re-fetch (test cleanup(clean-5)) */
  catcher = valTermRef(exception_term);
  deRef(catcher);
  SECURE(checkData(catcher));

  if ( catchfr )
  { Word p = argFrameP(FR, 1);
    Choice ch = (Choice)argFrameP(FR, 3); /* Aligned above */

    assert(ch->type == CHP_CATCH);

    deRef(p);

    assert(catchfr == FR);
    discardChoicesAfter(FR PASS_LD);
    environment_frame = FR;
    Undo(ch->mark);
    unify_ptrs(p, catcher PASS_LD); /* Undo() also */
				    /* undoes unify of findCatcher() */
    lTop = (LocalFrame) argFrameP(FR, 3); /* above the catch/3 */
    argFrame(lTop, 0) = argFrame(FR, 2);  /* copy recover goal */
    *valTermRef(exception_printed) = 0;   /* consider it handled */
    *valTermRef(exception_bin)     = 0;
    exception_term		   = 0;

    PC = findCatchExit();
#if O_DYNAMIC_STACKS
    considerGarbageCollect((Stack)NULL);

    if ( LD->trim_stack_requested )
    { LocalFrame lsave = lTop;
      lTop = (LocalFrame)argFrameP(lTop, 1);
      trimStacks(PASS_LD1);
      lTop = lsave;
    }
#endif

    VMI_GOTO(I_USERCALL0);
  } else
  { Word p;

    QF = QueryFromQid(qid);	/* may be shifted: recompute */
    set(QF, PL_Q_DETERMINISTIC);
    FR = environment_frame = &QF->frame;
    p = argFrameP(FR, FR->predicate->functor->arity);
    lTop = (LocalFrame)(p+1);

    Undo(QF->choice.mark);
    QF->exception = consTermRef(p);
    *p = except;

    if ( false(QF, PL_Q_PASS_EXCEPTION) )
    { *valTermRef(exception_bin)     = 0;
      exception_term		     = 0;
      *valTermRef(exception_printed) = 0; /* consider it handled */
    }

#if O_DYNAMIC_STACKS
    considerGarbageCollect((Stack)NULL);
#endif
    if ( LD->trim_stack_requested )
    { trimStacks(PASS_LD1);
      QF = QueryFromQid(qid);		/* may be shifted: recompute */
    }

    QF->foreign_frame = PL_open_foreign_frame();
    fail;
  }

}
#endif /*O_CATCHTHROW*/

#if O_BLOCK
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_EXIT: exit(Block, RVal).  First does !(Block).

vm_list(exit/2):

   0 i_enter
   1 b_var0
   2 b_var1
   3 B_EXIT
   4 i_exit
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(B_EXIT, 0, 0, ())
{ Word name, rval;
  LocalFrame blockfr;

  name = argFrameP(lTop, 0); deRef(name);
  rval = argFrameP(lTop, 1); deRef(rval);
  lTop = (LocalFrame)argFrameP(lTop, 2);

  if ( !(blockfr = findBlock(FR, name)) )
  { if ( exception_term )
      goto b_throw;

    BODY_FAILED;
  }

  if ( raw_unify_ptrs(argFrameP(blockfr, 2), rval PASS_LD) )
  { for( ; ; FR = FR->parent )
    { SECURE(assert(FR > blockfr));
      discardChoicesAfter(FR PASS_LD);
      discardFrame(FR, FINISH_CUT PASS_LD);
      if ( FR->parent == blockfr )
      { PC = FR->programPointer;
	break;
      }
    }
				  /* TBD: tracing? */
    environment_frame = FR = blockfr;
    discardChoicesAfter(FR PASS_LD); /* delete possible CHP_DEBUG */
    DEF = FR->predicate;
    lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
    ARGP = argFrameP(lTop, 0);

    NEXT_INSTRUCTION;
  } else
  { lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
    ARGP = argFrameP(lTop, 0);

    BODY_FAILED;
  }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CUT_BLOCK: !(Block). Cuts all alternatives created after entering the
named block.

vm_list(!/1):
   0 i_enter
   1 b_var0
   2 I_CUT_BLOCK
   3 i_exit
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_CUT_BLOCK, 0, 0, ())
{ LocalFrame cutfr;
  Choice ch;
  Word name;

  name = argFrameP(lTop, 0); deRef(name);

  if ( !(cutfr = findBlock(FR, name)) )
  { if ( exception_term )
      goto b_throw;
    BODY_FAILED;
  }

  for(ch=BFR; (void *)ch > (void *)cutfr; ch = ch->parent)
  { LocalFrame fr2;

    DEBUG(3, Sdprintf("Discarding %s\n", chp_chars(ch)));
    for(fr2 = ch->frame;
	fr2 && fr2->clause && fr2 > FR;
	fr2 = fr2->parent)
	discardFrame(fr2, FINISH_CUT PASS_LD);
  }
  BFR = ch;

  lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
  ARGP = argFrameP(lTop, 0);

  NEXT_INSTRUCTION;
}
#endif /*O_BLOCK*/


		 /*******************************
		 *	    META-CALLING	*
		 *******************************/

BEGIN_SHAREDVARS
  Module module;
  functor_t functor;
  int arity;
  Word args;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_USERCALL0 is generated by the compiler if a variable is encountered as
a subclause. Note that the compount   statement  opened here is encloses
also I_CALL. This allows us to use   local register variables, but still
jump to the `normal_call' label to do the common part of all these three
virtual machine instructions.

I_USERCALL0 has the task of  analysing  the   goal:  it  should fill the
->procedure slot of the new frame and  save the current program counter.
It also is responsible of filling the   argument part of the environment
frame with the arguments of the term.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_USERCALL0, VIF_BREAK, 0, ())
{ word goal;
  Word a;

  module = NULL;
  NFR = lTop;
  a = argFrameP(NFR, 0);		/* get the goal */
  a = stripModule(a, &module PASS_LD);


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Determine the functor definition associated with the goal as well as the
arity and a pointer to the argument vector of the goal.

If the goal is not a  simple  goal,   the  body  is  compiled to `local'
clause. The compilation mode is presented to compileClause() by the NULL
`head'. This compilation-mode  constructs  a   stack-frame  whose  first
argument is the  goal-term,  followed   by  large  structures (compound,
string) from the arguments, followed  by   the  normal  local variables,
followed by the VM codes and, the   clause structure and finally a dummy
list for the clause-chain  (ClauseRef)  used   for  the  frames ->clause
field.

The clause data is discarded automatically  if the frame is invalidated.
Note that compilation does not give contained   atoms a reference as the
atom is referenced by the goal-term anyway.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  if ( isTextAtom(goal = *a) )
  { functor = lookupFunctorDef(goal, 0);
    arity   = 0;
    args    = NULL;
  } else if ( isTerm(goal) )
  { FunctorDef fd;

    functor = functorTerm(goal);
    if ( functor == FUNCTOR_colon2 )
      goto call_type_error;

    fd = valueFunctor(functor);
    if ( false(fd, CONTROL_F)
#if O_DEBUG
	 || GD->bootsession || !GD->initialised
#endif
       )
    { args    = argTermP(goal, 0);
      arity   = fd->arity;
    } else
    { Clause cl;

      a = &goal;			/* we're going to overwrite */
      deRef(a);
      DEBUG(1, { term_t g = a - (Word)lBase;
		 LocalFrame ot = lTop;
		 lTop += 100;
		 pl_write(g); pl_nl();
		 lTop = ot;
	       });
      lTop = NFR;
      setNextFrameFlags(NFR, FR);
      if ( !(cl = compileClause(NULL, a, PROCEDURE_dcall1,
				module PASS_LD)) )
	goto b_throw;

      DEF 		  = NFR->predicate;
      SECURE(assert(DEF == PROCEDURE_dcall1->definition));
      NFR->parent	  = FR;
      NFR->programPointer = PC;
#ifdef O_PROFILE
      NFR->prof_node      = FR->prof_node;
#endif
#ifdef O_LOGICAL_UPDATE
      cl->generation.erased = ~0L;
      cl->generation.created = NFR->generation = GD->generation;
#endif
      PC = cl->codes;

      enterDefinition(DEF);
      environment_frame = FR = NFR;
      ARGP = argFrameP(lTop, 0);

      NEXT_INSTRUCTION;
    }
  } else
  { fid_t fid;

  call_type_error:
    lTop = (LocalFrame)argFrameP(NFR, 1);
    fid = PL_open_foreign_frame();
    PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, wordToTermRef(argFrameP(NFR, 0)));
    PL_close_foreign_frame(fid);
    goto b_throw;
  }
  goto i_usercall_common;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_USERCALLN: translation of call(Goal, Arg1, ...)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

VMI(I_USERCALLN, VIF_BREAK, 1, (CA1_INTEGER))
{ Word a;
  int callargs = (int)*PC++;
  word goal;

  NFR = lTop;
  a = argFrameP(NFR, 0);		/* get the (now) instantiated */
  deRef(a);			/* variable */

  module = NULL;
  a = stripModule(a, &module PASS_LD);

  if ( isTextAtom(goal = *a) )
  { arity   = 0;
    functor = lookupFunctorDef(goal, callargs);
    args    = NULL;
  } else if ( isTerm(goal) )
  { FunctorDef fdef = valueFunctor(functorTerm(goal));

    arity   = fdef->arity;
    functor = lookupFunctorDef(fdef->name, arity + callargs);
    args    = argTermP(goal, 0);
  } else
  { lTop = (LocalFrame)argFrameP(NFR, 1);
    PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, wordToTermRef(argFrameP(NFR, 0)));
    goto b_throw;
  }

  if ( arity != 1 )
  { int i, shift = arity - 1;

    a = argFrameP(NFR, 1);	/* pointer to 1-st arg */

    if ( shift > 0 )
    { for(i=callargs-1; i>=0; i--)
      { if ( isRef(a[i]) )
	{ Word a1 = unRef(a[i]);

	  if ( a1 >= a && a1 < a+arity )
	    a[i+shift] = makeRef(a1+shift);
	  else
	    a[i+shift] = a[i];
	} else
	  a[i+shift] = a[i];
      }
    } else
    { for(i=0; i < callargs; i++)
      { if ( isRef(a[i]) )
	{ Word a1 = unRef(a[i]);

	  if ( a1 >= a && a1 < a+arity )
	    a[i+shift] = makeRef(a1+shift);
	  else
	    a[i+shift] = a[i];
	} else
	  a[i+shift] = a[i];
      }
    }
  }

i_usercall_common:
  setNextFrameFlags(NFR, FR);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Now scan the argument vector of the goal and fill the arguments  of  the
frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  if ( arity > 0 )
  { if ( arity > MAXARITY )
    { fid_t fid;

      lTop = (LocalFrame)argFrameP(NFR, 1);
      fid = PL_open_foreign_frame();
      PL_error(NULL, 0, NULL, ERR_REPRESENTATION, ATOM_max_arity);
      PL_close_foreign_frame(fid);
      goto b_throw;
    }

    ARGP = argFrameP(NFR, 0);

    for(; arity-- > 0; ARGP++, args++)
      *ARGP = linkVal(args);
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find the associated procedure.  First look in the specified module.   If
the function is not there then look in the user module.  Finally specify
the context module environment for the goal. This is not necessary if it
will  be  specified  correctly  by  the goal started.  Otherwise tag the
frame and write  the  module  name  just  below  the  frame.   See  also
contextModule().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  SAVE_REGISTERS(qid);
  DEF = getProcDefinedDefinition(&NFR, PC,
				 resolveProcedure(functor, module)->definition
				 PASS_LD);
  LOAD_REGISTERS(qid);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Save the program counter (note  that   I_USERCALL0  has no argument) and
continue as with a normal call.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  if ( true(DEF, P_TRANSPARENT) )
    setContextModule(NFR, module);

  goto normal_call;
}
END_SHAREDVARS
