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

/*#define O_DEBUG 1*/
#ifdef __WINDOWS__

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
   Extension of SWI-Prolog:
   Primitives to support interprocess communication via DDE, for those
   platforms that support DDEML.  Currently, this is the Windows family (3.1
   and above) and Unix platforms with Bristol's Windows support.

   Eventually, this should turn into a full DDE capability.  For the
   present, I'm just implementing the client side of conversation
   management, and only providing request transactions, as follows:

   open_dde_conversation(+Service, +Topic, -Handle)
   Open a conversation with a server supporting the given service name and
   topic (atoms).  If successful, Handle may be used to send transactions to
   the server.  If no willing server is found, fails.

   close_dde_conversation(+Handle)
   Close the conversation associated with Handle.  All opened conversations
   should be closed when they're no longer needed, although the system
   will close any that remain open on process termination.

   dde_request(+Handle, +Item, -Value)
   Request a value from the server.  Item is an atom that identifies the
   requested data, and Value will be an atom (CF_TEXT data in DDE parlance)
   representing that data, if the request is successful.  If unsuccessful,
   Value will be unified with a term of form error(reason), identifying the
   problem.

   It could be argued that the atoms above should be strings; I may go that
   way sometime.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define INCLUDE_DDEML_H
#include "pl-incl.h"

#if O_DDE
#include <string.h>

#ifdef __WATCOMC__			/* at least version 9.5 */
#define WATCOM_DDEACCESS_BUG 1
#endif

#define FASTBUFSIZE 512			/* use local buffer upto here */
#define MAX_CONVERSATIONS 32		/* Max. # of conversations */
#define TIMEOUT_VERY_LONG 0x7fffffff;	/* largest positive int */

static HCONV conv_handle[MAX_CONVERSATIONS];
static HCONV server_handle[MAX_CONVERSATIONS];
static DWORD ddeInst;			/* Instance of this process */

static Module	 MODULE_dde;		/* win_dde */
static functor_t FUNCTOR_dde_connect3;
static functor_t FUNCTOR_dde_connect_confirm3;
static functor_t FUNCTOR_dde_disconnect1;
static functor_t FUNCTOR_dde_request4;
static functor_t FUNCTOR_dde_execute3;
static functor_t FUNCTOR_error1;

static const char *
dde_error_message(int errn)
{ const char *err;

  if ( errn <= 0 )
    errn = DdeGetLastError(ddeInst);

  switch(errn)
  { case DMLERR_ADVACKTIMEOUT:
    case DMLERR_DATAACKTIMEOUT:
    case DMLERR_EXECACKTIMEOUT:
    case DMLERR_POKEACKTIMEOUT:
    case DMLERR_UNADVACKTIMEOUT:	err = "Timeout";		break;
    case DMLERR_BUSY:			err = "Service busy";		break;
    case DMLERR_DLL_NOT_INITIALIZED:	err = "DDL not initialised";	break;
    case DMLERR_INVALIDPARAMETER:	err = "Invalid parameter";	break;
    case DMLERR_MEMORY_ERROR:		err = "Memory error";		break;
    case DMLERR_NO_CONV_ESTABLISHED:	err = "No conversation";	break;
    case DMLERR_NO_ERROR:		err = "No error???";		break;
    case DMLERR_NOTPROCESSED:		err = "Not processed";		break;
    case DMLERR_POSTMSG_FAILED:		err = "PostMessage() failed";	break;
    case DMLERR_REENTRANCY:		err = "Reentrance";		break;
    case DMLERR_SERVER_DIED:		err = "Server died";		break;
    default:				err = "Unknown error";		break;
  }

  return err;
}


static word
dde_warning(const char *cmd)
{ const char *err = dde_error_message(-1);

  return PL_error(NULL, 0, NULL, ERR_DDE_OP, cmd, err);
}


