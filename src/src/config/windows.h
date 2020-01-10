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

/* config.h.  Generated automatically by configure.  */
/* config.h.in.  Generated automatically from configure.in by autoheader.  */

#define __WIN32__ 1
#if !defined(VC8) && (_MSC_VER >= 1400)	/* Visual studio 8 */
#define VC8 1				/* (aka Microsoft 2005 VC++ */
#endif

typedef unsigned long sigset_t;		/* we don't have this */
typedef int mode_t;
#define HAVE_SIGSET_T 1			/* For the future */

#define NOTTYCONTROL TRUE		/* default -tty */
#define O_ASYNC_HOOK 1
#define NO_SEGV_HANDLING 1		/* at least, when making a DLL */
#define COPY_ATOMS_TO_HEAP 1		/* place in DLL isn't known */

#define OPEN_MAX 32

#define HAVE_UXNT_H 1
#define HAVE_MALLOC_H 1
#define HAVE_VIRTUALALLOC 1
#define HAVE_CLOCK 1			/* clock() timing function */
#define inline __inline

/* Define if you want to associate states */
#define ASSOCIATE_STATE "qlx"
#define ASSOCIATE_SRC	"pl"

#ifdef O_GMP
#define HAVE_GMP_H 1
#define HAVE_LIB_GMP 1
#endif

#ifdef __LCC__
#define NO_MS_EXTENSIONS 1
#else
#define HAVE___TRY 1
#endif

/* Define to enable life-data marking by scanning VM instructions */
#define LIFE_GC 1

/* Define for emulating dlopen(), etc. using LoadLibrary */
#define EMULATE_DLOPEN 1

/* Define to extension used for shared objects if not "so" */
#define SO_EXT "dll"

/* Define for the _xos_... functions */
#define O_XOS 1

/* Define O_RLC for the ../readline library */
#define O_RLC 1

/* Define for Windows DDE support */
#define O_DDE 1

/* Define for Windows DLL support */
#define O_DLL 1

/* Define if you disk-drives are special to you (DOS, Windows, OS/2) */
#define O_HASDRIVES 1

/* Define if you have shares using the notation //host/share */
#define O_HASSHARES 1

/* Maximum length of a path-name.  Note XOS! */
#define MAXPATHLEN 512

/* Define if you have <sys/wait.h> that is POSIX.1 compatible.  */
#undef HAVE_SYS_WAIT_H

/* Define if you have <vfork.h>.  */
#undef HAVE_VFORK_H

/* Define as __inline if that's what the C compiler calls it.  */
/* #undef inline */

/* Define to `int' if <sys/types.h> doesn't define.  */
/* #undef pid_t */

/* Define if you need to in order for stat and other things to work.  */
/* #undef _POSIX_SOURCE */

/* Define as the return type of signal handlers (int or void).  */
#define RETSIGTYPE void

/* If using the C implementation of alloca, define if you know the
   direction of stack growth for your system; otherwise it will be
   automatically deduced at run-time.
	STACK_DIRECTION > 0 => grows toward higher addresses
	STACK_DIRECTION < 0 => grows toward lower addresses
	STACK_DIRECTION = 0 => direction of growth unknown
 */
#define STACK_DIRECTION -1

/* Define if you have the ANSI C header files.  */
#define STDC_HEADERS 1

/* Define if you can safely include both <sys/time.h> and <time.h>.  */
#undef TIME_WITH_SYS_TIME

/* Define vfork as fork if vfork does not work.  */
/* #undef vfork */

/* Define if BSD compatible signals (i.e. no reset when fired) */
/* #undef BSD_SIGNALS */

/* Define if your processor stores words with the most significant
   byte first (like Motorola and SPARC, unlike Intel and VAX).  */
/* #undef WORDS_BIGENDIAN */

/* Define if mmap() can be used to allocate stacks */
#undef MMAP_STACK

/* Define if maximum address we can map at */
#undef MMAP_MAX_ADDRESS

/* Define if minimum address we can map at if > sbrk(0) */
/* #undef MMAP_MIN_ADDRESS */

