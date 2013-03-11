/*
 * device.c: IPTV plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include "config.h"
#include "source.h"
#include "device.h"

#define IPTV_MAX_DEVICES MAXDEVICES

static cIptvDevice * IptvDevicesS[IPTV_MAX_DEVICES] = { NULL };

cIptvDevice::cIptvDevice(unsigned int indexP)
: deviceIndexM(indexP),
  dvrFdM(-1),
  isPacketDeliveredM(false),
  isOpenDvrM(false),
  sidScanEnabledM(false),
  pidScanEnabledM(false),
  channelIdM(tChannelID::InvalidID)
{
  unsigned int bufsize = (unsigned int)MEGABYTE(IptvConfig.GetTsBufferSize());
  bufsize -= (bufsize % TS_SIZE);
  isyslog("creating IPTV device %d (CardIndex=%d)", deviceIndexM, CardIndex());
  tsBufferM = new cRingBufferLinear(bufsize + 1, TS_SIZE, false,
                                   *cString::sprintf("IPTV %d", deviceIndexM));
  tsBufferM->SetTimeouts(100, 100);
  tsBufferM->SetIoThrottle();
  ResetBuffering();
  pUdpProtocolM = new cIptvProtocolUdp();
  pCurlProtocolM = new cIptvProtocolCurl();
  pHttpProtocolM = new cIptvProtocolHttp();
  pFileProtocolM = new cIptvProtocolFile();
  pExtProtocolM = new cIptvProtocolExt();
  pIptvStreamerM = new cIptvStreamer(tsBufferM, (100 * TS_SIZE));
  pPidScannerM = new cPidScanner();
  // Initialize filter pointers
  memset(secFiltersM, 0, sizeof(secFiltersM));
  // Start section handler for iptv device
  StartSectionHandler();
  // Sid scanner must be created after the section handler
  AttachFilter(pSidScannerM = new cSidScanner());
  // Check if dvr fifo exists
  struct stat sb;
  cString filename = cString::sprintf(IPTV_DVR_FILENAME, deviceIndexM);
  stat(filename, &sb);
  if (S_ISFIFO(sb.st_mode)) {
     dvrFdM = open(filename, O_RDWR | O_NONBLOCK);
     if (dvrFdM >= 0)
        dsyslog("IPTV device %d redirecting input stream to '%s'", deviceIndexM, *filename);
     }
}

cIptvDevice::~cIptvDevice()
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  DELETE_POINTER(pIptvStreamerM);
  DELETE_POINTER(pUdpProtocolM);
  DELETE_POINTER(pCurlProtocolM);
  DELETE_POINTER(pHttpProtocolM);
  DELETE_POINTER(pFileProtocolM);
  DELETE_POINTER(pExtProtocolM);
  DELETE_POINTER(tsBufferM);
  DELETE_POINTER(pPidScannerM);
  DELETE_POINTER(pSidScannerM);
  // Stop section handler of iptv device
  StopSectionHandler();
  // Destroy all filters
  cMutexLock MutexLock(&mutexM);
  for (int i = 0; i < eMaxSecFilterCount; ++i)
      DeleteFilter(i);
  // Close dvr fifo
  if (dvrFdM >= 0) {
     int fd = dvrFdM;
     dvrFdM = -1;
     close(fd);
     }
}

bool cIptvDevice::Initialize(unsigned int deviceCountP)
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceCountP);
  new cIptvSourceParam(IPTV_SOURCE_CHARACTER, "IPTV");
  if (deviceCountP > IPTV_MAX_DEVICES)
     deviceCountP = IPTV_MAX_DEVICES;
  for (unsigned int i = 0; i < deviceCountP; ++i)
      IptvDevicesS[i] = new cIptvDevice(i);
  for (unsigned int i = deviceCountP; i < IPTV_MAX_DEVICES; ++i)
      IptvDevicesS[i] = NULL;
  return true;
}

unsigned int cIptvDevice::Count(void)
{
  unsigned int count = 0;
  debug("cIptvDevice::%s()", __FUNCTION__);
  for (unsigned int i = 0; i < IPTV_MAX_DEVICES; ++i) {
      if (IptvDevicesS[i] != NULL)
         count++;
      }
  return count;
}

cIptvDevice *cIptvDevice::GetIptvDevice(int cardIndexP)
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, cardIndexP);
  for (unsigned int i = 0; i < IPTV_MAX_DEVICES; ++i) {
      if ((IptvDevicesS[i] != NULL) && (IptvDevicesS[i]->CardIndex() == cardIndexP)) {
         //debug("cIptvDevice::%s(%d): found!", __FUNCTION__, cardIndexP);
         return IptvDevicesS[i];
         }
      }
  return NULL;
}

cString cIptvDevice::GetGeneralInformation(void)
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return cString::sprintf("IPTV device: %d\nCardIndex: %d\nStream: %s\nStream bitrate: %s\n%sChannel: %s",
                          deviceIndexM, CardIndex(),
                          pIptvStreamerM ? *pIptvStreamerM->GetInformation() : "",
                          pIptvStreamerM ? *pIptvStreamerM->GetStreamerStatistic() : "",
                          *GetBufferStatistic(),
                          *Channels.GetByNumber(cDevice::CurrentChannel())->ToText());
}

cString cIptvDevice::GetPidsInformation(void)
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return GetPidStatistic();
}

cString cIptvDevice::GetFiltersInformation(void)
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  unsigned int count = 0;
  cString s("Active section filters:\n");
  // loop through active section filters
  cMutexLock MutexLock(&mutexM);
  for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
      if (secFiltersM[i]) {
         s = cString::sprintf("%sFilter %d: %s Pid=0x%02X (%s)\n", *s, i,
                              *secFiltersM[i]->GetSectionStatistic(), secFiltersM[i]->GetPid(),
                              id_pid(secFiltersM[i]->GetPid()));
         if (++count > IPTV_STATS_ACTIVE_FILTERS_COUNT)
            break;
         }
      }
  return s;
}

cString cIptvDevice::GetInformation(unsigned int pageP)
{
  // generate information string
  cString s;
  switch (pageP) {
    case IPTV_DEVICE_INFO_GENERAL:
         s = GetGeneralInformation();
         break;
    case IPTV_DEVICE_INFO_PIDS:
         s = GetPidsInformation();
         break;
    case IPTV_DEVICE_INFO_FILTERS:
         s = GetFiltersInformation();
         break;
    case IPTV_DEVICE_INFO_PROTOCOL:
         s = pIptvStreamerM ? *pIptvStreamerM->GetInformation() : "";
         break;
    case IPTV_DEVICE_INFO_BITRATE:
         s = pIptvStreamerM ? *pIptvStreamerM->GetStreamerStatistic() : "";
         break;
    default:
         s = cString::sprintf("%s%s%s",
                              *GetGeneralInformation(),
                              *GetPidsInformation(),
                              *GetFiltersInformation());
         break;
    }
  return s;
}

cString cIptvDevice::DeviceType(void) const
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return "IPTV";
}

cString cIptvDevice::DeviceName(void) const
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return cString::sprintf("IPTV %d", deviceIndexM);
}

int cIptvDevice::SignalStrength(void) const
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return (100);
}

int cIptvDevice::SignalQuality(void) const
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return (100);
}

bool cIptvDevice::ProvidesSource(int sourceP) const
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return (cSource::IsType(sourceP, IPTV_SOURCE_CHARACTER));
}

bool cIptvDevice::ProvidesTransponder(const cChannel *channelP) const
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return (ProvidesSource(channelP->Source()));
}

bool cIptvDevice::ProvidesChannel(const cChannel *channelP, int priorityP, bool *needsDetachReceiversP) const
{
  bool result = false;
  bool hasPriority = (priorityP == IDLEPRIORITY) || (priorityP > this->Priority());
  bool needsDetachReceivers = false;

  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);

  if (channelP && ProvidesTransponder(channelP)) {
     result = hasPriority;
     if (Receiving()) {
        if (channelP->GetChannelID() == channelIdM)
           result = true;
        else
           needsDetachReceivers = Receiving();
        }
     }
  if (needsDetachReceiversP)
     *needsDetachReceiversP = needsDetachReceivers;
  return result;
}

bool cIptvDevice::ProvidesEIT(void) const
{
  return false;
}

int cIptvDevice::NumProvidedSystems(void) const
{
  return 1;
}

bool cIptvDevice::SetChannelDevice(const cChannel *channelP, bool liveViewP)
{
  cIptvProtocolIf *protocol;
  cIptvTransponderParameters itp(channelP->Parameters());

  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);

  if (isempty(itp.Address())) {
     error("Unrecognized IPTV address: %s", channelP->Parameters());
     return false;
     }
  switch (itp.Protocol()) {
    case cIptvTransponderParameters::eProtocolUDP:
         protocol = pUdpProtocolM;
         break;
    case cIptvTransponderParameters::eProtocolCURL:
         protocol = pCurlProtocolM;
         break;
    case cIptvTransponderParameters::eProtocolHTTP:
         protocol = pHttpProtocolM;
         break;
    case cIptvTransponderParameters::eProtocolFILE:
         protocol = pFileProtocolM;
         break;
    case cIptvTransponderParameters::eProtocolEXT:
         protocol = pExtProtocolM;
         break;
    default:
         error("Unrecognized IPTV protocol: %s", channelP->Parameters());
         return false;
         break;
  }
  sidScanEnabledM = itp.SidScan() ? true : false;
  pidScanEnabledM = itp.PidScan() ? true : false;
  if (pIptvStreamerM->Set(itp.Address(), itp.Parameter(), deviceIndexM, protocol)) {
     channelIdM = channelP->GetChannelID();
     if (sidScanEnabledM && pSidScannerM && IptvConfig.GetSectionFiltering())
        pSidScannerM->SetChannel(channelIdM);
     if (pidScanEnabledM && pPidScannerM)
        pPidScannerM->SetChannel(channelIdM);
     }
  return true;
}

bool cIptvDevice::SetPid(cPidHandle *handleP, int typeP, bool onP)
{
  debug("cIptvDevice::%s(%d): pid=%d type=%d on=%d", __FUNCTION__, deviceIndexM, handleP->pid, typeP, onP);
  return true;
}

bool cIptvDevice::DeleteFilter(unsigned int indexP)
{
  if ((indexP < eMaxSecFilterCount) && secFiltersM[indexP]) {
     //debug("cIptvDevice::%s(%d): index=%d", __FUNCTION__, deviceIndexM, indexP);
     cIptvSectionFilter *tmp = secFiltersM[indexP];
     secFiltersM[indexP] = NULL;
     delete tmp;
     return true;
     }
  return false;
}

bool cIptvDevice::IsBlackListed(u_short pidP, u_char tidP, u_char maskP) const
{
  //debug("cIptvDevice::%s(%d): pid=%d tid=%02X mask=%02X", __FUNCTION__, deviceIndexM, pidP, tidP, maskP);
  // loop through section filter table
  for (int i = 0; i < SECTION_FILTER_TABLE_SIZE; ++i) {
      int index = IptvConfig.GetDisabledFilters(i);
      // Check if matches
      if ((index >= 0) && (index < SECTION_FILTER_TABLE_SIZE) &&
          (section_filter_table[index].pid == pidP) && (section_filter_table[index].tid == tidP) &&
          (section_filter_table[index].mask == maskP)) {
         //debug("cIptvDevice::%s(%d): found %s", __FUNCTION__, deviceIndexM, section_filter_table[index].description);
         return true;
         }
      }
  return false;
}

int cIptvDevice::OpenFilter(u_short pidP, u_char tidP, u_char maskP)
{
  // Check if disabled by user
  if (!IptvConfig.GetSectionFiltering())
     return -1;
  // Lock
  cMutexLock MutexLock(&mutexM);
  // Blacklist check, refuse certain filters
  if (IsBlackListed(pidP, tidP, maskP))
     return -1;
  // Search the next free filter slot
  for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
      if (!secFiltersM[i]) {
         //debug("cIptvDevice::%s(%d): pid=%d tid=%02X mask=%02X index=%d", __FUNCTION__, deviceIndexM, pidP, tidP, maskP, i);
         secFiltersM[i] = new cIptvSectionFilter(deviceIndexM, pidP, tidP, maskP);
         if (secFiltersM[i])
            return i;
         break;
         }
      }
  // No free filter slot found
  return -1;
}

int cIptvDevice::ReadFilter(int handleP, void *bufferP, size_t lengthP)
{
  // Lock
  cMutexLock MutexLock(&mutexM);
  // ... and load
  if (secFiltersM[handleP]) {
     return secFiltersM[handleP]->Read(bufferP, lengthP);
     //debug("cIptvDevice::%s(%d): handle=%d length=%d", __FUNCTION__, deviceIndexM, handleP, lengthP);
     }
  return 0;
}

void cIptvDevice::CloseFilter(int handleP)
{
  // Lock
  cMutexLock MutexLock(&mutexM);
  // ... and load
  if (secFiltersM[handleP]) {
     //debug("cIptvDevice::%s(%d): handle=%d", __FUNCTION__, deviceIndexM, handleP);
     DeleteFilter(handleP);
     }
}

bool cIptvDevice::OpenDvr(void)
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  isPacketDeliveredM = false;
  tsBufferM->Clear();
  ResetBuffering();
  if (pIptvStreamerM)
     pIptvStreamerM->Open();
  if (sidScanEnabledM && pSidScannerM && IptvConfig.GetSectionFiltering())
     pSidScannerM->Open();
  isOpenDvrM = true;
  return true;
}

void cIptvDevice::CloseDvr(void)
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  if (sidScanEnabledM && pSidScannerM )
     pSidScannerM->Close();
  if (pIptvStreamerM)
     pIptvStreamerM->Close();
  isOpenDvrM = false;
}

bool cIptvDevice::HasLock(int timeoutMsP) const
{
  //debug("cIptvDevice::%s(%d): timeoutMs=%d", __FUNCTION__, deviceIndexM, timeoutMsP);
  return (!IsBuffering());
}

bool cIptvDevice::HasInternalCam(void)
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  return true;
}

void cIptvDevice::ResetBuffering(void)
{
  debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  // Pad prefill to multiple of TS_SIZE
  tsBufferPrefillM = (unsigned int)MEGABYTE(IptvConfig.GetTsBufferSize()) *
                    IptvConfig.GetTsBufferPrefillRatio() / 100;
  tsBufferPrefillM -= (tsBufferPrefillM % TS_SIZE);
}

bool cIptvDevice::IsBuffering(void) const
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  if (tsBufferPrefillM && tsBufferM && tsBufferM->Available() < tsBufferPrefillM)
     return true;
  else
     tsBufferPrefillM = 0;
  return false;
}

bool cIptvDevice::GetTSPacket(uchar *&Data)
{
  //debug("cIptvDevice::%s(%d)", __FUNCTION__, deviceIndexM);
  if (tsBufferM && !IsBuffering()) {
     if (isPacketDeliveredM) {
        tsBufferM->Del(TS_SIZE);
        isPacketDeliveredM = false;
        // Update buffer statistics
        AddBufferStatistic(TS_SIZE, tsBufferM->Available());
        }
     int Count = 0;
     uchar *p = tsBufferM->Get(Count);
     if (p && Count >= TS_SIZE) {
        if (*p != TS_SYNC_BYTE) {
           for (int i = 1; i < Count; i++) {
               if (p[i] == TS_SYNC_BYTE) {
                  Count = i;
                  break;
                  }
               }
           tsBufferM->Del(Count);
           error("Skipped %d bytes to sync on TS packet\n", Count);
           return false;
           }
        isPacketDeliveredM = true;
        Data = p;
        // Update pid statistics
        AddPidStatistic(ts_pid(p), payload(p));
        // Send data also to dvr fifo
        if (dvrFdM >= 0)
           Count = (int)write(dvrFdM, p, TS_SIZE);
        // Analyze incomplete streams with built-in pid analyzer
        if (pidScanEnabledM && pPidScannerM)
           pPidScannerM->Process(p);
        // Lock
        cMutexLock MutexLock(&mutexM);
        // Run the data through all filters
        for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
            if (secFiltersM[i])
               secFiltersM[i]->Process(p);
            }
        return true;
        }
     }
  // Reduce cpu load by preventing busylooping
  cCondWait::SleepMs(10);
  Data = NULL;
  return true;
}
