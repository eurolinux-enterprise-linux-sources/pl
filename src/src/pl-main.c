/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2005, University of Amsterdam

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
Get the ball rolling.  The main task of  this  module  is  command  line
option  parsing,  initialisation  and  handling  of errors and warnings.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define O_DEBUG 1

#include "rc/rc.h"
#include "pl-incl.h"
#include "pl-ctype.h"
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef __WINDOWS__
#include <process.h>			/* getpid() */
#endif

#define LOCK()   PL_LOCK(L_INIT)
#define UNLOCK() PL_UNLOCK(L_INIT)

#ifdef FORCED_MALLOC_BASE
#include "morecore.c"
#endif

#if defined(_DEBUG) && defined(__WINDOWS__)
#include <crtdbg.h>
#endif

#if defined(HAVE_MTRACE) && defined(O_MAINTENANCE)
#include <mcheck.h>
#endif

static int	usage(void);
static int	giveVersionInfo(const char *a);
static bool	vsysError(const char *fm, va_list args);

#define	optionString(s) { if (argc > 1) \
			  { if ( s ) remove_string(s); \
			    s = store_string(argv[1]); \
			    argc--; argv++; \
			  } else \
			  { return -1; \
			  } \
			}
#define K * 1024L

#define EXECVARMAGIC "$EXECVARS="
static const char exec_vars[512] = EXECVARMAGIC;

static const char *
exec_var(const char *name)
{ const char *s=exec_vars + strlen(EXECVARMAGIC);
  size_t l = strlen(name);

  while(s < exec_vars+sizeof(exec_vars))
  { if ( strncmp(name, s, l) == 0 && s[l] == '=' )
      return &s[l+1];
    while(*s && s< exec_vars+sizeof(exec_vars))
      s++;
    while(*s == '\0' && s< exec_vars+sizeof(exec_vars))
      s++;
  }

  return NULL;
}


static const char *
longopt(const char *opt, int argc, const char **argv)
{ size_t optlen = strlen(opt);

  for(; argc > 0; argc--, argv++)
  { const char *a = argv[0];

    if ( *a++ == '-' && *a++ == '-' )
    { if ( *a == EOS )		/* --: end of args */
	return NULL;
      if ( strncmp(a, opt, optlen) == 0 && a[optlen] == '=' )
	return &a[optlen+1];
    }
  }

  return NULL;
}


static char *
findHome(const char *symbols, int argc, const char **argv)
{ const char *home = NULL;
  char envbuf[MAXPATHLEN];
  char plp[MAXPATHLEN];
  const char *val;

  if ( (val=longopt("home", argc, argv)) )
  { if ( (home=PrologPath(val, plp, sizeof(plp))) )
      return store_string(home);
    return NULL;
  }

  if ( (val  = exec_var("homevar")) &&
       (home = Getenv(val, envbuf, sizeof(envbuf))) &&
       (home = PrologPath(home, plp, sizeof(plp))) )
    return store_string(home);
  if ( (val = exec_var("home")) &&
       (home = PrologPath(val, plp, sizeof(plp))) )
    return store_string(home);

  if ( !(home = Getenv("SWI_HOME_DIR", envbuf, sizeof(envbuf))) )
    home = Getenv("SWIPL", envbuf, sizeof(envbuf));
  if ( home &&
       (home = PrologPath(home, plp, sizeof(plp))) &&
       ExistsDirectory(home) )
    return store_string(home);

  if ( (home = symbols) )
  { char buf[MAXPATHLEN];
    char parent[MAXPATHLEN];
    IOSTREAM *fd;

    strcpy(parent, DirName(DirName(AbsoluteFile(home, buf), buf), buf));
    Ssprintf(buf, "%s/swipl", parent);

    if ( (fd = Sopen_file(buf, "r")) )
    { if ( Sfgets(buf, sizeof(buf), fd) )
      { size_t l = strlen(buf);

	while(l > 0 && buf[l-1] <= ' ')
	  l--;
	buf[l] = EOS;

#if O_XOS
      { char buf2[MAXPATHLEN];
	_xos_canonical_filename(buf, buf2, MAXPATHLEN, 0);
	strcpy(buf, buf2);
      }
#endif

	if ( !IsAbsolutePath(buf) )
	{ char buf2[MAXPATHLEN];

	  Ssprintf(buf2, "%s/%s", parent, buf);
	  home = AbsoluteFile(buf2, plp);
	} else
	  home = AbsoluteFile(buf, plp);

	if ( ExistsDirectory(home) )
	{ Sclose(fd);
	  return store_string(home);
	}
      }
      Sclose(fd);
    }
  }

  if ( (home = PrologPath(PLHOME, plp, sizeof(plp))) &&
       ExistsDirectory(home) )
    return store_string(home);

  return NULL;
}

/*
  -- atoenne -- convert state to an absolute path. This allows relative
  SWI_HOME_DIR and cleans up non-canonical paths.
*/