/* Define if uchar is not defined in <sys/types.h> */
#define NEED_UCHAR 1

/* Define if SIGPROF and setitimer() are available */
#define O_PROFILE 1

/* Define if signal handler is of the form f(sig, type, context, addr) */
/* #undef SIGNAL_HANDLER_PROVIDES_ADDRESS */

/* Define if (type)var = value is allowed */
#undef TAGGED_LVALUE

/* Define as 0 if text addresses start above 40K */
/* #undef VMCODE_IS_ADDRESS */

/* Define if first data symbol not is environ */
/* #undef FIRST_DATA_SYMBOL */

/* Define if pl-save.c works */
#undef O_SAVE

/* Define how to reset stdin after a restore */
#undef RESET_STDIN

/* Define if symbolic links are supported by the OS */
#undef HAVE_SYMLINKS

/* Define if AIX foreign language interface is to be used */
/* #undef O_AIX_FOREIGN */

/* Define if MACH foreign language interface is to be used */
/* #undef O_MACH_FOREIGN */

/* Define if BSD Unix ld -A foreign language interface is to be used */
#undef O_FOREIGN

/* Define if ld accepts -A option */
#undef HAVE_LD_A

/* Define if /dev/null is named differently */
/* #undef DEVNULL */

/* Define if wait() uses union wait */
/* #undef UNION_WAIT */

/* Define if <sys/ioctl> should *not* be included after <sys/termios.h> */
/* #undef NO_SYS_IOCTL_H_WITH_SYS_TERMIOS_H */

/* Define if, in addition to <errno.h>, extern int errno; is needed */
/* #undef NEED_DECL_ERRNO */

/* Define to "file.h" to include additional system prototypes */
/* #undef SYSLIB_H */

/* The number of bytes in a int.  */
#define SIZEOF_INT 4

/* The number of bytes in a long.  */
#define SIZEOF_LONG 4

/* Define if you have the access function.  */
#define HAVE_ACCESS 1

/* Define if you have the chmod function.  */
#define HAVE_CHMOD 1

/* we have fcntl() and it supports F_SETLKW */
#undef FCNTL_LOCKS

/* Define if you have the dlopen function.  */
/* #undef HAVE_DLOPEN */

/* Define if you have the dossleep function.  */
#undef HAVE_DOSSLEEP

/* Define if you have the delay function.  */
#undef HAVE_DELAY

/* Define if you have the fstat function.  */
#define HAVE_FSTAT 1

/* Define if you have the getcwd function.  */
#define HAVE_GETCWD 1

/* Define if you have the getpid function.  */
#define HAVE_GETPID 1

/* Define if you have the getdtablesize function.  */
#undef HAVE_GETDTABLESIZE

/* Define if you have the getpagesize function.  */
#undef HAVE_GETPAGESIZE

/* Define if you have the getpwnam function.  */
#undef HAVE_GETPWNAM

/* Define if you have the getrlimit function.  */
#undef HAVE_GETRLIMIT

/* Define if you have the gettimeofday function.  */
#undef HAVE_GETTIMEOFDAY

/* Define if you have the ftime function.  */
#define HAVE_FTIME 1

/* Define if you have the getw function.  */
#undef HAVE_GETW

/* Define if you have the memmove function.  */
#define HAVE_MEMMOVE 1

/* Define if you have the opendir function.  */
#define HAVE_OPENDIR 1

/* Define if you have the popen function.  */
#define HAVE_POPEN 1

/* Define if you have the putenv function.  */
#define HAVE_PUTENV 1

/* Define if you have the random function.  */
#undef HAVE_RANDOM

/* Define if you have the readlink function.  */
#undef HAVE_READLINK

/* Define if you have the remove function.  */
#define HAVE_REMOVE 1

/* Define if you have the rename function.  */
#define HAVE_RENAME 1

/* Define if you have the stricmp() function. */
#define HAVE_STRICMP 1

/* Define if you have the mbscasecoll() function. */
#define mbcasescoll mbsicoll
#define HAVE_MBCASESCOLL 1

/* Define if you have the strlwr() function */
#define HAVE_STRLWR 1

