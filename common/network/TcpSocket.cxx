/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef WIN32
//#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define errorNumber WSAGetLastError()
#else
#define errorNumber errno
#define closesocket close
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#endif

#include <stdlib.h>
#include <network/TcpSocket.h>
#include <rfb/util.h>
#include <rfb/LogWriter.h>

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long)-1)
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((unsigned long)0x7F000001)
#endif

#if defined(HAVE_GETADDRINFO) && !defined(IN6_ARE_ADDR_EQUAL)
#define IN6_ARE_ADDR_EQUAL(a,b) \
  (memcmp ((const void*)(a), (const void*)(b), sizeof (struct in6_addr)) == 0)
#endif

using namespace network;
using namespace rdr;

typedef struct vnc_sockaddr {
	union {
		sockaddr	sa;
		sockaddr_in	sin;
#ifdef HAVE_GETADDRINFO
		sockaddr_in6	sin6;
#endif
	} u;
} vnc_sockaddr_t;

static rfb::LogWriter vlog("TcpSocket");

/* Tunnelling support. */
int network::findFreeTcpPort (void)
{
  int sock;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;

  if ((sock = socket (AF_INET, SOCK_STREAM, 0)) < 0)
    throw SocketException ("unable to create socket", errorNumber);

  addr.sin_port = 0;
  if (bind (sock, (struct sockaddr *)&addr, sizeof (addr)) < 0)
    throw SocketException ("unable to find free port", errorNumber);

  socklen_t n = sizeof(addr);
  if (getsockname (sock, (struct sockaddr *)&addr, &n) < 0)
    throw SocketException ("unable to get port number", errorNumber);

  closesocket (sock);
  return ntohs(addr.sin_port);
}


// -=- Socket initialisation
static bool socketsInitialised = false;
static void initSockets() {
  if (socketsInitialised)
    return;
#ifdef WIN32
  WORD requiredVersion = MAKEWORD(2,0);
  WSADATA initResult;
  
  if (WSAStartup(requiredVersion, &initResult) != 0)
    throw SocketException("unable to initialise Winsock2", errorNumber);
#else
  signal(SIGPIPE, SIG_IGN);
#endif
  socketsInitialised = true;
}


// -=- TcpSocket

TcpSocket::TcpSocket(int sock, bool close)
  : Socket(new FdInStream(sock), new FdOutStream(sock), true), closeFd(close)
{
}

TcpSocket::TcpSocket(const char *host, int port)
  : closeFd(true)
{
  int sock, err, result, family;
  vnc_sockaddr_t sa;
  socklen_t salen;
#ifdef HAVE_GETADDRINFO
  struct addrinfo *ai, *current, hints;
#endif

  // - Create a socket
  initSockets();

#ifdef HAVE_GETADDRINFO
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;

  if ((result = getaddrinfo(host, NULL, &hints, &ai)) != 0) {
    throw Exception("unable to resolve host by name: %s",
		    gai_strerror(result));
  }

  for (current = ai; current != NULL; current = current->ai_next) {
    family = current->ai_family;
    if (family != AF_INET && family != AF_INET6)
      continue;

    salen = current->ai_addrlen;
    memcpy(&sa, current->ai_addr, salen);

    if (family == AF_INET)
      sa.u.sin.sin_port = htons(port);
    else
      sa.u.sin6.sin6_port = htons(port);

#else /* HAVE_GETADDRINFO */
    family = AF_INET;
    salen = sizeof(struct sockaddr_in);

    /* Try processing the host as an IP address */
    memset(&sa, 0, sizeof(sa));
    sa.u.sin.sin_family = AF_INET;
    sa.u.sin.sin_addr.s_addr = inet_addr((char *)host);
    sa.u.sin.sin_port = htons(port);
    if ((int)sa.u.sin.sin_addr.s_addr == -1) {
      /* Host was not an IP address - try resolving as DNS name */
      struct hostent *hostinfo;
      hostinfo = gethostbyname((char *)host);
      if (hostinfo && hostinfo->h_addr) {
        sa.u.sin.sin_addr.s_addr = ((struct in_addr *)hostinfo->h_addr)->s_addr;
      } else {
        err = errorNumber;
        throw SocketException("unable to resolve host by name", err);
      }
    }
#endif /* HAVE_GETADDRINFO */

    sock = socket (family, SOCK_STREAM, 0);
    if (sock == -1) {
      err = errorNumber;
#ifdef HAVE_GETADDRINFO
      freeaddrinfo(ai);
#endif /* HAVE_GETADDRINFO */
      throw SocketException("unable to create socket", err);
    }

  /* Attempt to connect to the remote host */
    while ((result = connect(sock, &sa.u.sa, salen)) == -1) {
      err = errorNumber;
#ifndef WIN32
      if (err == EINTR)
	continue;
#endif
      closesocket(sock);
      break;
    }

#ifdef HAVE_GETADDRINFO
    if (result == 0)
      break;
  }

  freeaddrinfo(ai);
#endif /* HAVE_GETADDRINFO */

  if (result == -1)
    throw SocketException("unable connect to socket", err);

#ifndef WIN32
  // - By default, close the socket on exec()
  fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif

  // Disable Nagle's algorithm, to reduce latency
  enableNagles(sock, false);

  // Create the input and output streams
  instream = new FdInStream(sock);
  outstream = new FdOutStream(sock);
  ownStreams = true;
}

