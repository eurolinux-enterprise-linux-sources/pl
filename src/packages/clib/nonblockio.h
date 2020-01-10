/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2007, University of Amsterdam

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    As a special exception, if you link this library with other files,
    compiled with a Free Software compiler, to produce an executable, this
    library does not by itself cause the resulting executable to be covered
    by the GNU General Public License. This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
*/

#ifndef H_NONBLOCKIO_INCLUDED
#define H_NONBLOCKIO_INCLUDED


		 /*******************************
		 *     GET REQUIRED HEADERS	*
		 *******************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <SWI-Stream.h>
#include <SWI-Prolog.h>

#ifdef __WINDOWS__

#include <io.h>
#include <winsock2.h>
typedef size_t socklen_t;

#else /*__WINDOWS__*/

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#ifdef HAVE_H_ERRNO
extern int h_errno;
#else
#define h_errno errno
#endif
#ifndef HAVE_SOCKLEN_T
#define socklen_t size_t
#endif

typedef int SOCKET;

#endif /*__WINDOWS__*/

typedef enum
{ TCP_ERRNO,
  TCP_HERRNO
} nbio_error_map;

typedef enum				/* nbio_setopt() commands */
{ TCP_NONBLOCK,
  TCP_REUSEADDR,
  TCP_NO_DELAY,
  TCP_DISPATCH,
  TCP_INSTREAM,
  TCP_OUTSTREAM,
  UDP_BROADCAST,
  NBIO_END
} nbio_option;

typedef enum
{ REQ_NONE = 0,				/* no request pending */
  REQ_ACCEPT,
  REQ_CONNECT,
  REQ_READ,
  REQ_WRITE,
  REQ_RECVFROM,
  REQ_SENDTO
} nbio_request;

					/* nbio_get_flags() mask */
#define SOCK_INSTREAM	0x01
#define SOCK_OUTSTREAM	0x02
#define SOCK_BIND	0x04		/* What have we done? */
#define SOCK_LISTEN	0x08
#define SOCK_CONNECT	0x10
#define SOCK_ACCEPT	0x20		/* Set on accepted sockets */
#define SOCK_NONBLOCK	0x40		/* Set to non-blocking mode */
#define SOCK_DISPATCH   0x80		/* do not dispatch events */
#define SOCK_CLOSE_SEEN	0x100		/* FD_CLOSE seen */
#define SOCK_EOF_SEEN   0x200		/* Seen end-of-file */
#define SOCK_WAITING	0x400		/* using nbio_wait() */


typedef int	nbio_sock_t;		/* socket handle (not a file-descr) */
typedef struct _plsocket *plsocket_ptr;	/* wrapped socket */

		 /*******************************
		 *	 BASIC FUNCTIONS	*
		 *******************************/

extern int	nbio_init(const char *module);
extern int	nbio_cleanup(void);
extern int	nbio_debug(int level);

extern nbio_sock_t
		nbio_socket(int domain, int type, int protocol);
extern int	nbio_connect(nbio_sock_t socket,
			     const struct sockaddr *serv_addr,
			     size_t addrlen);
extern int	nbio_bind(nbio_sock_t socket,
			  struct sockaddr *my_addr,
			  size_t addrlen);
extern int	nbio_listen(nbio_sock_t socket, int backlog);
extern nbio_sock_t
		nbio_accept(nbio_sock_t master,
			    struct sockaddr *addr,
			    socklen_t *addrlen);

extern ssize_t	nbio_read(nbio_sock_t socket, char *buf, size_t bufSize);
extern ssize_t 	nbio_write(nbio_sock_t socket, char *buf, size_t bufSize);
extern int	nbio_closesocket(nbio_sock_t socket);
extern int 	nbio_close_input(nbio_sock_t socket);
extern int 	nbio_close_output(nbio_sock_t socket);
extern ssize_t	nbio_recvfrom(int socket, void *buf, size_t bufSize, int flags,
			      struct sockaddr *from, socklen_t *fromlen);
extern ssize_t	nbio_sendto(nbio_sock_t socket, void *buf, size_t bufSize, int flags,
			    const struct sockaddr *to, socklen_t tolen);

extern int	nbio_wait(nbio_sock_t socket, nbio_request);
extern SOCKET	nbio_fd(nbio_sock_t socket);
extern int	nbio_select(int n,
			    fd_set *readfds,
			    fd_set *writefds,
			    fd_set *exceptfds,
			    struct timeval *timeout);

extern int	nbio_unify_ip4(term_t ip4, unsigned long hip);
extern int	nbio_get_ip(term_t ip4, struct in_addr *ip);

extern int	nbio_error(int code, nbio_error_map map);
extern const char*
		nbio_last_error(nbio_sock_t socket);
extern int	nbio_setopt(int socket, nbio_option opt, ...);
extern int	nbio_get_flags(int socket);

					/* support tipc_setopt() */
extern plsocket_ptr
		nbio_to_plsocket(nbio_sock_t socket);
extern SOCKET	plsocket_handle(plsocket_ptr s);


		 /*******************************
		 *	    CONVERSION		*
		 *******************************/

extern int	nbio_get_sockaddr(term_t Address,
				  struct sockaddr_in *addr);
extern int	nbio_get_ip4(term_t ip4, struct in_addr *ip);

#endif /*H_NONBLOCKIO_INCLUDED*/