static int
unify_hsz(term_t term, HSZ hsz)
{ wchar_t buf[FASTBUFSIZE];
  int len;

  if ( !(len=DdeQueryStringW(ddeInst, hsz, buf,
			     sizeof(buf)/sizeof(wchar_t)-1, CP_WINUNICODE)) )
  { dde_warning("string handle");
    return NULL_ATOM;
  }

  if ( len == sizeof(buf)/sizeof(wchar_t)-1 )
  { if ( (len=DdeQueryStringW(ddeInst, hsz, NULL, 0, CP_WINUNICODE)) > 0 )
    { wchar_t *b2 = malloc((len+1)*sizeof(wchar_t));
      int rc;

      DdeQueryStringW(ddeInst, hsz, b2, len+1, CP_WINUNICODE);
      rc = PL_unify_wchars(term, PL_ATOM, len, b2);
      free(b2);

      return rc;
    }

    dde_warning("string handle");
  }

  return PL_unify_wchars(term, PL_ATOM, len, buf);
}


static word
unify_hdata(term_t t, HDDEDATA data)
{ char buf[FASTBUFSIZE];
  int len;

  if ( !(len=DdeGetData(data, buf, sizeof(buf), 0)) )
    return dde_warning("data handle");

  DEBUG(0, Sdprintf("DdeGetData() returned %d bytes\n", len));

  if ( len == sizeof(buf) )
  { if ( (len=DdeGetData(data, NULL, 0, 0)) > 0 )
    { char *b2 = malloc(len);
      int rval;

      DdeGetData(data, b2, len, 0);
      rval = PL_unify_wchars(t, PL_ATOM, len/sizeof(wchar_t)-1, (wchar_t*)b2);
      free(b2);

      return rval;
    }

    return dde_warning("data handle");
  }

  return PL_unify_wchars(t, PL_ATOM, len/sizeof(wchar_t)-1, (wchar_t*)buf);
}


static int
get_hsz(term_t data, HSZ *rval)
{ wchar_t *s;
  size_t len;

  if ( PL_get_wchars(data, &len, &s, CVT_ALL|CVT_EXCEPTION) )
  { HSZ h = DdeCreateStringHandleW(ddeInst, s, CP_WINUNICODE);

    if ( s[len] )
      Sdprintf("OOPS, s[%d] != 0\n", len);

    if ( h )
    { *rval = h;
      succeed;
    }
  }

  fail;
}


static int
allocServerHandle(HCONV handle)
{ int i;

  for(i=0; i<MAX_CONVERSATIONS; i++)
  { if ( !server_handle[i] )
    { server_handle[i] = handle;
      return i;
    }
  }

  PL_error(NULL, 0, NULL, ERR_RESOURCE, ATOM_max_dde_handles);

  return -1;
}


static int
findServerHandle(HCONV handle)
{ int i;

  for(i=0; i<MAX_CONVERSATIONS; i++)
  { if ( server_handle[i] == handle )
      return i;
  }

  return -1;
}


