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

#define bool pl_bool			/* avoid conflict with curses */
#include "pl-incl.h"
#undef bool

#define LOCK()   PL_LOCK(L_TERM)
#define UNLOCK() PL_UNLOCK(L_TERM)

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module defines some hacks to get to the unix  termcap  library.   I
realise this is not a proper answer to terminal control from Prolog, but
I  needed  it  some day and at least it is better than doing things like
shell(clear), coding terminal sequences hard, etc.   One  day  I  should
write a decent interface to handle the terminal.  Maybe this will be too
late;  character terminals  disappear quickly now.  Use XPCE if you want
windowing!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef HAVE_TGETENT
#ifndef NO_SYS_IOCTL_H_WITH_SYS_TERMIOS_H
#include <sys/ioctl.h>
#endif

#undef clear				/* conflicts */
#include <curses.h>
#include <term.h>

#define MAX_TERMBUF	1024		/* Confirming manual */
#define STAT_START	0
#define STAT_OK		1
#define STAT_ERROR	2

#ifdef TERMCAP_NEEDS_VARS
char	PC;				/* Term lib variables */
char   *BC;
char   *UP;
short	ospeed;
#endif

#define term_initialised    (GD->terminal.initialised)
#define string_area	    (GD->terminal._string_area)
#define buf_area	    (GD->terminal._buf_area)
#define capabilities        (GD->terminal._capabilities)

typedef struct
{ atom_t type;				/* type of the entry */
  atom_t name;				/* Name of the value */
  word  value;				/* Value of the entry */
} entry, *Entry;

void
resetTerm()
{ LOCK();
  if ( capabilities == NULL )
  { capabilities = newHTable(16);
  } else
  { term_initialised = STAT_START;
    for_table(capabilities, s,
	      freeHeap(s->value, sizeof(entry)));
    clearHTable(capabilities);
  }
  UNLOCK();
}


/* MT: Locked by calling lookupEntry()
*/

static int
initTerm(void)
{ if ( term_initialised == STAT_START )
  { char term[100];

    term_initialised = STAT_ERROR;
    if ( !Getenv("TERM", term, sizeof(term)) )
    { term_t env = PL_new_term_ref();

      PL_put_atom_chars(env, "TERM");
      PL_error(NULL, 0, NULL, ERR_EXISTENCE,
	       ATOM_environment, env);
      goto out;
    }

    if ( buf_area == NULL )
      buf_area = allocHeap(MAX_TERMBUF);
    if ( string_area == NULL )
      string_area = allocHeap(MAX_TERMBUF);

    switch( tgetent(buf_area, term) )
    { case -1:
      { term_t t = PL_new_term_ref();

	PL_put_atom_chars(t, "termcap");
	PL_error(NULL, 0, "tgetent() cannot read database",
		 ERR_PERMISSION,
		 ATOM_file, ATOM_read, t);
	goto out;
      }
      case  1:
	break;
      default:
      case  0:
      { term_t t = PL_new_term_ref();

	PL_put_atom_chars(t, term);
	PL_error(NULL, 0, NULL, ERR_EXISTENCE,
		 ATOM_terminal, t);
	goto out;
      }
    }

    term_initialised = STAT_OK;
  }

out:
  return term_initialised == STAT_OK;
}


static Entry
lookupEntry(atom_t name, atom_t type)
{ Symbol s;
  Entry e;

  LOCK();
  if ( (s = lookupHTable(capabilities, (void*)name)) == NULL )
  { if ( !initTerm() )
    { e = NULL;
      goto out;
    }

    e = (Entry) allocHeap(sizeof(entry));
    e->name = name;
    e->type = type;
    e->value = 0L;

    if ( type == ATOM_number )
    { int n;

      if ( (n = tgetnum(stringAtom(name))) != -1 )
        e->value  = consInt(n);
    } else if ( type == ATOM_bool )
    { int b;

      if ( (b = tgetflag(stringAtom(name))) != -1 )
        e->value = (b ? ATOM_on : ATOM_off);
    } else if ( type == ATOM_string )
    { char *s;

      if ( (s = tgetstr(stringAtom(name), &string_area)) != NULL )
        e->value  = PL_new_atom(s);	/* locked: ok */
    } else
    { warning("tgetent/3: Illegal type");
      freeHeap(e, sizeof(entry));
      e = NULL;
      goto out;
    }

    addHTable(capabilities, (void *)name, e);
  } else
    e = (Entry)s->value;

out:
  UNLOCK();
  return e;
}


