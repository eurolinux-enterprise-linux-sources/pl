/*  $Id$

    Part of XPCE
    Designed and implemented by Anjo Anjewierden and Jan Wielemaker
    E-mail: jan@swi.psy.uva.nl

    Copyright (C) 1993 University of Amsterdam. All rights reserved.
*/

:- load_foreign_library(dispatch).

dispatch :-
	new(@d, dispatcher),
	send(@d, subscribe, 1, hello, message(@pce, write, @arg1)),
	send(@d, subscribe, 2, succeed, message(@pce, succeed)).

perform :-
	send(@pce, bench, message(@d, dispatch, succeed), 10000, send).

perform2 :-
	send(@pce, bench, message(@pce, succeed), 10000, forward).