TcpSocket::~TcpSocket() {
  if (closeFd)
    closesocket(getFd());
}

int TcpSocket::getMyPort() {
  return getSockPort(getFd());
}

char* TcpSocket::getPeerAddress() {
  struct sockaddr_in  info;
  struct in_addr    addr;
  socklen_t info_size = sizeof(info);

  getpeername(getFd(), (struct sockaddr *)&info, &info_size);
  memcpy(&addr, &info.sin_addr, sizeof(addr));

  char* name = inet_ntoa(addr);
  if (name) {
    return rfb::strDup(name);
  } else {
    return rfb::strDup("");
  }
}

int TcpSocket::getPeerPort() {
  struct sockaddr_in  info;
  socklen_t info_size = sizeof(info);

  getpeername(getFd(), (struct sockaddr *)&info, &info_size);
  return ntohs(info.sin_port);
}

char* TcpSocket::getPeerEndpoint() {
  rfb::CharArray address; address.buf = getPeerAddress();
  int port = getPeerPort();

  int buflen = strlen(address.buf) + 32;
  char* buffer = new char[buflen];
  sprintf(buffer, "%s::%d", address.buf, port);
  return buffer;
}

bool TcpSocket::sameMachine() {
  vnc_sockaddr_t peeraddr, myaddr;
  socklen_t addrlen;

  addrlen = sizeof(peeraddr);
  if (getpeername(getFd(), &peeraddr.u.sa, &addrlen) < 0)
      throw SocketException ("unable to get peer address", errorNumber);

  addrlen = sizeof(myaddr); /* need to reset, since getpeername overwrote */
  if (getsockname(getFd(), &myaddr.u.sa, &addrlen) < 0)
      throw SocketException ("unable to get my address", errorNumber);

  if (peeraddr.u.sa.sa_family != myaddr.u.sa.sa_family)
      return false;

#ifdef HAVE_GETADDRINFO
  if (peeraddr.u.sa.sa_family == AF_INET6)
      return IN6_ARE_ADDR_EQUAL(&peeraddr.u.sin6.sin6_addr,
				&myaddr.u.sin6.sin6_addr);
#endif

  return (peeraddr.u.sin.sin_addr.s_addr == myaddr.u.sin.sin_addr.s_addr);
}

void TcpSocket::shutdown()
{
  Socket::shutdown();
  ::shutdown(getFd(), 2);
}

