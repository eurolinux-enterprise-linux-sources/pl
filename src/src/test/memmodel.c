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

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This program   was written  to  determine the  memory  model   of your
machine.  It may be used when writing a new md-machine.h file.

Compile this file using:

	% cc -o m-model m-model.c
	% ./m-model
	Memory layout:

		Text at 0x2290
		Global variable at 0x40d0
		Local variable at 0xeffff938
		malloc() at 0x61a0
		C-Stack grows Downward

	No special declarations needed in "md.h"

	%
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <stdio.h>

#define K * 1024
#define MAX_DECL 100

int	global_var;

char *
sub()
{ char buf[10];

  return buf;
}

main(argc, argv)
int argc;
char *argv[];
{ char buf[10];
  unsigned long gva = (unsigned long) &global_var;
  int stack_up = (sub() > buf);
  char *decl[MAX_DECL];
  int ndecl = 0;

#ifdef VERBOSE
  printf("Memory layout:\n\n");
  printf("\tText at 0x%x\n", sub);
  printf("\tGlobal variable at 0x%x\n", gva);
  printf("\tLocal variable at 0x%x\n", buf);
  printf("\tmalloc() at 0x%x\n", malloc(10));
  printf("\tC-Stack grows %sward\n", stack_up ? "Up" : "Down");
#endif

  if      ( (gva & 0xf0000000L) == 0x80000000L )
    decl[ndecl++] = "DATA_AT_0X8=1;";
  else if ( (gva & 0xf0000000L) == 0x40000000L )
    decl[ndecl++] = "DATA_AT_0X4=1;";
  else if ( (gva & 0xf0000000L) == 0x20000000L )
    decl[ndecl++] = "DATA_AT_0X2=1;";
  else if ( (gva & 0xf0000000L) == 0x10000000L )
    decl[ndecl++] = "DATA_AT_0X1=1;";
  else if ( (gva & 0xf0000000L) )
  { printf("PROBLEM: Memory model not supported; see \"pl-incl.h\"\n");
    exit(1);
  }

  if ( stack_up )
    decl[ndecl++] = "STACK_DIRECTION=1;";
  else
    decl[ndecl++] = "STACK_DIRECTION=-1;";

  if ( !malloc(200000) )
  { printf("PROBLEM: malloc(200000) fails; see \"pl-os.c\"\n");
    exit(1);
  }

  if ( ndecl > 0 )
  { int n;

    for(n=0; n<ndecl; n++)
      printf("%s\n", decl[n]);
  }

  exit(0);
}