static HDDEDATA CALLBACK
DdeCallback(UINT type, UINT fmt, HCONV hconv, HSZ hsz1, HSZ hsz2,
            HDDEDATA hData, DWORD dwData1, DWORD dwData2)
{
  switch(type)
  {  case XTYP_CONNECT:
     { fid_t cid = PL_open_foreign_frame();
       term_t argv = PL_new_term_refs(3);
       predicate_t pred = PL_pred(FUNCTOR_dde_connect3, MODULE_dde);
       int rval;

       if ( unify_hsz(argv+0, hsz2) &&			/* topic */
	    unify_hsz(argv+1, hsz1) &&			/* service */
	    PL_unify_integer(argv+2, dwData2 ? 1 : 0) )	/* same instance */
       { rval = PL_call_predicate(MODULE_dde, TRUE, pred, argv);
       } else
       { rval = FALSE;
       }
       PL_discard_foreign_frame(cid);

       return (void *)rval;
     }
     case XTYP_CONNECT_CONFIRM:
     { fid_t cid = PL_open_foreign_frame();
       term_t argv = PL_new_term_refs(3);
       predicate_t pred = PL_pred(FUNCTOR_dde_connect_confirm3, MODULE_dde);
       int plhandle;

       if ( (plhandle = allocServerHandle(hconv)) >= 0 )
       { fid_t cid = PL_open_foreign_frame();
	 term_t argv = PL_new_term_refs(3);
	 predicate_t pred = PL_pred(FUNCTOR_dde_connect_confirm3, MODULE_dde);

	 if ( unify_hsz(argv+0, hsz2) &&			/* topic */
	      unify_hsz(argv+1, hsz1) &&			/* service */
	      PL_unify_integer(argv+2, plhandle) )
	   PL_call_predicate(MODULE_dde, TRUE, pred, argv);

	 PL_discard_foreign_frame(cid);
       }

       return NULL;
     }
     case XTYP_DISCONNECT:
     { fid_t cid = PL_open_foreign_frame();
       term_t argv = PL_new_term_refs(1);
       predicate_t pred = PL_pred(FUNCTOR_dde_disconnect1, MODULE_dde);
       int plhandle = findServerHandle(hconv);

       if ( plhandle >= 0 && plhandle < MAX_CONVERSATIONS )
	 server_handle[plhandle] = (HCONV)NULL;

       PL_put_integer(argv+0, plhandle);
       PL_call_predicate(MODULE_dde, TRUE, pred, argv);
       PL_discard_foreign_frame(cid);

       return NULL;
     }
     case XTYP_EXECUTE:
     { int plhandle = findServerHandle(hconv);
       HDDEDATA rval = DDE_FNOTPROCESSED;
       fid_t cid = PL_open_foreign_frame();
       term_t argv = PL_new_term_refs(3);
       predicate_t pred = PL_pred(FUNCTOR_dde_execute3, MODULE_dde);

       DEBUG(0, Sdprintf("Got XTYP_EXECUTE request\n"));

       PL_put_integer(argv+0, plhandle);
       unify_hsz(     argv+1, hsz1);
       unify_hdata(   argv+2, hData);
       if ( PL_call_predicate(MODULE_dde, TRUE, pred, argv) )
	 rval = (void *) DDE_FACK;
       PL_discard_foreign_frame(cid);
       DdeFreeDataHandle(hData);
       return rval;
     }
     case XTYP_REQUEST:
     { HDDEDATA data = (HDDEDATA) NULL;

       if ( fmt == CF_UNICODETEXT )
       { fid_t cid = PL_open_foreign_frame();
	 term_t argv = PL_new_term_refs(4);
	 predicate_t pred = PL_pred(FUNCTOR_dde_request4, MODULE_dde);
	 int plhandle = findServerHandle(hconv);

	 PL_put_integer( argv+0, plhandle);
	 unify_hsz(	 argv+1, hsz1);	/* topic */
	 unify_hsz(      argv+2, hsz2);	/* item */
	 PL_put_variable(argv+3);

	 if ( PL_call_predicate(MODULE_dde, TRUE, pred, argv) )
	 { wchar_t *s;
	   size_t len;

					/* TBD: error handling */
	   if ( PL_get_wchars(argv+3, &len, &s, CVT_ALL) )
	     data = DdeCreateDataHandle(ddeInst,
					(unsigned char*) s,
					(DWORD)(len+1)*sizeof(wchar_t),
					0, hsz2, CF_UNICODETEXT, 0);
	 }
	 PL_discard_foreign_frame(cid);
       }

       return data;
     }
     default:
       ;
  }

  return (HDDEDATA)NULL;
}


