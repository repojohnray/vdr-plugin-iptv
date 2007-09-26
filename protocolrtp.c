/*
 * protocolrtp.c: IPTV plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id: protocolrtp.c,v 1.2 2007/09/26 22:07:45 rahrenbe Exp $
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

#include <vdr/device.h>

#include "common.h"
#include "config.h"
#include "protocolrtp.h"

cIptvProtocolRtp::cIptvProtocolRtp()
: streamPort(1234),
  socketDesc(-1),
  isActive(false)
{
  debug("cIptvProtocolRtp::cIptvProtocolRtp(): %d/%d packets\n",
        IptvConfig.GetRtpBufferSize(), IptvConfig.GetMaxBufferSize());
  streamAddr = strdup("");
  // Allocate receive buffer
  readBuffer = MALLOC(unsigned char, (TS_SIZE * IptvConfig.GetMaxBufferSize()));
  if (!readBuffer)
     error("ERROR: MALLOC() failed in ProtocolRtp()");
}

cIptvProtocolRtp::~cIptvProtocolRtp()
{
  debug("cIptvProtocolRtp::~cIptvProtocolRtp()\n");
  // Drop the multicast group and close the socket
  Close();
  // Free allocated memory
  free(streamAddr);
  free(readBuffer);
}

bool cIptvProtocolRtp::OpenSocket(const int Port)
{
  debug("cIptvProtocolRtp::OpenSocket()\n");
  // If socket is there already and it is bound to a different port, it must
  // be closed first
  if (Port != streamPort) {
     debug("cIptvProtocolRtp::OpenSocket(): Socket tear-down\n");
     CloseSocket();
     }
  // Bind to the socket if it is not active already
  if (socketDesc < 0) {
     int yes = 1;     
     // Create socket
     socketDesc = socket(PF_INET, SOCK_DGRAM, 0);
     if (socketDesc < 0) {
        char tmp[64];
        error("ERROR: socket(): %s", strerror_r(errno, tmp, sizeof(tmp)));
        return false;
        }
     // Make it use non-blocking I/O to avoid stuck read calls
     if (fcntl(socketDesc, F_SETFL, O_NONBLOCK)) {
        char tmp[64];
        error("ERROR: fcntl(): %s", strerror_r(errno, tmp, sizeof(tmp)));
        CloseSocket();
        return false;
        }
     // Allow multiple sockets to use the same PORT number
     if (setsockopt(socketDesc, SOL_SOCKET, SO_REUSEADDR, &yes,
		    sizeof(yes)) < 0) {
        char tmp[64];
        error("ERROR: setsockopt(): %s", strerror_r(errno, tmp, sizeof(tmp)));
        CloseSocket();
        return false;
        }
     // Bind socket
     memset(&sockAddr, '\0', sizeof(sockAddr));
     sockAddr.sin_family = AF_INET;
     sockAddr.sin_port = htons(Port);
     sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
     int err = bind(socketDesc, (struct sockaddr *)&sockAddr, sizeof(sockAddr));
     if (err < 0) {
        char tmp[64];
        error("ERROR: bind(): %s", strerror_r(errno, tmp, sizeof(tmp)));
        CloseSocket();
        return false;
        }
     // Update stream port
     streamPort = Port;
     }
  return true;
}

void cIptvProtocolRtp::CloseSocket(void)
{
  debug("cIptvProtocolRtp::CloseSocket()\n");
  // Check if socket exists
  if (socketDesc >= 0) {
     close(socketDesc);
     socketDesc = -1;
     }
}

bool cIptvProtocolRtp::JoinMulticast(void)
{
  debug("cIptvProtocolRtp::JoinMulticast()\n");
  // Check that stream address is valid
  if (!isActive && !isempty(streamAddr)) {
     // Ensure that socket is valid
     OpenSocket(streamPort);
     // Join a new multicast group
     struct ip_mreq mreq;
     mreq.imr_multiaddr.s_addr = inet_addr(streamAddr);
     mreq.imr_interface.s_addr = htonl(INADDR_ANY);
     int err = setsockopt(socketDesc, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                          sizeof(mreq));
     if (err < 0) {
        char tmp[64];
        error("ERROR: setsockopt(): %s", strerror_r(errno, tmp, sizeof(tmp)));
        return false;
        }
     // Update multicasting flag
     isActive = true;
     }
  return true;
}

bool cIptvProtocolRtp::DropMulticast(void)
{
  debug("cIptvProtocolRtp::DropMulticast()\n");
  // Check that stream address is valid
  if (isActive && !isempty(streamAddr)) {
      // Ensure that socket is valid
      OpenSocket(streamPort);
      // Drop the multicast group
      struct ip_mreq mreq;
      mreq.imr_multiaddr.s_addr = inet_addr(streamAddr);
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      int err = setsockopt(socketDesc, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq,
                           sizeof(mreq));
      if (err < 0) {
         char tmp[64];
         error("ERROR: setsockopt(): %s", strerror_r(errno, tmp, sizeof(tmp)));
         return false;
         }
      // Update multicasting flag
      isActive = false;
     }
  return true;
}

int cIptvProtocolRtp::Read(unsigned char* *BufferAddr)
{
  //debug("cIptvProtocolRtp::Read()\n");
  socklen_t addrlen = sizeof(sockAddr);
  // Wait for data
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  // Use select
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(socketDesc, &rfds);
  int retval = select(socketDesc + 1, &rfds, NULL, NULL, &tv);
  // Check if error
  if (retval < 0) {
     char tmp[64];
     error("ERROR: select(): %s", strerror_r(errno, tmp, sizeof(tmp)));
     return -1;
     }
  // Check if data available
  else if (retval) {
     // Read data from socket
     int len = recvfrom(socketDesc, readBuffer, (TS_SIZE * IptvConfig.GetRtpBufferSize()),
                     MSG_DONTWAIT, (struct sockaddr *)&sockAddr, &addrlen);
     if (len > 0) {
        // http://www.networksorcery.com/enp/rfc/rfc2250.txt
        const int headerlen = 4 * sizeof(uint32_t);
        // Check if payload type is MPEG2 TS and payload contains multiple of TS packet data
        if (((readBuffer[1] & 0x7F) == 33) && (((len - headerlen) % TS_SIZE) == 0)) {
           // Set argument point to payload in read buffer
           *BufferAddr = &readBuffer[headerlen];
           return (len - headerlen);
           }
        }
     }
  return 0;
}

bool cIptvProtocolRtp::Open(void)
{
  debug("cIptvProtocolRtp::Open(): streamAddr=%s\n", streamAddr);
  // Join a new multicast group
  JoinMulticast();
  return true;
}

bool cIptvProtocolRtp::Close(void)
{
  debug("cIptvProtocolRtp::Close(): streamAddr=%s\n", streamAddr);
  // Drop the multicast group
  DropMulticast();
  // Close the socket
  CloseSocket();
  return true;
}

bool cIptvProtocolRtp::Set(const char* Address, const int Port)
{
  debug("cIptvProtocolRtp::Set(): %s:%d\n", Address, Port);
  if (!isempty(Address)) {
    // Drop the multicast group
    DropMulticast();
    // Update stream address and port
    streamAddr = strcpyrealloc(streamAddr, Address);
    streamPort = Port;
    // Join a new multicast group
    JoinMulticast();
    }
  return true;
}