word
pl_tty_get_capability(term_t name, term_t type, term_t value)
{ Entry e;
  atom_t n, t;

  if ( !PL_get_atom_ex(name, &n) || !PL_get_atom_ex(type, &t) )
    fail;
  if ( !(e = lookupEntry(n, t)) )
    fail;

  if ( e->value != 0L )
    return _PL_unify_atomic(value, e->value);

  fail;
}


static int
tputc(int chr)
{ return Sputc(chr, Suser_output);
}


word
pl_tty_goto(term_t x, term_t y)
{ Entry e;
  char *s;
  int ix, iy;

  if ( !PL_get_integer_ex(x, &ix) ||
       !PL_get_integer_ex(y, &iy) )
    fail;

  if ( (e = lookupEntry(ATOM_cm, ATOM_string)) == NULL ||
        e->value == 0L )
  { term_t obj = PL_new_term_ref();

    PL_put_atom(obj, ATOM_cm);
    return PL_error("tty_goto", 2, NULL, ERR_EXISTENCE,
		    ATOM_terminal_capability, obj);
  }

  s = tgoto(stringAtom(e->value), ix, iy);
  if ( streq(s, "OOPS") )
    fail;

  tputs(s, 1, tputc);
  succeed;
}


word
pl_tty_put(term_t a, term_t affcnt)
{ char *s;
  int n;

  if ( PL_get_chars_ex(a, &s, CVT_ALL) &&
       PL_get_integer_ex(affcnt, &n) )
  { tputs(s, n, tputc);
    succeed;
  }

  fail;
}


word
pl_tty_size(term_t r, term_t c)
{ int rows, cols;

#ifdef __unix__
  int iorval;

#ifdef TIOCGSIZE
  struct ttysize ws;
  iorval = ioctl(0, TIOCGSIZE, &ws);

  rows = ws.ts_lines;
  cols = ws.ts_cols;
#else
#ifdef TIOCGWINSZ
  struct winsize ws;
  iorval = ioctl(0, TIOCGWINSZ, &ws);

  rows = ws.ws_row;
  cols = ws.ws_col;
#else
  Entry er, ec;

  if ( (er=lookupEntry(ATOM_li, ATOM_number)) &&
       (ec=lookupEntry(ATOM_co, ATOM_number)) &&
       er->value && ec->value )
  { rows = valInt(er->value);
    cols = valInt(ec->value);
    iorval = 0;
  } else
    iorval = -1;
#endif
#endif

  if ( iorval != 0 )
    return PL_error("tty_size", 2, MSG_ERRNO, ERR_SYSCALL, "ioctl");
#else /*__unix__*/
  rows = ScreenRows();			/* old stuff refering to plterm.dll */
  cols = ScreenCols();			/* not used anyway */
#endif /*__unix__*/

  return PL_unify_integer(r, rows) &&
	 PL_unify_integer(c, cols);
}

#else /* ~TGETENT */

#ifdef __WINDOWS__

static void *
getModuleFunction(const char *module, const char *name)
{ HMODULE hconsole;

  if ( (hconsole=GetModuleHandle(module)) )
  { return GetProcAddress(hconsole, name);
  }

  return NULL;
}

word
pl_tty_size(term_t r, term_t c)
{ int (*ScreenCols)(void *h) = getModuleFunction("plterm", "ScreenCols");
  int (*ScreenRows)(void *h) = getModuleFunction("plterm", "ScreenRows");

  if ( ScreenCols && ScreenRows )
  { void *(*get_console)(void) = getModuleFunction(NULL, "PL_current_console");
    void *con = (get_console ? (*get_console)() : NULL);
    int rows = (*ScreenRows)(con);
    int cols = (*ScreenCols)(con);

    if ( PL_unify_integer(r, rows) &&
	 PL_unify_integer(c, cols) )
      succeed;
  }

  return notImplemented("tty_size", 2);
}

#define HAVE_PL_TTY_SIZE 1

#endif /*__WINDOWS__*/

void resetTerm()
{
}

word
pl_tty_get_capability(term_t name, term_t type, term_t value)
{ return notImplemented("tty_get_capability", 3);
}

word
pl_tty_goto(term_t x, term_t y)
{ return notImplemented("tty_goto", 2);
}

word
pl_tty_put(term_t a, term_t affcnt)
{ return notImplemented("tty_put", 2);
}

word
pl_set_tty(term_t old, term_t new)
{ return notImplemented("set_tty", 2);
}

#ifndef HAVE_PL_TTY_SIZE
word
pl_tty_size(term_t r, term_t c)
{ return notImplemented("tty_size", 2);
}
#endif

#endif /* TGETENT */
