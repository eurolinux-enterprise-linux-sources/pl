/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2009, University of Amsterdam

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

#ifndef MAXVARNAME
#define MAXVARNAME 1024
#endif

/** shell(+Command:text, -Status:integer) is det.

Run an external command and wait for its completion.
*/

static
PRED_IMPL("shell", 2, shell, 0)
{ char *cmd;

  if ( PL_get_chars_ex(A1, &cmd, CVT_ALL|REP_FN) )
  { int rval = System(cmd);

    return PL_unify_integer(A2, rval);
  }

  fail;
}


word
pl_getenv(term_t var, term_t value)
{ char *n;

  if ( PL_get_chars_ex(var, &n, CVT_ALL|REP_FN) )
  { char buf[1024];
    size_t size;

    if ( (size=getenv3(n, buf, sizeof(buf))) != (size_t)-1 )
    { if ( size < sizeof(buf) )
      { return PL_unify_chars(value, PL_ATOM|REP_FN, size, buf);
      } else
      { char *buf = PL_malloc(size+1);
        int rc;

        size = getenv3(n, buf, size+1);
        if ( size > 0 )
          rc = PL_unify_chars(value, PL_ATOM|REP_FN, size, buf);
        else
          rc = FALSE;

        PL_free(buf);
        return rc;
      }
    }

    fail;
  }

  fail;
}


word
pl_setenv(term_t var, term_t value)
{ char *n, *v;

  if ( PL_get_chars_ex(var, &n, CVT_ALL|REP_FN|BUF_RING) &&
       PL_get_chars_ex(value, &v, CVT_ALL|REP_FN) )
    return Setenv(n, v);

  fail;
}


word
pl_unsetenv(term_t var)
{ char *n;

  if ( PL_get_chars_ex(var, &n, CVT_ALL|REP_FN) )
    return Unsetenv(n);

  fail;
}


word
pl_get_time(term_t t)
{ return PL_unify_float(t, WallTime());
}


word
pl_sleep(term_t time)
{ double t;

  if ( PL_get_float_ex(time, &t) )
    return Pause(t);

  fail;
}

#ifdef __WINDOWS__
#include <process.h>
#endif

word
pl_get_pid(term_t pid)
{ return PL_unify_integer(pid, getpid());
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(system)
  PRED_DEF("shell", 2, shell, 0)
EndPredDefs