bool TcpSocket::enableNagles(int sock, bool enable) {
  int one = enable ? 0 : 1;
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
		 (char *)&one, sizeof(one)) < 0) {
    int e = errorNumber;
    vlog.error("unable to setsockopt TCP_NODELAY: %d", e);
    return false;
  }
  return true;
}

bool TcpSocket::cork(int sock, bool enable) {
#ifndef TCP_CORK
  return false;
#else
  int one = enable ? 1 : 0;
  if (setsockopt(sock, IPPROTO_TCP, TCP_CORK, (char *)&one, sizeof(one)) < 0)
    return false;
  return true;
#endif
}

bool TcpSocket::isSocket(int sock)
{
  struct sockaddr_in info;
  socklen_t info_size = sizeof(info);
  return getsockname(sock, (struct sockaddr *)&info, &info_size) >= 0;
}

bool TcpSocket::isConnected(int sock)
{
  struct sockaddr_in info;
  socklen_t info_size = sizeof(info);
  return getpeername(sock, (struct sockaddr *)&info, &info_size) >= 0;
}

int TcpSocket::getSockPort(int sock)
{
  struct sockaddr_in info;
  socklen_t info_size = sizeof(info);
  if (getsockname(sock, (struct sockaddr *)&info, &info_size) < 0)
    return 0;
  return ntohs(info.sin_port);
}


TcpListener::TcpListener(const char *listenaddr, int port, bool localhostOnly,
			 int sock, bool close_) : closeFd(close_)
{
  if (sock != -1) {
    fd = sock;
    return;
  }

  initSockets();
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    throw SocketException("unable to create listening socket", errorNumber);

#ifndef WIN32
  // - By default, close the socket on exec()
  fcntl(fd, F_SETFD, FD_CLOEXEC);

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		 (char *)&one, sizeof(one)) < 0) {
    int e = errorNumber;
    closesocket(fd);
    throw SocketException("unable to create listening socket", e);
  }
#endif

  // - Bind it to the desired port
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;

  if (localhostOnly) {
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (listenaddr != NULL) {
#ifdef HAVE_INET_ATON
    if (inet_aton(listenaddr, &addr.sin_addr) == 0)
#else
    /* Some systems (e.g. Windows) do not have inet_aton, sigh */
    if ((addr.sin_addr.s_addr = inet_addr(listenaddr)) == INADDR_NONE)
#endif
    {
      closesocket(fd);
      throw Exception("invalid network interface address: %s", listenaddr);
    }
  } else
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* Bind to 0.0.0.0 by default. */

  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    int e = errorNumber;
    closesocket(fd);
    throw SocketException("unable to bind listening socket", e);
  }

  // - Set it to be a listening socket
  if (listen(fd, 5) < 0) {
    int e = errorNumber;
    closesocket(fd);
    throw SocketException("unable to set socket to listening mode", e);
  }
}

TcpListener::~TcpListener() {
  if (closeFd) closesocket(fd);
}

void TcpListener::shutdown()
{
#ifdef WIN32
  closesocket(getFd());
#else
  ::shutdown(getFd(), 2);
#endif
}


Socket*
TcpListener::accept() {
  int new_sock = -1;

  // Accept an incoming connection
  if ((new_sock = ::accept(fd, 0, 0)) < 0)
    throw SocketException("unable to accept new connection", errorNumber);

#ifndef WIN32
  // - By default, close the socket on exec()
  fcntl(new_sock, F_SETFD, FD_CLOEXEC);
#endif

  // Disable Nagle's algorithm, to reduce latency
  TcpSocket::enableNagles(new_sock, false);

  // Create the socket object & check connection is allowed
  TcpSocket* s = new TcpSocket(new_sock);
  if (filter && !filter->verifyConnection(s)) {
    delete s;
    return 0;
  }
  return s;
}