/* Define if you have the rl_insert_close function.  */
#define HAVE_RL_INSERT_CLOSE 1

/* Define if you have the select function.  */
#define HAVE_SELECT 1

/* Define if you have the signal function.  */
#define HAVE_SIGNAL 1

/* Define if you have the sleep function.  */
#undef HAVE_SLEEP

/* Define if you have the srand function.  */
#define HAVE_SRAND 1

/* Define if you have the srandom function.  */
#undef HAVE_SRANDOM

/* Define if you have the stat function.  */
#define HAVE_STAT 1

/* Define if you have the strerror function.  */
#define HAVE_STRERROR 1

#define HAVE_CEIL  1
#define HAVE_FLOOR 1

/* Define if you have the tgetent function.  */
#undef HAVE_TGETENT

/* Define if you have the times function.  */
#undef HAVE_TIMES

/* Define if you have the <dirent.h> header file.  */
#define HAVE_DIRENT_H 1

/* Define if you have the <malloc.h> header file.  */
#define HAVE_MALLOC_H 1

/* Define if you have the <memory.h> header file.  */
#define HAVE_MEMORY_H 1

/* Define if you have the <ndir.h> header file.  */
/* #undef HAVE_NDIR_H */

/* Define if you have the <pwd.h> header file.  */
#undef HAVE_PWD_H

/* Define if you have the <string.h> header file.  */
#define HAVE_STRING_H 1

/* Define if you have the <sys/dir.h> header file.  */
/* #undef HAVE_SYS_DIR_H */

/* Define if you have the <sys/file.h> header file.  */
#undef HAVE_SYS_FILE_H

/* Define if you have the <sys/ndir.h> header file.  */
/* #undef HAVE_SYS_NDIR_H */

/* Define if you have the <sys/param.h> header file.  */
#undef HAVE_SYS_PARAM_H

/* Define if you have the <sys/resource.h> header file.  */
#undef HAVE_SYS_RESOURCE_H

/* Define if you have the <sys/select.h> header file.  */
/* #undef HAVE_SYS_SELECT_H */

/* Define if you have the <sys/stat.h> header file.  */
#define HAVE_SYS_STAT_H 1

/* Define if you have the <sys/termios.h> header file.  */
#undef HAVE_SYS_TERMIOS_H

/* Define if you have the <sys/time.h> header file.  */
#undef HAVE_SYS_TIME_H

/* Define if you have the <unistd.h> header file.  */
#undef HAVE_UNISTD_H

/* Define if you have the dl library (-ldl).  */
/* #undef HAVE_LIBDL */

/* Define if you have the elf library (-lelf).  */
/* #undef HAVE_LIBELF */

/* Define if you have the m library (-lm).  */
#define HAVE_LIBM 1

/* Define if you have the readline library (-lreadline).  */
/* #define HAVE_LIBREADLINE */

/* Define if you have the termcap library (-ltermcap).  */
#undef HAVE_LIBTERMCAP

/* Define if you have the ucb library (-lucb).  */
/* #undef HAVE_LIBUCB */

/* Define to make use of standard (UNIX98) pthread recursive mutexes */
#define RECURSIVE_MUTEXES 1

/* Define if pthread has pthread_mutexattr_settype() */
#define HAVE_PTHREAD_MUTEXATTR_SETTYPE 1

/* Format for int64_t */
#define INT64_FORMAT "%I64d"

/* Define to 1 if you have the <locale.h> header file. */
#define HAVE_LOCALE_H 1

/* Define to 1 if you have the `setlocale' function. */
#define HAVE_SETLOCALE 1

/* Define to 1 if you have `isnan' function */
#define HAVE_ISNAN 1

/* Define to 1 if you have `isinf' function */
/*#define HAVE_ISINF 1*/

/* Define to 1 if you have `_fpclass' function */
#define HAVE__FPCLASS 1

/* Define to 1 if you have <float.h> header */
#define HAVE_FLOAT_H 1

/* setenv comes from uxnt.c */
#define HAVE_SETENV 1

/* Define to 1 if you have the 'wcsxfrm' function. */
#define HAVE_WCSXFRM 1
