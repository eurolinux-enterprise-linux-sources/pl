/*  $Id$

    Copyright (c) 1991 Jan Wielemaker. All rights reserved.
    jan@swi.psy.uva.nl

    Purpose: Determine machines memory-model
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
  unsigned long gta = (unsigned long) sub;
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
    decl[ndecl++] = "POINTER_OFFSET=0x80000000L";
  else if ( (gva & 0xf0000000L) == 0x40000000L )
    decl[ndecl++] = "POINTER_OFFSET=0x40000000L";
  else if ( (gva & 0xf0000000L) == 0x20000000L )
    decl[ndecl++] = "POINTER_OFFSET=0x20000000L";
  else if ( (gva & 0xf0000000L) == 0x10000000L )
    decl[ndecl++] = "POINTER_OFFSET=0x10000000L";
  else if ( (gva & 0xf0000000L) )
    printf("ERROR: Cannot determine POINTER_OFFSET\n");

  if      ( (gta & 0xf0000000L) == 0x80000000L )
    decl[ndecl++] = "TEXT_OFFSET=0x80000000L";
  else if ( (gta & 0xf0000000L) == 0x40000000L )
    decl[ndecl++] = "TEXT_OFFSET=0x40000000L";
  else if ( (gta & 0xf0000000L) == 0x20000000L )
    decl[ndecl++] = "TEXT_OFFSET=0x20000000L";
  else if ( (gta & 0xf0000000L) == 0x10000000L )
    decl[ndecl++] = "TEXT_OFFSET=0x10000000L";
  else if ( (gta & 0xf0000000L) )
    printf("ERROR: Cannot determine TEXT_OFFSET\n");

  if ( stack_up )
    decl[ndecl++] = "STACK_DIRECTION=1";
  else
    decl[ndecl++] = "STACK_DIRECTION=-1";

  if ( ndecl > 0 )
  { int n;

    for(n=0; n<ndecl; n++)
      printf("%s\n", decl[n]);
  }

  exit(0);
}