static word
dde_initialise()
{ if ( ddeInst == (DWORD)NULL )
  { if (DdeInitializeW(&ddeInst, (PFNCALLBACK)DdeCallback,
		       APPCLASS_STANDARD|CBF_FAIL_ADVISES|CBF_FAIL_POKES|
		       CBF_SKIP_REGISTRATIONS|CBF_SKIP_UNREGISTRATIONS,
		       0L)
	!= DMLERR_NO_ERROR)
    { ddeInst = (DWORD) -1;
      return dde_warning("initialise");
    }

    MODULE_dde = lookupModule(PL_new_atom("win_dde"));

    FUNCTOR_dde_connect3  =
	lookupFunctorDef(PL_new_atom("$dde_connect"), 3);
    FUNCTOR_dde_connect_confirm3 =
	lookupFunctorDef(PL_new_atom("$dde_connect_confirm"), 3);
    FUNCTOR_dde_disconnect1 =
        lookupFunctorDef(PL_new_atom("$dde_disconnect"), 1);
    FUNCTOR_dde_request4  =
	lookupFunctorDef(PL_new_atom("$dde_request"), 4);
    FUNCTOR_dde_execute3  =
	lookupFunctorDef(PL_new_atom("$dde_execute"), 3);
    FUNCTOR_error1        =
        lookupFunctorDef(ATOM_error, 1);
  }

  succeed;
}


word
pl_dde_register_service(term_t topic, term_t onoff)
{ HSZ t;
  int a;

  TRY(dde_initialise());

  if ( !get_hsz(topic, &t) )
    fail;
  if ( !PL_get_bool(onoff, &a) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_bool, onoff);

  if ( !a )
  { int rval = (int)DdeNameService(ddeInst, t, 0L, DNS_UNREGISTER);
    DdeFreeStringHandle(ddeInst, t);
    return rval ? TRUE : FALSE;
  } else
  { if ( DdeNameService(ddeInst, t, 0L, DNS_REGISTER|DNS_FILTERON) )
      succeed;				/* should we free too? */

    DdeFreeStringHandle(ddeInst, t);
    return dde_warning("register_request");
  }
}


word
pl_open_dde_conversation(term_t service, term_t topic, term_t handle)
{ UINT i;
  HSZ Hservice, Htopic;

  if ( !dde_initialise() )
    fail;

  if ( !get_hsz(service, &Hservice) ||
       !get_hsz(topic, &Htopic) )
    fail;

  /* Establish a connection and get a handle for it */
  for (i=0; i < MAX_CONVERSATIONS; i++)   /* Find an open slot */
  { if (conv_handle[i] == (HCONV)NULL)
      break;
  }
  if (i == MAX_CONVERSATIONS)
    return PL_error(NULL, 0, NULL, ERR_RESOURCE, ATOM_max_dde_handles);

  if ( !(conv_handle[i] = DdeConnect(ddeInst, Hservice, Htopic, 0)) )
    fail;

  DdeFreeStringHandle(ddeInst, Hservice);
  DdeFreeStringHandle(ddeInst, Htopic);

  return PL_unify_integer(handle, i);
}


static int
get_conv_handle(term_t handle, int *theh)
{ int h;

  if ( !PL_get_integer(handle, &h) || h < 0 || h >= MAX_CONVERSATIONS )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_dde_handle, handle);

  if ( !conv_handle[h] )
    return PL_error(NULL, 0, 0, ERR_EXISTENCE, ATOM_dde_handle, handle);

  *theh = h;
  succeed;
}