void TcpListener::getMyAddresses(std::list<char*>* result) {
  const hostent* addrs = gethostbyname(0);
  if (addrs == 0)
    throw rdr::SystemException("gethostbyname", errorNumber);
  if (addrs->h_addrtype != AF_INET)
    throw rdr::Exception("getMyAddresses: bad family");
  for (int i=0; addrs->h_addr_list[i] != 0; i++) {
    const char* addrC = inet_ntoa(*((struct in_addr*)addrs->h_addr_list[i]));
    char* addr = new char[strlen(addrC)+1];
    strcpy(addr, addrC);
    result->push_back(addr);
  }
}

int TcpListener::getMyPort() {
  return TcpSocket::getSockPort(getFd());
}


TcpFilter::TcpFilter(const char* spec) {
  rfb::CharArray tmp;
  tmp.buf = rfb::strDup(spec);
  while (tmp.buf) {
    rfb::CharArray first;
    rfb::strSplit(tmp.buf, ',', &first.buf, &tmp.buf);
    if (strlen(first.buf))
      filter.push_back(parsePattern(first.buf));
  }
}

TcpFilter::~TcpFilter() {
}


static bool
patternMatchIP(const TcpFilter::Pattern& pattern, const char* value) {
  unsigned long address = inet_addr((char *)value);
  if (address == INADDR_NONE) return false;
  return ((pattern.address & pattern.mask) == (address & pattern.mask));
}

bool
TcpFilter::verifyConnection(Socket* s) {
  rfb::CharArray name;

  name.buf = s->getPeerAddress();
  std::list<TcpFilter::Pattern>::iterator i;
  for (i=filter.begin(); i!=filter.end(); i++) {
    if (patternMatchIP(*i, name.buf)) {
      switch ((*i).action) {
      case Accept:
        vlog.debug("ACCEPT %s", name.buf);
        return true;
      case Query:
        vlog.debug("QUERY %s", name.buf);
        s->setRequiresQuery();
        return true;
      case Reject:
        vlog.debug("REJECT %s", name.buf);
        return false;
      }
    }
  }

  vlog.debug("[REJECT] %s", name.buf);
  return false;
}


TcpFilter::Pattern TcpFilter::parsePattern(const char* p) {
  TcpFilter::Pattern pattern;

  bool expandMask = false;
  rfb::CharArray addr, mask;

  if (rfb::strSplit(&p[1], '/', &addr.buf, &mask.buf)) {
    if (rfb::strContains(mask.buf, '.')) {
      pattern.mask = inet_addr(mask.buf);
    } else {
      pattern.mask = atoi(mask.buf);
      expandMask = true;
    }
  } else {
    pattern.mask = 32;
    expandMask = true;
  }
  if (expandMask) {
    unsigned long expanded = 0;
    // *** check endianness!
    for (int i=0; i<(int)pattern.mask; i++)
      expanded |= 1<<(31-i);
    pattern.mask = htonl(expanded);
  }

  pattern.address = inet_addr(addr.buf) & pattern.mask;
  if ((pattern.address == INADDR_NONE) ||
      (pattern.address == 0)) pattern.mask = 0;

  switch(p[0]) {
  case '+': pattern.action = TcpFilter::Accept; break;
  case '-': pattern.action = TcpFilter::Reject; break;
  case '?': pattern.action = TcpFilter::Query; break;
  };

  return pattern;
}

char* TcpFilter::patternToStr(const TcpFilter::Pattern& p) {
  in_addr tmp;
  rfb::CharArray addr, mask;
  tmp.s_addr = p.address;
  addr.buf = rfb::strDup(inet_ntoa(tmp));
  tmp.s_addr = p.mask;
  mask.buf = rfb::strDup(inet_ntoa(tmp));
  char* result = new char[strlen(addr.buf)+1+strlen(mask.buf)+1+1];
  switch (p.action) {
  case Accept: result[0] = '+'; break;
  case Reject: result[0] = '-'; break;
  case Query: result[0] = '?'; break;
  };
  result[1] = 0;
  strcat(result, addr.buf);
  strcat(result, "/");
  strcat(result, mask.buf);
  return result;
}
