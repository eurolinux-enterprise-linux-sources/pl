/*  $Id$

    Part of XPCE
    Designed and implemented by Anjo Anjewierden and Jan Wielemaker
    E-mail: jan@swi.psy.uva.nl

    Copyright (C) 1993 University of Amsterdam. All rights reserved.
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
There she is again: The famous Hello World program!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <pce/Pce.h>
#include <pce/Label.h>
#include <pce/Button.h>
#include <pce/Message.h>
#include <pce/String.h>

PceStatus
pceInitApplication(int argc, char **argv)
{ PceObject d("dialog");	// Create an instance of dialog

  d.send("append", PceLabel("message", PceString("Hello World")));
  d.send("append", PceButton("quit", PceMessage(d, "destroy")));
  d.send("open");

  return TRUE;
}
