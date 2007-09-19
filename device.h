/*
 * device.h: IPTV plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id: device.h,v 1.10 2007/09/19 15:14:32 ajhseppa Exp $
 */

#ifndef __IPTV_DEVICE_H
#define __IPTV_DEVICE_H

#include <vdr/device.h>
#include "protocoludp.h"
//#include "protocolrtsp.h"
#include "protocolhttp.h"
#include "protocolfile.h"
#include "streamer.h"

#include "pidfilter.h"

struct filterInfo {
  bool active;
  int fifoDesc;
  char pipeName[128];
  int lastProvided; 
};
    

class cIptvDevice : public cDevice {
  // static ones
public:
  static unsigned int deviceCount;
  static bool Initialize(unsigned int DeviceCount);
  static unsigned int Count(void);
  static cIptvDevice *Get(unsigned int DeviceIndex);

  // private parts
private:
  unsigned int deviceIndex;
  bool isPacketDelivered;
  bool isOpenDvr;
  cRingBufferLinear *tsBuffer;
  int tsBufferPrefill;
  cIptvProtocolUdp *pUdpProtocol;
  cIptvProtocolHttp *pHttpProtocol;
  cIptvProtocolFile *pFileProtocol;
  //cIptvProtocolRtsp *pRtspProtocol;
  cIptvStreamer *pIptvStreamer;
  cMutex mutex;
  trans filter;
  filterInfo filters[32];

  // constructor & destructor
public:
  cIptvDevice(unsigned int DeviceIndex);
  virtual ~cIptvDevice();

  // for channel parsing
private:
  cString GetChannelSettings(const char *Param, int *IpPort, cIptvProtocolIf* *Protocol);
  bool ProvidesIptv(const char *Param) const;

  // for channel selection
public:
  virtual bool ProvidesSource(int Source) const;
  virtual bool ProvidesTransponder(const cChannel *Channel) const;
  virtual bool ProvidesChannel(const cChannel *Channel, int Priority = -1, bool *NeedsDetachReceivers = NULL) const;
protected:
  virtual bool SetChannelDevice(const cChannel *Channel, bool LiveView);

  // for recording
protected:
  virtual bool SetPid(cPidHandle *Handle, int Type, bool On);
  virtual bool OpenDvr(void);
  virtual void CloseDvr(void);
  virtual bool GetTSPacket(uchar *&Data);

  // for section filtering
  virtual int OpenFilter(u_short Pid, u_char Tid, u_char Mask);
};

#endif // __IPTV_DEVICE_H