word
pl_close_dde_conversation(term_t handle)
{ int hdl;

  if ( !get_conv_handle(handle, &hdl) )
    fail;

  DdeDisconnect(conv_handle[hdl]);
  conv_handle[hdl] = (HCONV)NULL;

  succeed;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
NOTE: Windows-XP gives the wrong value for valuelen below. Hence we will
use nul-terminated strings.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
pl_dde_request(term_t handle, term_t item,
	       term_t value, term_t timeout)
{ int hdl;
  int rval;
  HSZ Hitem;
  DWORD result, valuelen;
  HDDEDATA Hvalue;
  long tmo;
  static UINT fmt[] = {CF_UNICODETEXT, CF_TEXT};
  int fmti;

  if ( !get_conv_handle(handle, &hdl) ||
       !get_hsz(item, &Hitem) ||
       !PL_get_long_ex(timeout, &tmo) )
    fail;

  if ( tmo <= 0 )
    tmo = TIMEOUT_VERY_LONG;

  for(fmti = 0; fmti<2; fmti++)
  { Hvalue = DdeClientTransaction(NULL, 0, conv_handle[hdl], Hitem, fmt[fmti],
				  XTYP_REQUEST, (DWORD)tmo, &result);
    if ( Hvalue )
      break;
  }
  DdeFreeStringHandle(ddeInst, Hitem);

  if ( Hvalue)
  { char *valuedata;

    if ( (valuedata = DdeAccessData(Hvalue, &valuelen)) )
    { DEBUG(0, Sdprintf("valuelen = %ld; format = %d\n", valuelen, fmti));
      if ( fmt[fmti] == CF_TEXT )
      { DEBUG(0, Sdprintf("ANSI text\n"));
	rval = PL_unify_string_chars(value, valuedata);
      } else
	rval = PL_unify_wchars(value, PL_STRING, -1, (wchar_t*)valuedata);
      DdeUnaccessData(Hvalue);
      return rval;
    } else
    { return dde_warning("access_data");
    }
  } else
  { const char *errmsg = dde_error_message(-1);

    return PL_unify_term(value,
			 PL_FUNCTOR, FUNCTOR_error1, /* error(Message) */
			 PL_CHARS,   errmsg);
  }
}



word
pl_dde_execute(term_t handle, term_t command, term_t timeout)
{ int hdl;
  wchar_t *cmdstr;
  size_t cmdlen;
  HDDEDATA Hvalue, data;
  DWORD result;
  long tmo;

  if ( !get_conv_handle(handle, &hdl) ||
       !PL_get_wchars(command, &cmdlen, &cmdstr, CVT_ALL|CVT_EXCEPTION) ||
       !PL_get_long_ex(timeout, &tmo) )
    fail;

  if ( tmo <= 0 )
    tmo = TIMEOUT_VERY_LONG;

  if ( !(data = DdeCreateDataHandle(ddeInst,
				    (unsigned char*)cmdstr,
				    (DWORD)(cmdlen+1)*sizeof(wchar_t),
				    0, 0, CF_UNICODETEXT, 0)) )
    return dde_warning("dde_execute/3");

  Hvalue = DdeClientTransaction((LPBYTE) data, (DWORD) -1,
				conv_handle[hdl], 0L, 0,
				XTYP_EXECUTE, (DWORD) tmo, &result);
  if ( Hvalue )
    succeed;

  return dde_warning("execute");
}


word
pl_dde_poke(term_t handle, term_t item, term_t data, term_t timeout)
{ int hdl;
  wchar_t *datastr;
  size_t datalen;
  HDDEDATA Hvalue;
  HSZ Hitem;
  long tmo;

  if ( !get_conv_handle(handle, &hdl) ||
       !get_hsz(item, &Hitem) )
    fail;
  if ( !PL_get_wchars(data, &datalen, &datastr, CVT_ALL|CVT_EXCEPTION) )
    fail;
  if ( !PL_get_long_ex(timeout, &tmo) )
    fail;

  if ( tmo <= 0 )
    tmo = TIMEOUT_VERY_LONG;

  Hvalue = DdeClientTransaction((unsigned char*)datastr,
				(DWORD)(datalen+1)*sizeof(wchar_t),
				conv_handle[hdl], Hitem, CF_UNICODETEXT,
				XTYP_POKE, (DWORD)tmo, NULL);

#if 0
  if ( !Hvalue )
  { char *txt;

    if ( !PL_get_nchars(data, &datalen, &txt, CVT_ALL|CVT_EXCEPTION|REP_MB) )
      fail;

    Hvalue = DdeClientTransaction(txt, datalen+1,
				  conv_handle[hdl], Hitem, CF_TEXT,
				  XTYP_POKE, (DWORD)tmo, NULL);
  }
#endif

  if ( !Hvalue )
    return dde_warning("poke");

  succeed;
}

#endif /*O_DDE*/
#endif /*__WINDOWS__*/
