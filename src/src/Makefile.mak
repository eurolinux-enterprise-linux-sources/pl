################################################################
# Makefile for SWI-Prolog on MS-Windows
#
# Author:	Jan Wielemaker
#		J.Wielemaker@uva.nl
#		HCS (formerly SWI)
#		University of Amsterdam
#    		Kruislaan 419
#		1098 VA  Amsterdam
#		The Netherlands
#
# Public targets:
# 
#	* make			Simply makes all programs in the current tree
#	* make install		Installs the libraries and public executables
#	* make install-arch	Install machine dependent files
#	* make install-libs	Install machine independent files
#
# Copyright (C) University of Amsterdam
# 
# Copyright policy:
#	
#	* LGPL (see file COPYING or http://www.gnu.org/)
################################################################

STACK=4000000

PLHOME=..
!include rules.mk

PL=pl
PLCON=$(PLHOME)\bin\plcon.exe
PLWIN=$(PLHOME)\bin\plwin.exe
PLLD=$(PLHOME)\bin\plld.exe
PLRC=$(PLHOME)\bin\plrc.exe
PLDLL=$(PLHOME)\bin\libpl.dll
TERMDLL=$(PLHOME)\bin\plterm.dll
OUTDIRS=$(PLHOME)\bin $(PLHOME)\lib $(PLHOME)\include

LOCALLIB=$(UXLIB) rc/rc.lib libtai/tai.lib

PB=$(PLHOME)\boot
INCLUDEDIR=$(PLHOME)\include
CINCLUDE=$(INCLUDEDIR)\SWI-Prolog.h
STREAMH=$(INCLUDEDIR)\SWI-Stream.h
STARTUPPATH=$(PLHOME)\$(PLBOOTFILE)
LIBRARYDIR=$(PLBASE)\library

OBJ=	pl-atom.obj pl-wam.obj pl-stream.obj pl-error.obj pl-arith.obj \
	pl-bag.obj pl-comp.obj pl-rc.obj pl-dwim.obj pl-ext.obj \
	pl-file.obj pl-flag.obj pl-fmt.obj pl-funct.obj pl-gc.obj \
	pl-glob.obj pl-privitf.obj pl-list.obj pl-load.obj pl-modul.obj \
	pl-op.obj pl-os.obj pl-prims.obj pl-pro.obj pl-proc.obj \
	pl-prof.obj pl-read.obj pl-rec.obj pl-rl.obj pl-setup.obj \
	pl-sys.obj pl-table.obj pl-trace.obj pl-util.obj pl-wic.obj \
	pl-write.obj pl-term.obj pl-buffer.obj pl-thread.obj \
	pl-xterm.obj pl-prologflag.obj pl-ctype.obj pl-main.obj \
	pl-dde.obj pl-nt.obj pl-attvar.obj pl-gvar.obj pl-btree.obj \
	pl-utf8.obj pl-text.obj pl-mswchar.obj pl-gmp.obj pl-tai.obj \
	pl-segstack.obj pl-hash.obj pl-version.obj pl-codetable.obj \
	pl-supervisor.obj pl-option.obj pl-files.obj

PLINIT=	$(PB)/init.pl

INCSRC=	pl-index.c pl-alloc.c pl-fli.c
SRC=	$(OBJ:.o=.c) $(DEPOBJ:.o=.c) $(EXT:.o=.c) $(INCSRC)
HDR=	config.h parms.h pl-buffer.h pl-ctype.h pl-incl.h SWI-Prolog.h \
	pl-main.h pl-os.h pl-data.h
VMI=	pl-jumptable.ic pl-codetable.c pl-vmi.h

PLSRC=	../boot/syspred.pl ../boot/toplevel.pl ../boot/license.pl \
	../boot/bags.pl ../boot/apply.pl \
	../boot/writef.pl ../boot/history.pl \
	../boot/dwim.pl ../boot/rc.pl \
	../boot/parms.pl ../boot/autoload.pl ../boot/qlf.pl \
	../boot/topvars.pl ../boot/messages.pl ../boot/load.pl ../boot/menu.pl
