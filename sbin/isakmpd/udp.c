/*	$OpenBSD: udp.c,v 1.38 2001/06/29 22:59:40 angelos Exp $	*/
/*	$EOM: udp.c,v 1.57 2001/01/26 10:09:57 niklas Exp $	*/

/*
 * Copyright (c) 1998, 1999, 2001 Niklas Hallqvist.  All rights reserved.
 * Copyright (c) 2000 Angelos D. Keromytis.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Ericsson Radio Systems.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This code was written under funding by Ericsson Radio Systems.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#ifndef linux
#include <sys/sockio.h>
#endif
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysdep.h"

#include "conf.h"
#include "if.h"
#include "isakmp.h"
#include "log.h"
#include "message.h"
#include "sysdep.h"
#include "transport.h"
#include "udp.h"
#include "util.h"

#define UDP_SIZE 65536

/* If a system doesn't have SO_REUSEPORT, SO_REUSEADDR will have to do.  */
#ifndef SO_REUSEPORT
#define SO_REUSEPORT SO_REUSEADDR
#endif

/* XXX IPv4 specific.  */
struct udp_transport {
  struct transport transport;
  struct sockaddr *src;
  struct sockaddr *dst;
  int s;
  LIST_ENTRY (udp_transport) link;
};

static struct transport *udp_clone (struct udp_transport *, struct sockaddr *);
static struct transport *udp_create (char *);
static void udp_remove (struct transport *);
static void udp_report (struct transport *);
static int udp_fd_set (struct transport *, fd_set *, int);
static int udp_fd_isset (struct transport *, fd_set *);
static void udp_handle_message (struct transport *);
static struct transport *udp_make (struct sockaddr *);
static int udp_send_message (struct message *);
static void udp_get_dst (struct transport *, struct sockaddr **);
static void udp_get_src (struct transport *, struct sockaddr **);
static char *udp_decode_ids (struct transport *);

static struct transport_vtbl udp_transport_vtbl = {
  { 0 }, "udp",
  udp_create,
  udp_remove,
  udp_report,
  udp_fd_set,
  udp_fd_isset,
  udp_handle_message,
  udp_send_message,
  udp_get_dst,
  udp_get_src,
  udp_decode_ids
};

/* A list of UDP transports we listen for messages on.  */
static LIST_HEAD (udp_listen_list, udp_transport) udp_listen_list;

char *udp_default_port = 0;
char *udp_bind_port = 0;
static struct transport *default_transport, *default_transport6;

/* Find an UDP transport listening on ADDR:PORT.  */
static struct udp_transport *
udp_listen_lookup (struct sockaddr *addr)
{
  struct udp_transport *u;

  for (u = LIST_FIRST (&udp_listen_list); u; u = LIST_NEXT (u, link))
    /* XXX enough ? */
    if (u->src->sa_len == addr->sa_len &&
	memcmp (u->src, addr, addr->sa_len) == 0)
      return u;
  return 0;
}

