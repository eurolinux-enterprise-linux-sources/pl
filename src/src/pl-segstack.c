/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@uva.nl
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

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Segmented stack handling. A segmented stack is a stack that is allocated
in segments, This means we cannot   compare  addresses otherwise then by
identity.  We use a segmented stack for cycle detection.

Measurements with the chunksize on SuSE Linux  10.2 indicate there is no
measurable performance change above approximately  256 bytes. We'll keep
the figure on the safe  side  for   systems  with  less efficient malloc
implementations.

Note  that  atom-gc  requires   completely    asynchronous   calling  of
scanSegStack() and therefore pushSegStack()/popSegStack()  must push the
data before updating the pointers.

TBD: Avoid instruction/cache write reordering in push/pop.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define CHUNKSIZE (1*1024)

int
pushSegStack(segstack *stack, void *data)
{ if ( stack->top + stack->unit_size <= stack->max )
  { memcpy(stack->top, data, stack->unit_size);
    stack->top += stack->unit_size;
    stack->count++;

    return TRUE;
  } else
  { segchunk *chunk = PL_malloc(CHUNKSIZE);

    if ( !chunk )
      return FALSE;			/* out of memory */

    chunk->next = NULL;
    chunk->previous = stack->last;
    chunk->top = chunk->data;		/* async scanning */
    if ( stack->last )
    { stack->last->next = chunk;
      stack->last->top = stack->top;
      stack->top = chunk->top;		/* async scanning */
      stack->last = chunk;
    } else
    { stack->top = chunk->top;		/* async scanning */
      stack->last = stack->first = chunk;
    }

    stack->base = chunk->data;
    stack->max  = addPointer(chunk, CHUNKSIZE);
    memcpy(chunk->data, data, stack->unit_size);
    stack->top  = chunk->data + stack->unit_size;
    stack->count++;

    return TRUE;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Pop data. Note that we leave the first chunk associated with the stack
to speedup frequent small usage.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
popSegStack(segstack *stack, void *data)
{ again:

  if ( stack->top >= stack->base + stack->unit_size )
  { stack->top -= stack->unit_size;
    memcpy(data, stack->top, stack->unit_size);
    stack->count--;

    return TRUE;
  } else
  { segchunk *chunk = stack->last;

    if ( chunk )
    { if ( chunk->previous )
      { stack->last = chunk->previous;
	stack->last->next = NULL;
	PL_free(chunk);

	chunk = stack->last;
	stack->base = chunk->data;
	stack->max  = addPointer(chunk, CHUNKSIZE);
	stack->top  = chunk->top;
	goto again;
      }
#if 0
        else
      { PL_free(chunk);
	stack->first = stack->last = NULL;
	stack->base = stack->max = stack->top = NULL;
      }
#endif
    }

    return FALSE;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
scanSegStack(segstack *stack, void (*func)(void *cell))
Walk along all living cells on the stack and call func on them.  The stack
is traversed last-to-first.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static inline void
scan_chunk(segstack *stack, char *top, char *base, void (*func)(void *cell))
{ while(top >= base+stack->unit_size)
  { top -= stack->unit_size;
    (*func)((void*)top);
  }
}


void
scanSegStack(segstack *stack, void (*func)(void *cell))
{ segchunk *chunk;

  if ( (chunk=stack->last) )		/* something there */
  { chunk->top = stack->top;		/* close last chunk */
    for(; chunk; chunk=chunk->previous)
      scan_chunk(stack, chunk->top, chunk->data, func);
  }
}


void
clearSegStack(segstack *s)
{ segchunk *c, *n;

  c = s->first;

  for(; c; c = n)
  { n = c->next;
    PL_free(c);
  }

  memset(s, 0, sizeof(*s));
}
