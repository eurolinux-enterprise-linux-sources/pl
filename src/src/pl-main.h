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

		/********************************
		*           STRUCTURES		*
		********************************/

typedef struct
{ char *state;				/* system's boot file */
  char *startup;			/* default user startup file */
  int  local;				/* default local stack size (K) */
  int  global;				/* default global stack size (K) */
  int  trail;				/* default trail stack size (K) */
  int  argument;			/* default argument stack size (K) */
  int  heap;				/* default heap size (K) */
  char *goal;				/* default initialisation goal */
  char *toplevel;			/* default top level goal */
  bool notty;				/* use tty? */
  char *arch;				/* machine/OS we are using */
  char *home;				/* systems home directory */
} pl_defaults_t;

typedef struct
{ intptr_t		localSize;		/* size of local stack */
  intptr_t		globalSize;		/* size of global stack */
  intptr_t		trailSize;		/* size of trail stack */
  intptr_t		argumentSize;		/* size of argument stack */
  intptr_t		heapSize;		/* size of the heap */
  char *	goal;			/* initial goal */
  char *	topLevel;		/* toplevel goal */
  char *	initFile;		/* -f initialisation file */
  char *	systemInitFile;		/* -F initialisation file */
  char *	scriptFile;		/* -s script file */
  char *	compileOut;		/* file to store compiler output */
  char *	saveclass;		/* Type of saved state */
  bool		silent;			/* -q: quiet operation */
} pl_options_t;


		/********************************
		*           PARAMETERS		*
		********************************/

#ifndef DEFSTARTUP
#define DEFSTARTUP ".plrc"
#endif
#ifndef SYSTEMHOME
#define SYSTEMHOME "/usr/local/lib/pl"
#endif
#ifndef NOTTYCONTROL
#define NOTTYCONTROL FALSE
#endif

#ifndef ARCH
#define ARCH "unknown"
#endif
#ifndef OS
#define OS "unknown"
#endif

#define DEF_DEFLOCAL	(4096*SIZEOF_VOIDP)
#define DEF_DEFGLOBAL	(8192*SIZEOF_VOIDP) /* 32MB on 32-bit hardware */
#define DEF_DEFTRAIL	(8192*SIZEOF_VOIDP)
#define DEF_DEFARGUMENT (250*SIZEOF_VOIDP)
#define DEF_DEFHEAP     0		/* unlimited */

#ifndef DEFLOCAL
#define DEFLOCAL    DEF_DEFLOCAL
#endif
#ifndef DEFGLOBAL
#define DEFGLOBAL   DEF_DEFGLOBAL
#endif
#ifndef DEFTRAIL
#define DEFTRAIL    DEF_DEFTRAIL
#endif
#ifndef DEFARGUMENT
#define DEFARGUMENT DEF_DEFARGUMENT
#endif
#ifndef DEFHEAP
#define DEFHEAP     DEF_DEFHEAP
#endif