PLWINLIBS=	wise.pl dde.pl progman.pl registry.pl
PLLIBS= MANUAL helpidx.pl help.pl explain.pl sort.pl \
	qsave.pl shlib.pl statistics.pl system.pl \
	backcomp.pl gensym.pl listing.pl debug.pl vm.pl error.pl \
	bim.pl quintus.pl edinburgh.pl ctypes.pl files.pl \
	edit.pl emacs_interface.pl shell.pl check.pl ugraphs.pl \
	tty.pl readln.pl readutil.pl make.pl hotfix.pl option.pl date.pl \
	am_match.pl oset.pl ordsets.pl occurs.pl lists.pl heaps.pl \
	www_browser.pl url.pl utf8.pl main.pl win_menu.pl assoc.pl nb_set.pl \
	qpforeign.pl dif.pl when.pl prolog_stack.pl prolog_clause.pl \
	prolog_xref.pl checklast.pl checkselect.pl operators.pl \
	prolog_source.pl broadcast.pl pairs.pl base64.pl record.pl \
	rbtrees.pl settings.pl dialect.pl apply_macros.pl apply.pl \
	nb_rbtrees.pl aggregate.pl pure_input.pl pio.pl terms.pl \
	charsio.pl \
	$(PLWINLIBS)
!IF "$(MT)" == "true"
PLLIBS=$(PLLIBS) threadutil.pl thread.pl thread_pool.pl
!ENDIF
CLP=	bounds.pl clp_events.pl clp_distinct.pl simplex.pl clpfd.pl
COMMON=	
DIALECT=yap.pl hprolog.pl
YAP=	README.TXT
ISO=	iso_predicates.pl
UNICODE=blocks.pl unicode_data.pl
MANDIR= "$(PLBASE)\doc\Manual"

all:	lite packages

lite:	banner \
	headers	swipl subdirs vmi \
	$(PLCON) startup index $(PLWIN) $(PLLD) \
	dlldemos

plcon:	$(PLCON)
plwin:	$(PLWIN)
plld:	$(PLLD)

system:		$(PLCON)
startup:	$(STARTUPPATH)
headers:	$(CINCLUDE) $(STREAMH)

banner:
		@echo ****************
		@echo Making SWI-Prolog $(PLVERSION) for $(ARCH)
		@echo To be installed in $(PLBASE)
!IF "$(DBG)" == "true"
		@echo *** Compiling version for DEBUGGING
!ENDIF
!IF "$(MT)" == "true"
		@echo *** Building MULTI-Threading version
!ENDIF
		@echo ****************

$(PLLIB):	$(OBJ) $(LOCALLIB)
		$(LD) $(LDFLAGS) /dll /out:$(PLDLL) /implib:$@ $(OBJ) $(LOCALLIB) $(GMPLIB) $(LIBS) winmm.lib $(DBGLIBS)

$(PLCON):	$(PLLIB) pl-ntcon.obj
		$(LD) $(LDFLAGS) /subsystem:console /out:$@ pl-ntcon.obj $(PLLIB)
		editbin /stack:$(STACK) $(PLCON)

$(PLWIN):	$(PLLIB) pl-ntmain.obj pl.res
		$(LD) $(LDFLAGS) /subsystem:windows /out:$@ pl-ntmain.obj $(PLLIB) $(TERMLIB) pl.res $(LIBS)
		editbin /stack:$(STACK) $(PLWIN)

pl.res:		pl.rc pl.ico xpce.ico
		$(RSC) /fo$@ pl.rc

$(STARTUPPATH):	$(PLINIT) $(PLSRC) $(PLCON)
		$(PLCON) -O -o $(STARTUPPATH) -b $(PLINIT)

$(OUTDIRS):
		if not exist "$@/$(NULL)" $(MKDIR) "$@"

subdirs:	$(OUTDIRS)
		chdir win32\uxnt & $(MAKE)
		chdir win32\console & $(MAKE)
		chdir rc & $(MAKE)
		chdir libtai & $(MAKE)

