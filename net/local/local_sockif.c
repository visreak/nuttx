/****************************************************************************
 * net/local/local_sockif.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <netinet/in.h>

#include <nuttx/net/net.h>

#include "local/local.h"

#ifdef CONFIG_NET_LOCAL

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int     local_setup(FAR struct socket *psock, int protocol);
static int     local_bind(FAR struct socket *psock,
                 FAR const struct sockaddr *addr, socklen_t addrlen);
static int     local_connect(FAR struct socket *psock,
                 FAR const struct sockaddr *addr, socklen_t addrlen);
static int     local_accept(FAR struct socket *psock, FAR struct sockaddr *addr,
                 FAR socklen_t *addrlen, FAR struct socket *newsock);
static ssize_t local_send(FAR struct socket *psock, FAR const void *buf,
                 size_t len, int flags);
static ssize_t local_sendto(FAR struct socket *psock, FAR const void *buf,
                 size_t len, int flags, FAR const struct sockaddr *to,
                 socklen_t tolen);

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct sock_intf_s g_local_sockif =
{
  local_setup,    /* si_setup */
  local_bind,     /* si_bind */
  local_listen,   /* si_listen */
  local_connect,  /* si_connect */
  local_accept,   /* si_accept */
  local_send,     /* si_send */
  local_sendto,   /* si_sendto */
  local_recvfrom  /* si_recvfrom */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: local_sockif_alloc
 *
 * Description:
 *   Allocate and attach a local, Unix domain connection structure.
 *
 ****************************************************************************/

static int local_sockif_alloc(FAR struct socket *psock)
{
  /* Allocate the local connection structure */

  FAR struct local_conn_s *conn = local_alloc();
  if (conn == NULL)
    {
      /* Failed to reserve a connection structure */

      return -ENOMEM;
    }

  /* Set the reference count on the connection structure.  This reference
   * count will be incremented only if the socket is dup'ed
   */

  DEBUGASSERT(conn->lc_crefs == 0);
  conn->lc_crefs = 1;

  /* Save the pre-allocated connection in the socket structure */

  psock->s_conn = conn;
  return OK;
}

/****************************************************************************
 * Name: local_setup
 *
 * Description:
 *   Called for socket() to verify that the provided socket type and
 *   protocol are usable by this address family.  Perform any family-
 *   specific socket fields.
 *
 * Parameters:
 *   psock    A pointer to a user allocated socket structure to be initialized.
 *   protocol (see sys/socket.h)
 *
 * Returned Value:
 *   Zero (OK) is returned on success.  Otherwise, a negater errno value is
 *   returned.
 *
 ****************************************************************************/

static int local_setup(FAR struct socket *psock, int protocol)
{
  /* Allocate the appropriate connection structure.  This reserves the
   * the connection structure is is unallocated at this point.  It will
   * not actually be initialized until the socket is connected.
   *
   * Only SOCK_STREAM and SOCK_DGRAM and possible SOCK_RAW are supported.
   */

  switch (psock->s_type)
    {
#ifdef CONFIG_NET_TCP
      case SOCK_STREAM:
        if (protocol != 0 && protocol != IPPROTO_TCP)
          {
            return -EPROTONOSUPPORT;
          }

        /* Allocate and attach the local connection structure */

        return local_sockif_alloc(psock);
#endif /* CONFIG_NET_TCP */

#ifdef CONFIG_NET_UDP
      case SOCK_DGRAM:
        if (protocol != 0 && protocol != IPPROTO_UDP)
          {
            return -EPROTONOSUPPORT;
          }

        /* Allocate and attach the local connection structure */

        return local_sockif_alloc(psock);
#endif /* CONFIG_NET_UDP */

      default:
        return -EPROTONOSUPPORT;
    }
}

/****************************************************************************
 * Name: local_bind
 *
 * Description:
 *   local_bind() gives the socket 'psock' the local address 'addr'.  'addr'
 *   is 'addrlen' bytes long.  Traditionally, this is called "assigning a
 *   name to a socket."  When a socket is created with socket(), it exists
 *   in a name space (address family) but has no name assigned.
 *
 * Parameters:
 *   psock    Socket structure of the socket to bind
 *   addr     Socket local address
 *   addrlen  Length of 'addr'
 *
 * Returned Value:
 *   0 on success;  A negated errno value is returned on failure.  See
 *   bind() for a list a appropriate error values.
 *
 ****************************************************************************/