/* Create a UDP transport structure bound to LADDR just for listening.  */
static struct transport *
udp_make (struct sockaddr *laddr)
{
  struct udp_transport *t = 0;
  int s, on;

  t = calloc (1, sizeof *t);
  if (!t)
    {
      log_print ("udp_make: malloc (%d) failed", sizeof *t);
      return 0;
    }

  s = socket (laddr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
  if (s == -1)
    {
      log_error ("udp_make: socket (%d, %d, %d)", laddr->sa_family, SOCK_DGRAM,
		 IPPROTO_UDP);
      goto err;
    }

  /* Make sure we don't get our traffic encrypted.  */
  if (sysdep_cleartext (s, laddr->sa_family) == -1)
    goto err;

  /*
   * In order to have several bound specific address-port combinations
   * with the same port SO_REUSEADDR is needed.
   * If this is a wildcard socket and we are not listening there, but only
   * sending from it make sure it is entirely reuseable with SO_REUSEPORT.
   */
  on = 1;
  if (setsockopt (s, SOL_SOCKET,
#if 0
		  (laddr->sin_addr.s_addr == INADDR_ANY
		   && conf_get_str ("General", "Listen-on"))
		  ? SO_REUSEPORT : SO_REUSEADDR,
#else
		  SO_REUSEADDR,
#endif
		  (void *)&on, sizeof on) == -1)
    {
      log_error ("udp_make: setsockopt (%d, %d, %d, %p, %d)", s, SOL_SOCKET,
#if 0
		 (laddr->sin_addr.s_addr == INADDR_ANY
		  && conf_get_str ("General", "Listen-on"))
		 ? SO_REUSEPORT : SO_REUSEADDR,
#else
		 SO_REUSEADDR,
#endif
		 &on, sizeof on);
      goto err;
    }

  t->transport.vtbl = &udp_transport_vtbl;
  t->src = laddr;
  if (bind (s, t->src, t->src->sa_len))
    {
      char *tstr;
      if (sockaddr2text (t->src, &tstr, 0))
	  log_error ("udp_make: bind (%d, %p, %d)", s, &t->src, sizeof t->src);
      else
	{
	  log_error ("udp_make: bind (%d, %s, %d)", s, tstr, sizeof t->src);
	  free (tstr);
	}
      goto err;
    }

  t->s = s;
  transport_add (&t->transport);
  transport_reference (&t->transport);
  t->transport.flags |= TRANSPORT_LISTEN;
  return &t->transport;

err:
  if (s != -1)
    close (s);
  if (t)
    udp_remove (&t->transport);
  return 0;
}

/* Clone a listen transport U, record a destination RADDR for outbound use.  */
static struct transport *
udp_clone (struct udp_transport *u, struct sockaddr *raddr)
{
  struct transport *t;
  struct udp_transport *u2;

  t = malloc (sizeof *u);
  if (!t)
    {
      log_error ("udp_clone: malloc (%d) failed", sizeof *u);
      return 0;
    }
  u2 = (struct udp_transport *)t;

  memcpy (u2, u, sizeof *u);

  u2->src = malloc (u->src->sa_len);
  if (!u2->src)
    {
      free (t);
      return 0;
    }
  memcpy (u2->src, u->src, u->src->sa_len);

  u2->dst = malloc (raddr->sa_len);
  if (!u2->dst)
    {
      free (u2->src);
      free (t);
      return 0;
    }
  memcpy (u2->dst, raddr, raddr->sa_len);

  t->flags &= ~TRANSPORT_LISTEN;

  transport_add (t);

  return t;
}

/*
 * Initialize an object of the UDP transport class.  Fill in the local
 * IP address and port information and create a server socket bound to
 * that specific port.  Add the polymorphic transport structure to the
 * system-wide pools of known ISAKMP transports.
 */
static struct transport *
udp_bind (struct sockaddr *addr)
{
  struct sockaddr *src = malloc (addr->sa_len);

  if (!src)
    return 0;

  memcpy (src, addr, addr->sa_len);
  return udp_make (src);
}

/*
 * When looking at a specific network interface address, if it's an INET one,
 * create an UDP server socket bound to it.
 */
static void
udp_bind_if (struct ifreq *ifrp, void *arg)
{
  char *port = (char *)arg;
  struct sockaddr *if_addr = &ifrp->ifr_addr;
  struct conf_list *listen_on;
  struct conf_list_node *address;
  struct sockaddr *addr;
  struct transport *t;
  struct ifreq flags_ifr;
  char *addr_str;
  int s, error;

  /*
   * Well, UDP is an internet protocol after all so drop other ifreqs.
   */
#ifdef USE_OLD_SOCKADDR
  if (if_addr->sa_family != AF_INET && if_addr->sa_family != AF_INET6)
    return;
#else
  if ((if_addr->sa_family != AF_INET ||
       if_addr->sa_len != sizeof (struct sockaddr_in)) &&
      (if_addr->sa_family != AF_INET6 ||
       if_addr->sa_len != sizeof (struct sockaddr_in6)))
    return;
#endif

  /*
   * These special addresses are not useable as they have special meaning
   * in the IP stack.
   */
  if (if_addr->sa_family == AF_INET && 
      (((struct sockaddr_in *)&ifrp->ifr_addr)->sin_addr.s_addr == INADDR_ANY
       || (((struct sockaddr_in *)&ifrp->ifr_addr)->sin_addr.s_addr
	   == INADDR_NONE)) )
    return;

  /* Don't bother with interfaces that are down.  */
  s = socket (if_addr->sa_family, SOCK_DGRAM, 0);
  if (s == -1)
    {
      log_error ("udp_bind_if: socket (%d, SOCK_DGRAM, 0) failed",
		 if_addr->sa_family);
      return;
    }
  strncpy (flags_ifr.ifr_name, ifrp->ifr_name, sizeof flags_ifr.ifr_name - 1);
  if (ioctl (s, SIOCGIFFLAGS, (caddr_t)&flags_ifr) == -1)
    {
      log_error ("udp_bind_if: ioctl (%d, SIOCGIFFLAGS, ...) failed", s);
      return;
    }
  close (s);
  if (!(flags_ifr.ifr_flags & IFF_UP))
    return;

  /* Set port */
  switch (if_addr->sa_family)
    {
    case AF_INET:
      ((struct sockaddr_in *)if_addr)->sin_port = 
	htons((in_port_t)strtol (port, NULL, 10));
      break;
    case AF_INET6:
      ((struct sockaddr_in6 *)if_addr)->sin6_port = 
	htons((in_port_t)strtol (port, NULL, 10));
      break;
    default:
      log_print ("udp_bind_if: unsupported protocol family %d",
		 if_addr->sa_family);
      break;
    }

  /*
   * If we are explicit about what addresses we can listen to, be sure
   * to respect that option.
   * This is quite wasteful redoing the list-run for every interface,
   * but who cares?  This is not an operation that needs to be fast.
   */
  listen_on = conf_get_list ("General", "Listen-on");
  if (listen_on)
    {
      for (address = TAILQ_FIRST (&listen_on->fields); address;
	   address = TAILQ_NEXT (address, link))
	{
	  if (text2sockaddr (address->field, port, &addr))
	    {
	      log_print ("udp_bind_if: invalid address %s in \"Listen-on\"",
			 address->field);
	      continue;
	    }

	  /* If found, take the easy way out. */
	  if (memcmp (addr, if_addr, addr->sa_len) == 0)
	    {
	      free (addr);
	      break;
	    }
	  free (addr);
	}
      conf_free_list (listen_on);

      /*
       * If address is zero then we did not find the address among the ones
       * we should listen to.
       * XXX We do not discover if we do not find our listen addresses...
       * Maybe this should be the other way round.
       */
      if (!address)
	return;
    }

  t = udp_bind (if_addr);
  if (!t)
    {
      error = sockaddr2text (if_addr, &addr_str, 0);
      log_print ("udp_bind_if: failed to create a socket on %s:%s",
		 error ? "unknown" : addr_str, port);
      if (!error)
	free (addr_str);
      return;
    }
  LIST_INSERT_HEAD (&udp_listen_list, (struct udp_transport *)t, link);
}

/*
 * NAME is a section name found in the config database.  Setup and return
 * a transport useable to talk to the peer specified by that name.
 */
static struct transport *
udp_create (char *name)
{
  struct udp_transport *u;
  struct transport *rv;
  struct sockaddr *dst, *addr;
  char *addr_str, *port_str;

  port_str = conf_get_str (name, "Port");
  if (!port_str)
    port_str = udp_default_port;
  if (!port_str)
    port_str = "500";

  addr_str = conf_get_str (name, "Address");
  if (!addr_str)
    {
      log_print ("udp_create: no address configured for \"%s\"", name);
      return 0;
    }
  if (text2sockaddr (addr_str, port_str, &dst))
    {
      log_print ("udp_create: address \"%s\" not understood", addr_str);
      return 0;
    }

  addr_str = conf_get_str (name, "Local-address");
  if (!addr_str)
    addr_str = conf_get_str ("General", "Listen-on");
  if (!addr_str)
    {
      if ((dst->sa_family == AF_INET && !default_transport) ||
	  (dst->sa_family == AF_INET6 && !default_transport6))
	{
	  log_print ("udp_create: no default transport");
	  rv = 0;
	  goto ret;
	}
      else
	{
	  /* XXX Ugly!  */
	  rv = udp_clone ((struct udp_transport *)(dst->sa_family == AF_INET
						   ? default_transport
						   : default_transport6), dst);
	  goto ret;
	}
    }

  if (text2sockaddr (addr_str, port_str, &addr))
    {
      log_print ("udp_create: address \"%s\" not understood", addr_str);
      rv = 0;
      goto ret;
    }
  u = udp_listen_lookup (addr);
  free (addr);
  if (!u)
    {
      log_print ("udp_create: %s:%s must exist as a listener too", addr_str,
		 udp_default_port);
      rv = 0;
      goto ret;
    } 
  rv = udp_clone (u, dst);

 ret:
  free (dst);
  return rv;
}

void
udp_remove (struct transport *t)
{
  struct udp_transport *u = (struct udp_transport *)t;

  if (u->src)
    free (u->src);
  if (u->dst)
    free (u->dst);
  free (t);
}

/* Report transport-method specifics of the T transport.  */
void
udp_report (struct transport *t)
{
  struct udp_transport *u = (struct udp_transport *)t;
  char *src, *dst;

  if (sockaddr2text (u->src, &src, 0) || sockaddr2text (u->dst, &dst, 0))
    goto ret;

  LOG_DBG ((LOG_REPORT, 0, "udp_report: fd %d src %s dst %s", u->s, src, dst));

 ret:
  if (dst)
    free (dst);
  if (src)
    free (src);
}

/*
 * Find out the magic numbers for the UDP protocol as well as the UDP port
 * to use.  Setup an UDP server for each address of this machine, and one
 * for the generic case when we are the initiator.
 */
void
udp_init ()
{
  struct sockaddr_storage dflt_stor;
  struct sockaddr_in *dflt = (struct sockaddr_in *)&dflt_stor;
  char *port;

  /* Initialize the protocol and port numbers.  */
  port = udp_default_port ? udp_default_port : "500";

  LIST_INIT (&udp_listen_list);

  transport_method_add (&udp_transport_vtbl);

  /* Bind the ISAKMP UDP port on all network interfaces we have.  */
  /* XXX need to check errors */
  if_map (udp_bind_if, port);

  if (conf_get_str("General", "Listen-on"))
    return;

  /*
   * If we don't bind to specific addresses via the Listen-on configuration
   * option, bind to INADDR_ANY in case of new addresses popping up.
   * XXX We should use packets coming in on this socket as a signal
   * to reprobe for new interfaces.
   */
  memset (&dflt_stor, 0, sizeof dflt_stor);
  dflt->sin_family = AF_INET;
  ((struct sockaddr_in *)dflt)->sin_len = sizeof (struct sockaddr_in);
  ((struct sockaddr_in *)dflt)->sin_port = 
    htons ((in_port_t)strtol (port, NULL, 10));

  default_transport = udp_bind ((struct sockaddr *)&dflt_stor);
  if (!default_transport)
    log_error ("udp_init: could not allocate default IPv4 ISAKMP UDP port");
  else if (conf_get_str ("General", "Listen-on"))
    default_transport->flags &= ~TRANSPORT_LISTEN;

  memset (&dflt_stor, 0, sizeof dflt_stor);
  dflt->sin_family = AF_INET6;
  ((struct sockaddr_in6 *)dflt)->sin6_len = sizeof (struct sockaddr_in6);
  ((struct sockaddr_in6 *)dflt)->sin6_port = 
    htons ((in_port_t)strtol (port, NULL, 10));

  default_transport6 = udp_bind ((struct sockaddr *)&dflt_stor);
  if (!default_transport6)
    log_error ("udp_init: could not allocate default IPv6 ISAKMP UDP port");
  else if (conf_get_str ("General", "Listen-on"))
    default_transport6->flags &= ~TRANSPORT_LISTEN;
}

/*
 * Set transport T's socket in FDS, return a value useable by select(2)
 * as the number of file descriptors to check.
 */
static int
udp_fd_set (struct transport *t, fd_set *fds, int bit)
{
  struct udp_transport *u = (struct udp_transport *)t;

  if (bit)
    FD_SET (u->s, fds);
  else
    FD_CLR (u->s, fds);

  return u->s + 1;
}

/* Check if transport T's socket is set in FDS.  */
static int
udp_fd_isset (struct transport *t, fd_set *fds)
{
  struct udp_transport *u = (struct udp_transport *)t;

  return FD_ISSET (u->s, fds);
}

/*
 * A message has arrived on transport T's socket.  If T is single-ended,
 * clone it into a double-ended transport which we will use from now on.
 * Package the message as we want it and continue processing in the message
 * module.
 */
static void
udp_handle_message (struct transport *t)
{
  struct udp_transport *u = (struct udp_transport *)t;
  u_int8_t buf[UDP_SIZE];
  struct sockaddr_storage from;
  int len = sizeof from;
  ssize_t n;
  struct message *msg;

  n = recvfrom (u->s, buf, UDP_SIZE, 0, (struct sockaddr *)&from, &len);
  if (n == -1)
    {
      log_error ("recvfrom (%d, %p, %d, %d, %p, %p)", u->s, buf, UDP_SIZE, 0,
		 &from, &len);
      return;
    }

  /*
   * Make a specialized UDP transport structure out of the incoming
   * transport and the address information we got from recvfrom(2).
   */
  t = udp_clone (u, (struct sockaddr *)&from);
  if (!t)
    /* XXX Should we do more here?  */
    return;

  msg = message_alloc (t, buf, n);
  if (!msg)
    /* XXX Log?  */
    return;
  message_recv (msg);
}

/* Physically send the message MSG over its associated transport.  */
static int
udp_send_message (struct message *msg)
{
  struct udp_transport *u = (struct udp_transport *)msg->transport;
  ssize_t n;
  struct msghdr m;

  /*
   * Sending on connected sockets requires that no destination address is
   * given, or else EISCONN will occur.
   */
  m.msg_name = (caddr_t)u->dst;
  m.msg_namelen = u->dst->sa_len;
  m.msg_iov = msg->iov;
  m.msg_iovlen = msg->iovlen;
  m.msg_control = 0;
  m.msg_controllen = 0;
  m.msg_flags = 0;
  n = sendmsg (u->s, &m, 0);
  if (n == -1)
    {
      log_error ("sendmsg (%d, %p, %d)", u->s, &m, 0);
      return -1;
    }
  return 0;
}

/*
 * Get transport T's peer address and stuff it into the sockaddr pointed
 * to by DST.
 */
static void
udp_get_dst (struct transport *t, struct sockaddr **dst)
{
  *dst = ((struct udp_transport *)t)->dst;
}

/*
 * Get transport T's local address and stuff it into the sockaddr pointed
 * to by SRC.  Put its length into SRC_LEN.
 */
static void
udp_get_src (struct transport *t, struct sockaddr **src)
{
  *src = ((struct udp_transport *)t)->src;
}

static char *
udp_decode_ids (struct transport *t)
{
  static char result[1024];
  char idsrc[256], iddst[256];

#ifdef HAVE_GETNAMEINFO
  if (getnameinfo ((struct sockaddr *)&((struct udp_transport *)t)->src,
		   sizeof ((struct udp_transport *)t)->src,
		   idsrc, sizeof idsrc, NULL, 0, NI_NUMERICHOST) != 0)
    {
      log_print ("udp_decode_ids: getnameinfo () failed");
      strcpy (idsrc, "<error>");
    }

  if (getnameinfo ((struct sockaddr *)&((struct udp_transport *)t)->dst,
		   sizeof ((struct udp_transport *)t)->dst,
		   iddst, sizeof iddst, NULL, 0, NI_NUMERICHOST) != 0)
    {
      log_error ("udp_decode_ids: getnameinfo () failed");
      strcpy (iddst, "<error>");
    }
#else
  strcpy (idsrc, inet_ntoa (((struct udp_transport *)t)->src.sin_addr));
  strcpy (iddst, inet_ntoa (((struct udp_transport *)t)->dst.sin_addr));
#endif /* HAVE_GETNAMEINFO */

  sprintf (result, "src: %s dst: %s", idsrc, iddst);

  return result;
}
/*
 * Take a string containing an ext representation of port and return a
 * binary port number in host byte order.  Return zero if anything goes wrong.
 */
in_port_t
udp_decode_port (char *port_str)
{
  char *port_str_end;
  long port_long;
  struct servent *service;

  port_long = ntohl (strtol (port_str, &port_str_end, 0));
  if (port_str == port_str_end)
    {
      service = getservbyname (port_str, "udp");
      if (!service)
	{
	  log_print ("udp_decode_port: service \"%s\" unknown", port_str);
	  return 0;
	}
      return ntohs (service->s_port);
    }
  else if (port_long < 1 || port_long > 65535)
    {
      log_print ("udp_decode_port: port %ld out of range", port_long);
      return 0;
    }

  return port_long;
}