index:
		$(PLCON) -x $(STARTUPPATH) \
			-f none -F none \
			-g make_library_index('../library') \
			-t halt

$(CINCLUDE):	$(OUTDIRS) SWI-Prolog.h
		copy SWI-Prolog.h $@

$(STREAMH):	SWI-Stream.h $(INCLUDEDIR)
		copy SWI-Stream.h $@

$(OBJ):		pl-vmi.h
pl-funct.obj:	pl-funct.ih
pl-atom.obj:	pl-funct.ih
pl-wam.obj:	pl-vmi.c pl-alloc.c pl-index.c pl-fli.c pl-jumptable.ic
pl-stream.obj:	popen.c

# this should be pl-vmi.h, but that causes a recompile of everything.
# Seems NMAKE dependency computation is broken ...
vmi:		pl-vmi.c mkvmi.exe
		mkvmi.exe
		echo "ok" > vmi
		
pl-funct.ih:	ATOMS defatom.exe
		defatom.exe

pl-atom.ih:	ATOMS defatom.exe
		defatom.exe 

defatom.exe:	defatom.obj
		$(LD) /out:$@ /subsystem:console defatom.obj $(LIBS)
		
mkvmi.exe:	mkvmi.obj
		$(LD) /out:$@ /subsystem:console mkvmi.obj $(LIBS)
		
$(PLLD):	plld.obj
		$(LD) /out:$@ /subsystem:console plld.obj $(LIBS)

tags:		TAGS

TAGS:		$(SRC)
		$(ETAGS) $(SRC) $(HDR)

swipl:
		echo . > $@

check:
		$(PLCON) -f test.pl -F none -g test,halt -t halt(1)

################################################################
# Installation.  The default target is dv-install to install the
# normal development version
################################################################

!IF "$(CFG)" == "rt"
install:	$(BINDIR) iprog install_packages
!ELSE
install:	install-arch install-libs install-readme install_packages \
		xpce_packages install-dotfiles install-demo html-install 
!ENDIF

install-arch:	idirs iprog
		$(INSTALL_PROGRAM) $(PLLD)  "$(BINDIR)"
		$(INSTALL_PROGRAM) $(PLRC)  "$(BINDIR)"
		$(INSTALL_PROGRAM) ..\bin\plregtry.dll  "$(BINDIR)"
		$(INSTALL_PROGRAM) ..\bin\dlltest.dll  "$(BINDIR)"
		$(INSTALL_DATA) $(PLLIB) "$(LIBDIR)"
		$(INSTALL_DATA) $(TERMLIB) "$(LIBDIR)"

iprog::
		$(INSTALL_PROGRAM) $(PLWIN) "$(BINDIR)"
		$(INSTALL_PROGRAM) $(PLCON) "$(BINDIR)"
		$(INSTALL_PROGRAM) $(PLDLL) "$(BINDIR)"
		$(INSTALL_PROGRAM) $(TERMDLL) "$(BINDIR)"
!IF "$(PDB)" == "true"
		$(INSTALL_PROGRAM) ..\bin\plwin.pdb "$(BINDIR)"
		$(INSTALL_PROGRAM) ..\bin\plcon.pdb "$(BINDIR)"
		$(INSTALL_PROGRAM) ..\bin\libpl.pdb "$(BINDIR)"
		$(INSTALL_PROGRAM) ..\bin\plterm.pdb "$(BINDIR)"
!ENDIF
!IF "$(MT)" == "true"
		@echo Installing pthreadVC.dll
		$(INSTALL_PROGRAM) "$(EXTRALIBDIR)\$(LIBPTHREAD).dll" "$(BINDIR)"
		$(INSTALL_DATA) "$(EXTRALIBDIR)\$(LIBPTHREAD).lib" "$(LIBDIR)"
!ENDIF
!IF "$(MSVCRT)" != ""
		@echo Adding MSVC runtime
		$(INSTALL_PROGRAM) "$(MSVCRTDIR)\$(MSVCRT)" "$(BINDIR)"