static int local_bind(FAR struct socket *psock,
                      FAR const struct sockaddr *addr, socklen_t addrlen)
{
  int ret;

  /* Verify that a valid address has been provided */

  if (addr->sa_family != AF_LOCAL || addrlen < sizeof(sa_family_t))
    {
      nerr("ERROR: Invalid address length: %d < %d\n",
           addrlen, sizeof(sa_family_t));
      return -EBADF;
    }

  /* Perform the binding depending on the protocol type */

  switch (psock->s_type)
    {
      /* Bind a local TCP/IP stream or datagram socket  */

#if defined(ONFIG_NET_TCP) || defined(CONFIG_NET_UDP)
#ifdef CONFIG_NET_TCP
      case SOCK_STREAM:
#endif
#ifdef CONFIG_NET_UDP
      case SOCK_DGRAM:
#endif
        {
          /* Bind the Unix domain connection structure */

          ret psock_local_bind(psock, addr, addrlen);

          /* Mark the socket bound */

          if (ret >= 0)
            {
              psock->s_flags |= _SF_BOUND;
            }
        }
        break;
#endif /* CONFIG_NET_TCP || CONFIG_NET_UDP*/

      default:
        ret = -EBADF;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: local_connect
 *
 * Description:
 *   local_connect() connects the local socket referred to by the structure
 *   'psock' to the address specified by 'addr'. The addrlen argument
 *   specifies the size of 'addr'.  The format of the address in 'addr' is
 *   determined by the address space of the socket 'psock'.
 *
 *   If the socket 'psock' is of type SOCK_DGRAM then 'addr' is the address
 *   to which datagrams are sent by default, and the only address from which
 *   datagrams are received. If the socket is of type SOCK_STREAM or
 *   SOCK_SEQPACKET, this call attempts to make a connection to the socket
 *   that is bound to the address specified by 'addr'.
 *
 *   Generally, connection-based protocol sockets may successfully
 *   local_connect() only once; connectionless protocol sockets may use
 *   local_connect() multiple times to change their association.
 *   Connectionless sockets may dissolve the association by connecting to
 *   an address with the sa_family member of sockaddr set to AF_UNSPEC.
 *
 * Parameters:
 *   psock     Pointer to a socket structure initialized by psock_socket()
 *   addr      Server address (form depends on type of socket)
 *   addrlen   Length of actual 'addr'
 *
 * Returned Value:
 *   0 on success; a negated errno value on failue.  See connect() for the
 *   list of appropriate errno values to be returned.
 *
 ****************************************************************************/

static int local_connect(FAR struct socket *psock,
                         FAR const struct sockaddr *addr, socklen_t addrlen)
{
  /* Verify that a valid address has been provided */

  if (addr->sa_family != AF_LOCAL || addrlen < sizeof(sa_family_t))
    {
      return -EBADF;
    }

  /* Perform the connection depending on the protocol type */

  switch (psock->s_type)
    {
#ifdef CONFIG_NET_TCP
      case SOCK_STREAM:
        {
          /* Verify that the socket is not already connected */

          if (_SS_ISCONNECTED(psock->s_flags))
            {
              return -EISCONN;
            }

          /* It's not...  Connect to the local Unix domain server */

          return psock_local_connect(psock, addr);
        }
        break;
#endif /* CONFIG_NET_TCP */

#ifdef CONFIG_NET_UDP
      case SOCK_DGRAM:
        {
          /* Perform the datagram connection logic */

          return psock_local_connect(psock, addr);
        }
        break;
#endif /* CONFIG_NET_UDP */

      default:
        return -EBADF;
    }
}

/****************************************************************************
 * Name: local_send
 *
 * Description:
 *   Implements the send() operation for the case of the local, Unix socket.
 *
 * Parameters:
 *   psock    An instance of the internal socket structure.
 *   buf      Data to send
 *   len      Length of data to send
 *   flags    Send flags
 *
 * Returned Value:
 *   On success, returns the number of characters sent.  On  error, a negated
 *   errno value is returned (see send() for the list of appropriate error
 *   values.
 *
 ****************************************************************************/

static ssize_t local_send(FAR struct socket *psock, FAR const void *buf,
                          size_t len, int flags)
{
  ssize_t ret;

  switch (psock->s_type)
    {
#ifdef CONFIG_NET_TCP
      case SOCK_STREAM:
        {
          /* Local TCP packet send */

          ret = psock_local_send(psock, buf, len, flags);
        }
        break;
#endif /* CONFIG_NET_TCP */

#ifdef CONFIG_NET_UDP
      case SOCK_DGRAM:
        {
          /* Local UDP packet send */
#warning Missing logic
          ret = -ENOSYS;
        }
        break;
#endif /* CONFIG_NET_UDP */

      default:
        {
          /* EDESTADDRREQ.  Signifies that the socket is not connection-mode
           * and no peer address is set.
           */

          ret = -EDESTADDRREQ;
        }
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: local_sendto
 *
 * Description:
 *   Implements the sendto() operation for the case of the local, Unix socket.
 *
 * Parameters:
 *   psock    A pointer to a NuttX-specific, internal socket structure
 *   buf      Data to send
 *   len      Length of data to send
 *   flags    Send flags
 *   to       Address of recipient
 *   tolen    The length of the address structure
 *
 * Returned Value:
 *   On success, returns the number of characters sent.  On  error, a negated
 *   errno value is returned (see send_to() for the list of appropriate error
 *   values.
 *
 ****************************************************************************/

ssize_t local_sendto(FAR struct socket *psock, FAR const void *buf,
                     size_t len, int flags, FAR const struct sockaddr *to,
                     socklen_t tolen)
{
  ssize_t nsent;

  /* Verify that a valid address has been provided */

  if (to->sa_family != AF_LOCAL || tolen < sizeof(sa_family_t))
    {
      nerr("ERROR: Unrecognized address family: %d\n",
           to->sa_family);
      return -EAFNOSUPPORT;
    }

#ifdef CONFIG_NET_UDP
  /* If this is a connected socket, then return EISCONN */

  if (psock->s_type != SOCK_DGRAM)
    {
      nerr("ERROR: Connected socket\n");
      return -EISCONN;
    }

  /* Now handle the local UDP sendto() operation */

  nsent = psock_local_sendto(psock, buf, len, flags, to, tolen);
#else
  nsent = -EISCONN;
#endif /* CONFIG_NET_LOCAL_DGRAM */

  return nsent;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name:
 *
 * Description:
 *
 * Parameters:
 *
 * Returned Value:
 *
 * Assumptions:
 *
 ****************************************************************************/

#endif /* CONFIG_NET_LOCAL */