#ifndef IS_DIR_SEPARATOR
#define IS_DIR_SEPARATOR(c) ((c) == '/')
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The default name of the system init file `base.rc' is determined from the
basename of the running program, taking all the leading alnum characters.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static char *
defaultSystemInitFile(const char *a0)
{ char plp[MAXPATHLEN];
  char *base = BaseName(PrologPath(a0, plp, sizeof(plp)));
  char buf[256];
  char *s = buf;

  while(*base && isAlpha(*base))
    *s++ = *base++;
  *s = EOS;

  if ( buf[0] != EOS )
    return store_string(buf);

  return store_string("pl");
}


#define MEMAREA_INVALID_SIZE (uintptr_t)(~0L)

static uintptr_t
memarea_limit(const char *s)
{ number n;
  unsigned char *q;

  if ( str_number((unsigned char *)s, &q, &n, FALSE) && intNumber(&n) )
  { switch((int)*q)
    { case 'k':
      case 'K':
      case EOS:
	return (intptr_t)n.value.i K;
      case 'm':
      case 'M':
	return (intptr_t)n.value.i K K;
      case 'g':
      case 'G':
	return (intptr_t)n.value.i K K K;
      case 'b':
      case 'B':
	return (intptr_t)n.value.i;
    }
  }

  return MEMAREA_INVALID_SIZE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
When detected to run under a  GNU-Emacs   shell  or using M-x run-prolog
from GNU-Emacs, don't pretend we can manipulate the TTY settings.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
setupGNUEmacsInferiorMode()
{ char envbuf[4];
  char *s;
  int val;

  if ( ((s = Getenv("EMACS", envbuf, sizeof(envbuf))) && s[0]) ||
       ((s = Getenv("INFERIOR", envbuf, sizeof(envbuf))) && streq(s, "yes")) )
  { clearPrologFlagMask(PLFLAG_TTY_CONTROL);
    val = TRUE;
  } else
  { val = FALSE;
  }

  PL_set_prolog_flag("emacs_inferior_process", PL_BOOL|FF_READONLY, val);
}


static void
initPaths(int argc, const char **argv)
{ char plp[MAXPATHLEN];

  if ( argc > 0 )
  { char plp1[MAXPATHLEN];
    const char *symbols = NULL;		/* The executable */

    if ( !(symbols = findExecutable(argv[0], plp1)) ||
	 !(symbols = DeRefLink(symbols, plp)) )
      symbols = argv[0];

    DEBUG(2, Sdprintf("rc-module: %s\n", symbols));

    systemDefaults.home	       = findHome(symbols, argc, argv);

#ifdef __WINDOWS__			/* we want no module but the .EXE */
    GD->paths.module	       = store_string(symbols);
    symbols = findExecutable(NULL, plp);
    DEBUG(2, Sdprintf("Executable: %s\n", symbols));
#endif
    GD->paths.executable       = store_string(symbols);
    GD->options.systemInitFile = defaultSystemInitFile(argv[0]);
  } else
  { systemDefaults.home	       = findHome(NULL, argc, argv);
    GD->options.systemInitFile = store_string("none");
#ifdef __WINDOWS__			/* we want no module but the .EXE */
    GD->paths.module	       = store_string("libpl.dll");
#endif
  }

  systemDefaults.startup = store_string(PrologPath(DEFSTARTUP, plp, sizeof(plp)));

#ifdef O_XOS
  if ( systemDefaults.home )
  { char buf[MAXPATHLEN];
    _xos_limited_os_filename(systemDefaults.home, buf);
    systemDefaults.home = store_string(buf);
  }
#endif
}


static void
initDefaults()
{ systemDefaults.arch	     = ARCH;
  systemDefaults.local       = DEFLOCAL;
  systemDefaults.global      = DEFGLOBAL;
  systemDefaults.trail       = DEFTRAIL;
  systemDefaults.argument    = DEFARGUMENT;
  systemDefaults.heap	     = DEFHEAP;
  systemDefaults.goal	     = "'$welcome'";
  systemDefaults.toplevel    = "prolog";
  systemDefaults.notty       = NOTTYCONTROL;

#ifdef __WINDOWS__
  getDefaultsFromRegistry();
#endif

  GD->io_initialised	     = FALSE;
  GD->initialised	     = FALSE;
  GD->bootsession	     = FALSE;

  if ( systemDefaults.notty )
    clearPrologFlagMask(PLFLAG_TTY_CONTROL);
  else
    setPrologFlagMask(PLFLAG_TTY_CONTROL);

  setPrologFlagMask(PLFLAG_DEBUGINFO);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Initialise the default values  for  the   various  options.  First,  the
options are initialised from the  code.  Next,   if  this  is not a boot
session, the resource $system:options is opened, which contains lines of
the format

	<name>=<value>

This  file  is  parsed,  and  the    values  are  interpreted.  See  the
if-then-else below for the defined values.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define MAXVARNAME 256
#define MAXVARVAL  1024

static int
getVarFromStream(IOSTREAM *s, char *name, char *value)
{ char *q;
  int l;
  int c;

again:
  for(l=MAXVARNAME, q=name; --l > 0; )
  { switch(c = Sgetc(s))
    { case EOF:
	return FALSE;
      case '=':
	*q = EOS;
        goto do_value;
      case '\n':
	goto again;
      default:
	*q++ = c;
    }
  }
  return FALSE;

do_value:
  for(l=MAXVARVAL, q=value; --l > 0; )
  { switch(c = Sgetc(s))
    { case EOF:
      case '\n':
	*q = EOS;
        return TRUE;
      default:
	*q++ = c;
    }
  }
  return FALSE;
}


static void
initDefaultOptions()
{ GD->options.compileOut    = store_string("a.out");
  GD->options.localSize     = systemDefaults.local    K;
  GD->options.globalSize    = systemDefaults.global   K;
  GD->options.trailSize     = systemDefaults.trail    K;
  GD->options.argumentSize  = systemDefaults.argument K;
  GD->options.heapSize      = systemDefaults.heap     K;
  GD->options.goal	    = store_string(systemDefaults.goal);
  GD->options.topLevel      = store_string(systemDefaults.toplevel);
  GD->options.initFile      = store_string(systemDefaults.startup);
  GD->options.scriptFile    = store_string("");
  GD->options.saveclass	    = store_string("none");

  if ( !GD->bootsession && GD->resourceDB )
  { IOSTREAM *op = SopenRC(GD->resourceDB, "$options", "$prolog", RC_RDONLY);

    if ( op )
    { char name[MAXVARNAME];
      char val[MAXVARVAL];

      while( getVarFromStream(op, name, val) )
	set_pl_option(name, val);

      Sclose(op);
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Does the commandline option parsing.  Actually   we  should  use the GNU
getopt package and deal nicely with intptr_t   arguments  as well as shorts,
but these options are  too  widely  used   as  they  are  to change them
overnight. Returns -1 on error.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
isoption(const char *av, const char *opt)
{ return (streq(av, opt) || (av[0] == '-' && streq(av+1, opt)));
}


static int
parseCommandLineOptions(int argc0, char **argv, int *compile)
{ int argc = argc0;



  for( ; argc > 0 && (argv[0][0] == '-' || argv[0][0] == '+'); argc--, argv++ )
  { char *s = &argv[0][1];

    if ( streq(s, "-" ) )		/* pl <plargs> -- <app-args> */
      break;

    if ( streq(s, "tty") )	/* +/-tty */
    { if ( s[-1] == '+' )
	setPrologFlagMask(PLFLAG_TTY_CONTROL);
      else
	clearPrologFlagMask(PLFLAG_TTY_CONTROL);

      continue;
    } else if ( isoption(s, "nosignals") )
    { clearPrologFlagMask(PLFLAG_SIGNALS);
      continue;
    } else if ( isoption(s, "nodebug") )
    { clearPrologFlagMask(PLFLAG_DEBUGINFO);
      continue;
    } else if ( streq(s, "-quiet") )
    { GD->options.silent = TRUE;
      continue;
    }

    if ( *s == '-' )
      continue;				/* don't handle --long=value */

    while(*s)
    { switch(*s)
      { case 'd':	if (argc > 1)
			{ GD->debug_level = atoi(argv[1]);
			  argc--, argv++;
			} else
			  return -1;
			break;
	case 'p':	if (!argc)	/* handled in Prolog */
			  return -1;
			argc--, argv++;
			break;
	case 'O':	GD->cmdline.optimise = TRUE; /* see initFeatures() */
			break;
	case 'x':
  	case 'o':	optionString(GD->options.compileOut);
			break;
	case 'f':	optionString(GD->options.initFile);
			break;
	case 'F':	optionString(GD->options.systemInitFile);
			break;
	case 's':	optionString(GD->options.scriptFile);
			break;
	case 'g':	optionString(GD->options.goal);
			break;
	case 't':	optionString(GD->options.topLevel);
			break;
	case 'c':	*compile = TRUE;
			break;
	case 'b':	GD->bootsession = TRUE;
			break;
	case 'q':	GD->options.silent = TRUE;
			break;
	case 'L':
	case 'G':
	case 'T':
	case 'A':
	case 'H':
        { uintptr_t size = memarea_limit(&s[1]);

	  if ( size == MEMAREA_INVALID_SIZE )
	    return -1;

	  switch(*s)
	  { case 'L':	GD->options.localSize    = size; goto next;
	    case 'G':	GD->options.globalSize   = size; goto next;
	    case 'T':	GD->options.trailSize    = size; goto next;
	    case 'A':	GD->options.argumentSize = size; goto next;
	    case 'H':	GD->options.heapSize     = size; goto next;
	  }
	}
      }
      s++;
    }
    next:;
  }

  return argc0-argc;
}


static void
replace_extension(char *path, const char *ext)
{ char *s = path + strlen(path);

  for( ; s > path; s-- )
  { if ( s[-1] == '.' )
    { strcpy(s, ext);
      return;
    }
    if ( s[-1] == '/' )
      break;
  }

  s += strlen(s);
  *s++ = '.';
  strcpy(s, ext);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find the resource database.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifndef BOOTFILE		/* normally delivered through config.h */
#if SIZEOF_VOIDP == 4
#define BOOTFILE "boot32.prc"
#else
#if SIZEOF_VOIDP == 8
#define BOOTFILE "boot64.prc"
#else
#define BOOTFILE "boot.prc"
#endif
#endif
#endif

RcArchive
openResourceDB(int argc, char **argv)
{ RcArchive rc;
  char *xfile = NULL;
  int flags = (GD->bootsession ? RC_WRONLY|RC_CREATE|RC_TRUNC : RC_RDONLY);
  char tmp[MAXPATHLEN];
  int n;

  if ( !GD->bootsession )
  {
#ifdef __WINDOWS__
    if ( GD->paths.module &&
	 !streq(GD->paths.module, GD->paths.executable) &&
	 (rc = rc_open_archive(GD->paths.module, flags)) )
      return rc;
#endif
    if ( (rc = rc_open_archive(GD->paths.executable, flags)) )
      return rc;
  }

  for(n=0; n<argc-1; n++)
  { if ( argv[n][0] == '-' && argv[n][2] == EOS ) /* -? */
    { if ( argv[n][1] == '-' )
	break;				/* trapped -- */
      if ( GD->bootsession )
      { if ( argv[n][1] == 'o' )
	{ xfile = argv[n+1];
	  break;
	}
      } else
      { if ( argv[n][1] == 'x' )
	{ xfile = argv[n+1];
	  break;
	}
      }
    }
  }

  if ( xfile )
  { if ( !(rc = rc_open_archive(xfile, flags)) )
      fatalError("Could not open resource database \"%s\": %s",
		 xfile, OsError());

    return rc;
  }

  strcpy(tmp, GD->paths.executable);
  replace_extension(tmp, "prc");

  if ( (rc=rc_open_archive(tmp, flags)) )
    return rc;

  if ( systemDefaults.home )
  { strcpy(tmp, systemDefaults.home);
    strcat(tmp, "/");
    strcat(tmp, BOOTFILE);

    return rc_open_archive(tmp, flags);
  }

  return NULL;
}


int
PL_is_initialised(int *argc, char ***argv)
{ if ( GD->initialised )
  { if ( argc )
      *argc = GD->cmdline.argc;
    if ( argv )
      *argv = GD->cmdline.argv;

    return TRUE;
  }

  return FALSE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Argument handling. This routine also takes care of a script-file launced
with the first line

#!/path/to/pl -L0 -f

On Unix this is passed as below.  We   need  to break the first argument
ourselves and we need to restore the argv[0]  path as pl might not be on
$PATH!

	{pl, '-L0 -f', <file>}

On some unix systems, currently MacOS X  tiger, the arguments are passed
as { pl, -L0, -f, <file> }, which   must be converted by adding -- after
the script file. This  mode  is   selected  if  SCRIPT_BREAKDOWN_ARGS is
defined by configure.

On Windows this is simply passed as below.   We have to analyse the file
ourselves. Unfortunately this needs to be done  in C as it might contain
stack-parameters.

	{plwin.exe <file>}
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifndef MAXLINE
#define MAXLINE 1024
#endif
#ifndef MAXARGV
#define MAXARGV 1024
#endif

static void
script_argv(int argc, char **argv)
{ FILE *fd;
  int i;

  DEBUG(1,
	{ for(i=0; i< argc; i++)
	    Sdprintf("argv[%d] = '%s'\n", i, argv[i]);
	});

#ifdef SCRIPT_BREAKDOWN_ARGS
  for(i=1; i < argc-1; i++)
  { if ( argv[i][0] == '-' && argv[i][2] == '\0' )
    { switch(argv[i][1])
      { case '-':
	  goto noscript;
	case 's':
	case 'f':
	  if ( (fd=fopen(argv[i+1], "r")) )
	  { char buf[MAXLINE];
	    char **av;
	    int j;

	    if ( !fgets(buf, sizeof(buf), fd) )
	    { fclose(fd);
	      goto noscript;
	    }
	    if ( !strprefix(buf, "#!") )
	    { fclose(fd);
	      goto noscript;
	    }
	    fclose(fd);

	    DEBUG(1, Sdprintf("Got script %s\n", argv[i+1]));

	    av = allocHeap(sizeof(char*)*(argc+2));
	    for(j=0; j<=i+1; j++)
	      av[j] = argv[j];
	    av[j] = "--";
	    for(; j<argc; j++)
	      av[j+1] = argv[j];
	    av[j+1] = NULL;
	    GD->cmdline.argc = argc+1;
	    GD->cmdline.argv = av;

	    return;
	  }
      }
    }
  }
#else /*SCRIPT_BREAKDOWN_ARGS*/

#ifdef __unix__
  if ( argc >= 3 &&
       (strpostfix(argv[1], "-f") || strpostfix(argv[1], "-s")) &&
       (fd = fopen(argv[2], "r")) )	/* ok, this is a script invocation */
#else
  if ( argc >= 2 &&
       (fd = fopen(argv[1], "r")) )
#endif
  { char buf[MAXLINE];
    char *s;
    char *av[MAXARGV];
    int  an = 0;

    fgets(buf, sizeof(buf), fd);
    if ( !strprefix(buf, "#!") )
    { fclose(fd);
      goto noscript;
    }

    for(s = &buf[2]; *s; )
    { while( *s && isBlank(*s) )
	s++;

      if ( *s )
      { char *start = s;
	char *o = s;

	while( *s && !isBlank(*s) )
	{ if ( *s == '\'' || *s == '"' )
	  { int c0 = *s++;

	    while(*s && *s != c0)
	      *o++ = *s++;
	    if ( *s )
	      s++;
	  } else
	    *o++ = *s++;
	}

#ifndef __unix__
	if ( an == 0 )
	{ av[an++] = argv[0];		/* the original interpreter */
	  if ( *o != '-' )		/* The interpreter */
	    continue;
	}
#endif
	av[an] = allocHeap(o-start+1);
	strncpy(av[an], start, o-start);
	av[an][o-start] = EOS;
	if ( ++an >= MAXARGV )
	  fatalError("Too many script arguments");
      }
    }
    if ( an+argc-2+2 > MAXARGV )	/* skip 2, add -- and NULL */
      fatalError("Too many script arguments");

#ifdef __unix__
    i = 2;
#else
    i = 1;
#endif
    av[an++] = argv[i++];		/* the script file */
    av[an++] = "--";			/* separate arguments */
    for(; i<argc; i++)
      av[an++] = argv[i];
    GD->cmdline.argc = an;
    av[an++] = NULL;
    GD->cmdline.argv = allocHeap(sizeof(char *) * an);
    memcpy(GD->cmdline.argv, av, sizeof(char *) * an);

    fclose(fd);
  } else
#endif /*SCRIPT_BREAKDOWN_ARGS*/
  { noscript:
    GD->cmdline.argc = argc;
    GD->cmdline.argv = argv;
  }
}


int
PL_initialise(int argc, char **argv)
{ int n;
  bool compile = FALSE;
  const char *rcpath = "<none>";

#if defined(_DEBUG) && defined(__WINDOWS__) && 0
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|
		 _CRTDBG_CHECK_CRT_DF|
		 //_CRTDBG_CHECK_ALWAYS_DF| 	/* very expensive */
		 //_CRTDBG_DELAY_FREE_MEM_DF|   /* does not reuse freed mem */
		 //_CRTDBG_LEAK_CHECK_DF|
		 0);
#endif

#if defined(HAVE_MTRACE) && defined(O_MAINTENANCE)
  if ( getenv("MALLOC_TRACE") )		/* glibc malloc tracer */
    mtrace();
#endif

  if ( GD->initialised )
  { succeed;
  }

  initPrologThreads();			/* initialise thread system */
  SinitStreams();			/* before anything else */

  GD->debug_level = 0;			/* 1-9: debug, also -d <level> */

  script_argv(argc, argv);		/* hande #! arguments */
  argc = GD->cmdline.argc;
  argv = GD->cmdline.argv;
  GD->cmdline._c_argc = -1;
  DEBUG(1,
  { int i;

    Sdprintf("argv =");
    for(i=0; i<argc; i++)
      Sdprintf(" %s", argv[i]);
    Sdprintf("\n");
  });

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FORCED_MALLOC_BASE is a debugging aid for  me   to  force  the system to
allocate memory starting from a specific   address.  Probably only works
properly on Linux. Don't bother with it.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef FORCED_MALLOC_BASE
  start_memory((void *)FORCED_MALLOC_BASE);
  Sdprintf("FORCED_MALLOC_BASE at 0x%08x\n", FORCED_MALLOC_BASE);
#endif
#if O_MALLOC_DEBUG
  malloc_debug(O_MALLOC_DEBUG);
#endif

  initOs();				/* Initialise OS bindings */
  initDefaults();			/* Initialise global defaults */
  initPaths(argc, (const char**)argv);	/* fetch some useful paths */

  setupGNUEmacsInferiorMode();		/* Detect running under EMACS */
#ifdef HAVE_SIGNAL
  setPrologFlagMask(PLFLAG_SIGNALS);	/* default: handle signals */
#endif

  if ( (GD->resourceDB = rc_open_archive(GD->paths.executable, RC_RDONLY)) )
  { rcpath = ((RcArchive)GD->resourceDB)->path;
    initDefaultOptions();
  }

  if ( !GD->resourceDB ||
       !streq(GD->options.saveclass, "runtime") )
  { int done;
    argc--; argv++;

    if ( argc == 1 && giveVersionInfo(argv[0]) ) /* -help, -v, etc */
    { exit(0);
    }

    for(n=0; n<argc; n++)		/* need to check this first */
    { if ( streq(argv[n], "--" ) )	/* --: terminates argument list */
	break;
      if ( streq(argv[n], "-b" ) )	/* -b: boot compilation */
      { GD->bootsession = TRUE;
	break;
      }
    }

    DEBUG(1, if (GD->bootsession) Sdprintf("Boot session\n"););

    if ( !GD->resourceDB )
    { if ( !(GD->resourceDB = openResourceDB(argc, argv)) )
      { fatalError("Could not find system resources");
      }
      rcpath = ((RcArchive)GD->resourceDB)->path;

      initDefaultOptions();
    }

    if ( (done = parseCommandLineOptions(argc, argv, &compile)) < 0 )
    { usage();
      fail;
    }
    argc -= done;
    argv += done;
  }

  setupProlog();
#ifdef O_PLMT
  aliasThread(PL_thread_self(), ATOM_main);
  enableThreads(TRUE);
#endif
  PL_set_prolog_flag("resource_database", PL_ATOM|FF_READONLY, rcpath);
  initialiseForeign(GD->cmdline.argc, /* PL_initialise_hook() functions */
		    GD->cmdline.argv);
  systemMode(TRUE);

  if ( GD->bootsession )
  { IOSTREAM *s = SopenRC(GD->resourceDB, "$state", "$prolog", RC_WRONLY);
    char *rcpathcopy = store_string(rcpath); /* rcpath is destroyed on close */

    if ( !compileFileList(s, argc, argv) )
    { PL_halt(1);
    }
    if ( Sclose(s) != 0 || !rc_save_archive(GD->resourceDB, NULL) )
    {
#ifdef __WINDOWS__
      PlMessage("Failed to save system resources: %s", rc_strerror(rc_errno));
#else
      Sfprintf(Serror,
	       "[ERROR: Failed to save system resources %s]\n",
	       rc_strerror(rc_errno));
#endif
      PL_halt(1);
    }
#ifdef __WINDOWS__
    PlMessage("Boot compilation has created %s", rcpathcopy);
#else
    Sfprintf(Serror,
	     "Boot compilation has created %s\n", rcpathcopy);
#endif
    PL_halt(0);
  } else
  { IOSTREAM *statefd = SopenRC(GD->resourceDB, "$state", "$prolog", RC_RDONLY);

    if ( statefd )
    { GD->bootsession = TRUE;
      if ( !loadWicFromStream(statefd) )
      { fail;
      }
      GD->bootsession = FALSE;

      Sclose(statefd);
    } else
    { fatalError("Resource database \"%s\" does not contain a saved state",
		 rcpath);
    }
  }

  debugstatus.styleCheck = (LONGATOM_CHECK|
			    SINGLETON_CHECK|
			    DISCONTIGUOUS_STYLE);
  systemMode(FALSE);
  GD->initialised = TRUE;
  registerForeignLicenses();

  DEBUG(1, Sdprintf("Starting Prolog Part of initialisation\n"));

  if ( compile )
  { int status = prologToplevel(PL_new_atom("$compile")) ? 0 : 1;

    PL_halt(status);
    fail;				/* make compile happy */
  } else
  { int status = prologToplevel(PL_new_atom("$initialise"));
    return status;
  }
}


typedef const char *cline;

static int
usage()
{ static const cline lines[] = {
    "%s: Usage:\n",
    "    1) %s --help     Display this message (also -h)\n",
    "    2) %s --version  Display version information (also -v)\n",
    "    3) %s --arch     Display architecture\n",
    "    4) %s --dump-runtime-variables[=format]\n"
    "                     Dump link info in sh(1) format\n",
    "    5) %s [options]\n",
    "    6) %s [options] [-o output] -c file ...\n",
    "    7) %s [options] [-o output] -b bootfile -c file ...\n",
    "Options:\n",
    "    -x state         Start from state (must be first)\n",
    "    -[LGTA]size[KMG] Specify {Local,Global,Trail,Argument} limits\n",
    "    -t toplevel      Toplevel goal\n",
    "    -g goal          Initialisation goal\n",
    "    -f file          User initialisation file\n",
    "    -F file          System initialisation file\n",
    "    -s file          Script source file\n",
    "    [+/-]tty         Allow tty control\n",
    "    -O               Optimised compilation\n",
    "    --nosignals      Do not modify any signal handling\n",
    "    --nodebug        Omit generation of debug info\n",
    "    --quiet          Quiet operation (also -q)\n",
    "    --home=DIR       Use DIR as SWI-Prolog home\n",
    NULL
  };
  const cline *lp = lines;
  char *prog;

  if ( GD->cmdline.argc > 0 )
    prog = BaseName(GD->cmdline.argv[0]);
  else
    prog = "pl";

  for(lp = lines; *lp; lp++)
    Sfprintf(Serror, *lp, prog);

  return TRUE;
}

static int
version()
{ Sprintf("SWI-Prolog version %d.%d.%d for %s\n",
	  PLVERSION / 10000,
	  (PLVERSION / 100) % 100,
	  PLVERSION % 100,
	  ARCH);

  return TRUE;
}


static int
arch()
{ Sprintf("%s\n", ARCH);

  return TRUE;
}

#define FMT_SH 1			/* Unix sh: name="value" */
#define FMT_CMD 2			/* Windows cmd.exe: set name=value */

static void
printvar(const char *name, const char *value, int format)
{ switch(format)
  { case FMT_SH:
      Sprintf("%s=\"%s\";\n", name, value);
      break;
    case FMT_CMD:
      Sprintf("SET %s=%s\n", name, value);
      break;
    default:
      assert(0);
  }
}


static int
runtime_vars(int format)
{ char *home;
#ifdef O_XOS
  char base[MAXPATHLEN];
#endif
  char version[20];

  if ( systemDefaults.home )
  {
#ifdef O_XOS
    if ( format == FMT_CMD )
    { _xos_os_filename(systemDefaults.home, base, MAXPATHLEN);
      home = base;
    } else
      home = systemDefaults.home;
#else
    home = systemDefaults.home;
#endif
  } else
  { home = "<no home>";
  }

  Ssprintf(version, "%d", PLVERSION);

  printvar("CC",	C_CC, format);
  printvar("PLBASE",	home, format);
  printvar("PLARCH",	ARCH, format);
  printvar("PLLIBS",	C_LIBS, format);
  printvar("PLLIB",	C_PLLIB, format);
  printvar("PLCFLAGS",  C_CFLAGS, format);
  printvar("PLLDFLAGS", C_LDFLAGS, format);
#ifdef SO_EXT
  printvar("PLSOEXT",	SO_EXT, format);
#endif
  printvar("PLVERSION", version, format);
#if defined(HAVE_DLOPEN) || defined(HAVE_SHL_LOAD) || defined(EMULATE_DLOPEN)
  printvar("PLSHARED",	"yes", format);
#else
  printvar("PLSHARED",	"no", format);
#endif
#ifdef O_PLMT
  printvar("PLTHREADS", "yes", format);
#else
  printvar("PLTHREADS", "no", format);
#endif

  return TRUE;
}


static int
giveVersionInfo(const char *a)
{ if ( *a != '-' )
    return FALSE;

  if ( streq(a, "-help") || streq(a, "--help") || streq(a, "-h") )
    return usage();
  if ( streq(a, "-arch") || streq(a, "--arch") )
    return arch();
  if ( streq(a, "--version") || streq(a, "-v") )
    return version();

  if ( a[1] == '-' )			/* allow for --name versions */
    a++;

  if ( streq(a, "-dump-runtime-variables") )
    return runtime_vars(FMT_SH);
  if ( streq(a, "-dump-runtime-variables=sh") )
    return runtime_vars(FMT_SH);
  if ( streq(a, "-dump-runtime-variables=cmd") )
    return runtime_vars(FMT_CMD);

  return FALSE;
}


		 /*******************************
		 *	     CLEANUP		*
		 *******************************/

typedef void (*halt_function)(int, void*);

struct on_halt
{ halt_function	function;
  void*		argument;
  OnHalt	next;
};


void
PL_on_halt(halt_function f, void *arg)
{ if ( !GD->os.halting )
  { OnHalt h = allocHeap(sizeof(struct on_halt));

    h->function = f;
    h->argument = arg;
    startCritical;
    h->next = GD->os.on_halt_list;
    GD->os.on_halt_list = h;
    endCritical;
  }
}


int
PL_cleanup(int rval)
{ OnHalt h;

  LOCK();
  if ( GD->cleaning != CLN_NORMAL )
  { UNLOCK();
    return FALSE;
  }
#ifdef O_PLMT
  if ( PL_thread_self() != 1 )
  { UNLOCK();
    return FALSE;
  }
#endif

  GD->cleaning = CLN_ACTIVE;

  pl_notrace();				/* avoid recursive tracing */
#ifdef O_PROFILE
  resetProfiler();			/* don't do profiling anymore */
#endif
#ifdef O_PLMT
  exitPrologThreads();
#endif

  Scurout = Soutput;			/* reset output stream to user */

  GD->cleaning = CLN_PROLOG;

  qlfCleanup();				/* remove errornous .qlf files */
  if ( GD->initialised )
  { fid_t cid = PL_open_foreign_frame();
    predicate_t proc = PL_predicate("$run_at_halt", 0, "system");

    DEBUG(5, Sdprintf("Running at_halt hooks\n"));
    PL_call_predicate(MODULE_system, FALSE, proc, 0);
    PL_discard_foreign_frame(cid);
  }

  GD->cleaning = CLN_FOREIGN;

					/* run PL_on_halt() hooks */
  for(h = GD->os.on_halt_list; h; h = h->next)
    (*h->function)(rval, h->argument);

#ifdef __WINDOWS__
  if ( rval != 0 && !hasConsole() )
    PlMessage("Exit status is %d", rval);
#endif

  dieIO();				/* streams may refer to foreign code */
					/* Standard I/O is only flushed! */

  GD->cleaning = CLN_SHARED;

  if ( GD->initialised )
  { fid_t cid = PL_open_foreign_frame();
    predicate_t proc = PL_predicate("unload_all_foreign_libraries", 0,
				    "shlib");
    if ( isDefinedProcedure(proc) )
      PL_call_predicate(MODULE_system, FALSE, proc, 0);
    PL_discard_foreign_frame(cid);
  }

  GD->cleaning = CLN_DATA;

  RemoveTemporaryFiles();

  if ( GD->resourceDB )
  { rc_close_archive(GD->resourceDB);
    GD->resourceDB = NULL;
  }

  cleanupSignals();
  freeStacks(PASS_LD1);
#ifdef HAVE_DMALLOC_H
  dmalloc_verify(0);
#endif
  freeLocalData(LD);
  cleanupSourceFiles();
  cleanupAtoms();
  cleanupFunctors();
  cleanupArith();
  cleanupInitialiseHooks();
  cleanupExtensions();
  cleanupOs();
  Scleanup();
#ifdef O_PLMT
  cleanupThreads();
#endif
  cleanupForeign();
  cleanupCodeToAtom();
  cleanupMemAlloc();

  UNLOCK();				/* requires GD->thread.enabled */

  memset(&PL_global_data, 0, sizeof(PL_global_data));
  memset(&PL_local_data,  0, sizeof(PL_local_data));

  return TRUE;
}

		 /*******************************
		 *	ERRORS AND WARNINGS	*
		 *******************************/

#include <stdarg.h>

bool
sysError(const char *fm, ...)
{ va_list args;

  va_start(args, fm);
  vsysError(fm, args);
  va_end(args);

  PL_fail;
}


void
fatalError(const char *fm, ...)
{ va_list args;

  va_start(args, fm);
  vfatalError(fm, args);
/*va_end(args);*/
}


bool
warning(const char *fm, ...)
{ va_list args;

  va_start(args, fm);
  vwarning(fm, args);
  va_end(args);

  PL_fail;
}


static bool
vsysError(const char *fm, va_list args)
{ static int active = 0;

  if ( active++ )
    PL_halt(3);

#ifdef O_PLMT
  Sfprintf(Serror, "[PROLOG SYSTEM ERROR:  Thread %d\n\t",
	   PL_thread_self());
#else
  Sfprintf(Serror, "[PROLOG SYSTEM ERROR:\n\t");
#endif
  Svfprintf(Serror, fm, args);
  if ( gc_status.active )
  { Sfprintf(Serror,
	    "\n[While in %ld-th garbage collection]\n",
	    gc_status.collections);
  }

#if defined(O_DEBUGGER)
  systemMode(TRUE);
  Sfprintf(Serror, "\n\nPROLOG STACK:\n");
  backTrace(NULL, 10);
  Sfprintf(Serror, "]\n");
#endif /*O_DEBUGGER*/

  if ( GD->bootsession )
    PL_halt(1);

action:
#ifdef HAVE_GETPID
  Sfprintf(Serror, "\n[pid=%d] Action? ", getpid());
#else
  Sfprintf(Serror, "\nAction? ");
#endif
  Sflush(Soutput);
  ResetTty();

  switch(getSingleChar(Sinput, FALSE))
  { case 'a':
      pl_abort(ABORT_FATAL);
      break;
    case EOF:
      Sfprintf(Serror, "EOF: exit\n");
    case 'e':
      PL_halt(3);
      break;
    default:
      Sfprintf(Serror,
	       "Unknown action.  Valid actions are:\n"
	      "\ta\tabort to toplevel\n"
	      "\te\texit Prolog\n");
      goto action;
  }

  pl_abort(ABORT_FATAL);
  PL_halt(3);
  PL_fail;
}


void
vfatalError(const char *fm, va_list args)
{
#ifdef __WINDOWS__
  char msg[500];
  Ssprintf(msg, "[FATAL ERROR:\n\t");
  Svsprintf(&msg[strlen(msg)], fm, args);
  Ssprintf(&msg[strlen(msg)], "]");

  PlMessage(msg);
#else
  Sfprintf(Serror, "[FATAL ERROR:\n\t");
  Svfprintf(Serror, fm, args);
  Sfprintf(Serror, "]\n");
#endif

  PL_halt(2);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
One day, warning() should be replaced by   PL_error()  or direct call to
print_message/2. For now we make warning call print_message/2, so we can
move the rest of the warnings gradually. For this reason we make a term

	message_lines(ListOfLines)

Where ListOfLines is a list of string objects.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool
vwarning(const char *fm, va_list args)
{ toldString();				/* play safe */

  if ( truePrologFlag(PLFLAG_REPORT_ERROR) )
  { if ( !GD->bootsession && GD->initialised &&
	 !LD->outofstack && 		/* cannot call Prolog */
	 !fm[0] == '$')			/* explicit: don't call Prolog */
    { char message[LINESIZ];
      char *s = message;
      fid_t cid   = PL_open_foreign_frame();
      term_t av   = PL_new_term_refs(2);
      term_t tail = PL_copy_term_ref(av+1);
      term_t head = PL_new_term_ref();

      Svsprintf(message, fm, args);

      for(;;)
      { char *eol = strchr(s, '\n');

	if ( eol )
	{ PL_unify_list(tail, head, tail);
	  PL_unify_string_nchars(head, eol-s, s);
	  s = eol+1;
	} else
	{ if ( *s )
	  { PL_unify_list(tail, head, tail);
	    PL_unify_string_chars(head, s);
	  }
	  PL_unify_nil(tail);
	  break;
	}
      }
      PL_cons_functor(av+1, FUNCTOR_message_lines1, av+1);
      PL_put_atom(av, ATOM_error);	/* error? */

      PL_call_predicate(NULL, PL_Q_NODEBUG, PROCEDURE_print_message2, av);
      PL_discard_foreign_frame(cid);
    } else
    { Sfprintf(Suser_error, "ERROR: ");
      Svfprintf(Suser_error, fm, args);
      Sfprintf(Suser_error, "\n");
      Pause(0.5);
    }
  }

  if ( !ReadingSource && truePrologFlag(PLFLAG_DEBUG_ON_ERROR) )
    pl_trace();

  PL_fail;
}