!ENDIF

install-libs:	idirs iinclude iboot ilib
		$(INSTALL_DATA) $(STARTUPPATH) "$(PLBASE)\$(BOOTFILE)"
		$(INSTALL_DATA) swipl "$(PLBASE)\swipl"
		chdir "$(PLBASE)\library" & \
		   $(PLCON) \
			-f none \
			-g make_library_index('.') \
			-t halt

install-demo:	idirs
		$(INSTALL_DATA) ..\demo\likes.pl "$(PLBASE)\demo"
		$(INSTALL_DATA) ..\demo\README "$(PLBASE)\demo\README.TXT"

IDIRS=		"$(BINDIR)" "$(LIBDIR)" "$(PLBASE)\include" \
		"$(PLBASE)\boot" "$(PLBASE)\library" "$(PKGDOC)" \
		"$(PLCUSTOM)" "$(PLBASE)\demo" "$(PLBASE)\library\clp" \
		"$(PLBASE)\library\common" \
		"$(PLBASE)\library\dialect" "$(PLBASE)\library\dialect\yap" \
		"$(PLBASE)\library\dialect\iso" \
		"$(PLBASE)\library\unicode" $(MANDIR)

$(IDIRS):
		if not exist $@/$(NULL) $(MKDIR) $@

idirs:		$(IDIRS)

iboot:		
		chdir $(PLHOME)\boot & copy *.pl "$(PLBASE)\boot"
		copy win32\misc\mkboot.bat "$(PLBASE)\bin\mkboot.bat"

ilib:		icommon iclp idialect iyap iiso iunicode
		chdir $(PLHOME)\library & \
			for %f in ($(PLLIBS)) do copy %f "$(PLBASE)\library"

iclp::
		chdir $(PLHOME)\library\clp & \
			for %f in ($(CLP)) do copy %f "$(PLBASE)\library\clp"

icommon::
		copy "$(PLHOME)\library\common\README" "$(PLBASE)\library\common\README.TXT"
#		chdir $(PLHOME)\library\common & \
#			for %f in ($(COMMON)) do copy %f "$(PLBASE)\library\common"

idialect:	iyap
		chdir $(PLHOME)\library\dialect & \
			for %f in ($(DIALECT)) do copy %f "$(PLBASE)\library\dialect"

iyap::
		chdir $(PLHOME)\library\dialect\yap & \
			for %f in ($(YAP)) do copy %f "$(PLBASE)\library\dialect\yap"

iiso::
		chdir $(PLHOME)\library\dialect\iso & \
			for %f in ($(ISO)) do copy %f "$(PLBASE)\library\dialect\iso"

iunicode::
		chdir $(PLHOME)\library\unicode & \
		  for %f in ($(UNICODE)) do copy %f "$(PLBASE)\library\unicode"

iinclude:       
		$(INSTALL_DATA) $(PLHOME)\include\SWI-Prolog.h "$(PLBASE)\include"
		$(INSTALL_DATA) $(PLHOME)\include\SWI-Stream.h "$(PLBASE)\include"
		$(INSTALL_DATA) $(PLHOME)\include\console.h "$(PLBASE)\include\plterm.h"
!IF "$(MT)" == "true"
		$(INSTALL_DATA) "$(EXTRAINCDIR)\pthread.h" "$(PLBASE)\include"
		$(INSTALL_DATA) "$(EXTRAINCDIR)\sched.h" "$(PLBASE)\include"
		$(INSTALL_DATA) "$(EXTRAINCDIR)\semaphore.h" "$(PLBASE)\include"
!ENDIF

install-readme::
		$(INSTALL_DATA) ..\README "$(PLBASE)\README.TXT"
		$(INSTALL_DATA) ..\VERSION "$(PLBASE)"
		$(INSTALL_DATA) ..\ChangeLog "$(PLBASE)\ChangeLog.TXT"
		$(INSTALL_DATA) ..\COPYING "$(PLBASE)\COPYING.TXT"
		$(INSTALL_DATA) ..\man\windows.html "$(PLBASE)\doc"

install-dotfiles::
		$(INSTALL_DATA) ..\dotfiles\dotplrc "$(PLCUSTOM)\pl.ini"
		$(INSTALL_DATA) ..\dotfiles\dotxpcerc "$(PLCUSTOM)\xpce.ini"
		$(INSTALL_DATA) ..\dotfiles\README "$(PLCUSTOM)\README.TXT"

html-install::
		copy ..\man\Manual\*.html $(MANDIR) > nul
		copy ..\man\Manual\*.gif $(MANDIR) > nul


################################################################
# INSTALLER
################################################################

installer::
		$(INSTALL_DATA) win32\installer\options.ini "$(PLBASE)\.."
		$(INSTALL_DATA) win32\installer\pl.nsi "$(PLBASE)\.."
		$(INSTALL_DATA) win32\installer\mkinstaller.pl "$(PLBASE)\.."
		"$(NSIS)" $(NSISDEFS) "$(PLBASE)\..\pl.nsi"

################################################################
# DLL DEMOS
################################################################

dlldemos::
		chdir win32\foreign & $(MAKE)

################################################################
# Build and install packages
################################################################

packages:
		@for %p in ($(PKGS)) do \
		   @if exist "$(PKGDIR)\%p" \
		      $(CMD) /c "chdir $(PKGDIR)\%p & $(MAKE)"

install_packages:
		@for %p in ($(PKGS)) do \
		   @if exist "$(PKGDIR)\%p" \
		      $(CMD) /c "chdir $(PKGDIR)\%p & $(MAKE) install"
!IF "$(CFG)" == "dev"
		@for %p in ($(PKGS)) do \
		   if exist "$(PKGDIR)\%p" \
		      $(CMD) /c "chdir $(PKGDIR)\%p & $(MAKE) html-install"
		if exist $(PKGDIR)\index.html \
		    copy $(PKGDIR)\index.html "$(PKGDOC)"
!ENDIF

xpce_packages:
		@for %p in ($(PKGS)) do \
		   @if exist "$(PKGDIR)\%p" \
		      $(CMD) /c "chdir $(PKGDIR)\%p & $(MAKE) xpce-install"

clean_packages:
		for %p in ($(PKGS)) do \
		   if exist "$(PKGDIR)\%p" \
		      $(CMD) /c "chdir $(PKGDIR)\%p & $(MAKE) clean"

distclean_packages:
		for %p in ($(PKGS)) do \
		   if exist "$(PKGDIR)\%p" \
		      $(CMD) /c "chdir $(PKGDIR)\%p & $(MAKE) distclean"


################################################################
# Quick common actions during development
################################################################

pce-dll::
		$(CMD) /c "chdir $(PKGDIR)\xpce\src & $(MAKE) idll"
clib-install::
		$(CMD) /c "chdir $(PKGDIR)\clib & $(MAKE) install"
odbc-install:
		$(CMD) /c "chdir $(PKGDIR)\odbc & $(MAKE) install"

################################################################
# Cleanup
################################################################

clean:		clean_packages
		chdir rc & $(MAKE) clean
		chdir libtai & $(MAKE) clean
		chdir win32\uxnt & $(MAKE) clean
		chdir win32\console & $(MAKE) clean
		chdir win32\foreign & $(MAKE) clean
		-del *.obj *~ pl.res vmi 2>nul

distclean:	clean distclean_packages
		@chdir rc & $(MAKE) distclean
		@chdir libtai & $(MAKE) distclean
		@chdir win32\foreign & $(MAKE) distclean
		-del ..\bin\*.exe ..\bin\*.dll ..\bin\*.pdb 2>nul
		-del ..\library\INDEX.pl 2>nul
		-del swipl swiplbin 2>nul

realclean:	clean
		del $(STARTUPPATH)

uninstall:
		rmdir /s /q $(PLBASE)

