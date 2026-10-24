#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include "asio_backend.h"
#include "asio_ipc.h"
#include "asio.h"
#include "asiodrivers.h"

namespace {
constexpr int kMaxChannels = 64;
constexpr int kQueueFrames = 32768;
AsioDrivers g_drivers;
std::mutex g_lock;
std::atomic<bool> g_running{false};
std::atomic<bool> g_open{false};
long g_inChannels=0, g_outChannels=0, g_bufferFrames=0;
int g_sourceChannels=2;
double g_sampleRate=48000.0;
std::vector<ASIOBufferInfo> g_buffers;
std::vector<ASIOChannelInfo> g_channelInfo;
std::vector<unsigned char> g_route;
std::vector<double> g_ring;
std::atomic<uint64_t> g_writeFrame{0}, g_readFrame{0};
ASIOCallbacks g_callbacks{};
HWND g_window=nullptr;
HANDLE g_ipcMapping=nullptr;
GAB_ASIO_IPC::SharedRing* g_ipc=nullptr;

void clearOutput(long index) {
  for (long c=0;c<g_outChannels;c++) {
    void* p=g_buffers[static_cast<size_t>(c)].buffers[index];
    if (!p) continue;
    const ASIOSampleType t=g_channelInfo[static_cast<size_t>(c)].type;
    size_t bytes=4;
    if (t==ASIOSTFloat64LSB || t==ASIOSTFloat64MSB) bytes=8;
    else if (t==ASIOSTInt16LSB || t==ASIOSTInt16MSB) bytes=2;
    else if (t==ASIOSTInt24LSB || t==ASIOSTInt24MSB) bytes=3;
    else if (t==ASIOSTDSDInt8LSB1 || t==ASIOSTDSDInt8MSB1 || t==ASIOSTDSDInt8NER8) bytes=1;
    memset(p,0,static_cast<size_t>(g_bufferFrames)*bytes);
  }
}
void writeSample(void* dst, ASIOSampleType type, long frame, double v) {
  if (!std::isfinite(v)) v=0.0;
  v=std::max(-1.0,std::min(1.0,v));
  switch(type) {
    case ASIOSTFloat64LSB: reinterpret_cast<double*>(dst)[frame]=v; break;
    case ASIOSTFloat64MSB: { double x=v; unsigned char b[8]; memcpy(b,&x,8); std::reverse(b,b+8); memcpy(static_cast<unsigned char*>(dst)+frame*8,b,8); break; }
    case ASIOSTFloat32LSB: reinterpret_cast<float*>(dst)[frame]=static_cast<float>(v); break;
    case ASIOSTFloat32MSB: { float x=static_cast<float>(v); unsigned char b[4]; memcpy(b,&x,4); std::reverse(b,b+4); memcpy(static_cast<unsigned char*>(dst)+frame*4,b,4); break; }
    case ASIOSTInt16LSB: reinterpret_cast<int16_t*>(dst)[frame]=static_cast<int16_t>(v<0?v*32768.0:v*32767.0); break;
    case ASIOSTInt16MSB: { int16_t x=static_cast<int16_t>(v<0?v*32768.0:v*32767.0); unsigned char b[2]; memcpy(b,&x,2); std::reverse(b,b+2); memcpy(static_cast<unsigned char*>(dst)+frame*2,b,2); break; }
    case ASIOSTInt24LSB: { int32_t x=static_cast<int32_t>(v<0?v*8388608.0:v*8388607.0); unsigned char* b=static_cast<unsigned char*>(dst)+frame*3; b[0]=static_cast<unsigned char>(x); b[1]=static_cast<unsigned char>(x>>8); b[2]=static_cast<unsigned char>(x>>16); break; }
    case ASIOSTInt24MSB: { int32_t x=static_cast<int32_t>(v<0?v*8388608.0:v*8388607.0); unsigned char* b=static_cast<unsigned char*>(dst)+frame*3; b[0]=static_cast<unsigned char>(x>>16); b[1]=static_cast<unsigned char>(x>>8); b[2]=static_cast<unsigned char>(x); break; }
    case ASIOSTInt32LSB: case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24: reinterpret_cast<int32_t*>(dst)[frame]=static_cast<int32_t>(v<0?v*2147483648.0:v*2147483647.0); break;
    case ASIOSTInt32MSB: case ASIOSTInt32MSB16: case ASIOSTInt32MSB18: case ASIOSTInt32MSB20: case ASIOSTInt32MSB24: { int32_t x=static_cast<int32_t>(v<0?v*2147483648.0:v*2147483647.0); unsigned char b[4]; memcpy(b,&x,4); std::reverse(b,b+4); memcpy(static_cast<unsigned char*>(dst)+frame*4,b,4); break; }
    default: break; // Unsupported driver sample types remain silent, never misinterpreted.
  }
}
void bufferSwitch(long index, ASIOBool) {
  if (!g_open.load() || index<0 || index>1) return;
  clearOutput(index);
  uint64_t r=g_readFrame.load(std::memory_order_relaxed);
  const uint64_t w=g_writeFrame.load(std::memory_order_acquire);
  const uint64_t available=w-r;
  long frames=static_cast<long>(std::min<uint64_t>(static_cast<uint64_t>(g_bufferFrames),available));
  LONG64 ipcRead=0; LONG64 ipcWrite=0; long ipcFrames=0;
  if (g_ipc && g_ipc->magic==GAB_ASIO_IPC::kMagic) {
    ipcRead=InterlockedCompareExchange64(&g_ipc->readFrame,0,0);
    ipcWrite=InterlockedCompareExchange64(&g_ipc->writeFrame,0,0);
    ipcFrames=static_cast<long>(std::min<LONG64>(g_bufferFrames,std::max<LONG64>(0,ipcWrite-ipcRead)));
    frames=ipcFrames;
  }
  for (long o=0;o<g_outChannels;o++) {
    void* out=g_buffers[static_cast<size_t>(o)].buffers[index];
    const ASIOSampleType type=g_channelInfo[static_cast<size_t>(o)].type;
    if (!out) continue;
    for (long f=0;f<frames;f++) {
      double sum=0.0;
      for (int s=0;s<g_sourceChannels;s++) {
        if (!g_route[static_cast<size_t>(s)*g_outChannels+o]) continue;
        if (g_ipc && f<ipcFrames) {
          if (s<GAB_ASIO_IPC::kMaxChannels) sum += g_ipc->samples[GAB_ASIO_IPC::SampleIndex(ipcRead+f,s)];
        } else if (!g_ipc && f<frames) {
          const size_t pos=static_cast<size_t>((r+static_cast<uint64_t>(f))%kQueueFrames)*g_sourceChannels+s;
          sum += g_ring[pos];
        }
      }
      writeSample(out,type,f,sum);
    }
  }
  if (g_ipc && ipcFrames>0) InterlockedExchange64(&g_ipc->readFrame,ipcRead+ipcFrames);
  else if (!g_ipc) g_readFrame.store(r+static_cast<uint64_t>(frames),std::memory_order_release);
  ASIOOutputReady();
}
void sampleRateChanged(ASIOSampleRate rate) { g_sampleRate=rate; }
long asioMessage(long selector,long value,void*,double*) {
  if (selector==kAsioSelectorSupported) {
    switch(value) { case kAsioEngineVersion: case kAsioResetRequest: case kAsioBufferSizeChange: case kAsioResyncRequest: case kAsioLatenciesChanged: case kAsioSupportsTimeInfo: return 1; default: return 0; }
  }
  if (selector==kAsioEngineVersion) return 2;
  return 0;
}
ASIOTime* bufferSwitchTimeInfo(ASIOTime* time,long index,ASIOBool direct) { bufferSwitch(index,direct); return time; }
void fillCallbacks() { g_callbacks.bufferSwitch=bufferSwitch; g_callbacks.sampleRateDidChange=sampleRateChanged; g_callbacks.asioMessage=asioMessage; g_callbacks.bufferSwitchTimeInfo=bufferSwitchTimeInfo; }
void releaseDriver() {
  if (g_running.exchange(false)) ASIOStop();
  if (!g_buffers.empty()) ASIODisposeBuffers();
  g_buffers.clear(); g_channelInfo.clear();
  if (g_open.exchange(false)) ASIOExit();
  g_inChannels=g_outChannels=g_bufferFrames=0;
  g_writeFrame=0; g_readFrame=0; g_ring.clear(); g_route.clear();
  if (g_ipc) { g_ipc->magic=0; MemoryBarrier(); UnmapViewOfFile(g_ipc); g_ipc=nullptr; }
  if (g_ipcMapping) { CloseHandle(g_ipcMapping); g_ipcMapping=nullptr; }
  g_drivers.removeCurrentDriver();
}
}

extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_GetDriverCount() {
  char names[128][32]{}; return static_cast<int>(g_drivers.getDriverNames(names,128));
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_GetDriverName(int index,wchar_t* dst,int dstChars) {
  if (!dst || dstChars<2 || index<0) return 0;
  char names[128][32]{}; long n=g_drivers.getDriverNames(names,128); if(index>=n)return 0;
  int r=MultiByteToWideChar(CP_ACP,0,names[index],-1,dst,dstChars); return r>0?1:0;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_Open(int driverIndex,double sampleRate,long preferredBufferFrames,int sourceChannels) {
  std::lock_guard<std::mutex> guard(g_lock); releaseDriver();
  if(driverIndex<0 || sourceChannels<1 || sourceChannels>kMaxChannels || sampleRate<8000 || sampleRate>384000) return 0;
  char names[128][32]{}; long n=g_drivers.getDriverNames(names,128); if(driverIndex>=n)return 0;
  if(!g_drivers.loadDriver(names[driverIndex])) return 0;
  g_window=GetForegroundWindow();
  ASIODriverInfo info{}; info.asioVersion=2; info.sysRef=g_window;
  if(ASIOInit(&info)!=ASE_OK) { g_drivers.removeCurrentDriver(); return 0; }
  g_open=true; g_sourceChannels=sourceChannels; g_sampleRate=sampleRate;
  if(ASIOCanSampleRate(sampleRate)!=ASE_OK || ASIOSetSampleRate(sampleRate)!=ASE_OK) { releaseDriver(); return 0; }
  if(ASIOGetChannels(&g_inChannels,&g_outChannels)!=ASE_OK || g_outChannels<1 || g_outChannels>kMaxChannels) { releaseDriver(); return 0; }
  long minB=0,maxB=0,prefB=0,gran=0;
  if(ASIOGetBufferSize(&minB,&maxB,&prefB,&gran)!=ASE_OK) { releaseDriver(); return 0; }
  long size=preferredBufferFrames>0?preferredBufferFrames:prefB;
  size=std::max(minB,std::min(maxB,size));
  if(gran>0) size=(size/gran)*gran; else if(gran==-1) { long p=1; while(p<size && p<maxB) p<<=1; size=std::max(minB,std::min(maxB,p)); }
  g_bufferFrames=size; g_buffers.resize(static_cast<size_t>(g_outChannels)); g_channelInfo.resize(static_cast<size_t>(g_outChannels));
  for(long i=0;i<g_outChannels;i++) { g_buffers[i].isInput=ASIOFalse; g_buffers[i].channelNum=i; g_buffers[i].buffers[0]=g_buffers[i].buffers[1]=nullptr; g_channelInfo[i].channel=i; g_channelInfo[i].isInput=ASIOFalse; if(ASIOGetChannelInfo(&g_channelInfo[i])!=ASE_OK) { releaseDriver(); return 0; } }
  fillCallbacks();
  if(ASIOCreateBuffers(g_buffers.data(),g_outChannels,g_bufferFrames,&g_callbacks)!=ASE_OK) { releaseDriver(); return 0; }
  g_ring.assign(static_cast<size_t>(kQueueFrames)*g_sourceChannels,0.0);
  g_ipcMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(GAB_ASIO_IPC::SharedRing)),GAB_ASIO_IPC::kMappingName);
  if (g_ipcMapping) {
    g_ipc=static_cast<GAB_ASIO_IPC::SharedRing*>(MapViewOfFile(g_ipcMapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(GAB_ASIO_IPC::SharedRing)));
    if (g_ipc) { ZeroMemory(g_ipc,sizeof(*g_ipc)); g_ipc->magic=GAB_ASIO_IPC::kMagic; g_ipc->sourceChannels=GAB_ASIO_IPC::kMaxChannels; g_ipc->sampleRate=static_cast<LONG>(sampleRate); g_ipc->resamplingEnabled=1; g_ipc->inputSampleRate=0; g_ipc->inputBits=0; g_ipc->formatMismatch=0; }
    else { CloseHandle(g_ipcMapping); g_ipcMapping=nullptr; }
  }
  if (!g_ipc) { releaseDriver(); return 0; }
  g_route.assign(static_cast<size_t>(g_sourceChannels)*g_outChannels,0);
  for(int c=0;c<std::min<int>(g_sourceChannels,static_cast<int>(g_outChannels));c++) g_route[static_cast<size_t>(c)*g_outChannels+c]=1;
  if(ASIOStart()!=ASE_OK) { releaseDriver(); return 0; }
  g_running=true; return 1;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_CanSampleRate(double rate) {
  if (rate < 8000.0 || rate > 384000.0) return 0;
  std::lock_guard<std::mutex> guard(g_lock);
  if (!g_open.load()) return 0;
  return ASIOCanSampleRate(rate)==ASE_OK ? 1 : 0;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_SetResamplingEnabled(int enabled) {
  if (!g_ipc || g_ipc->magic!=GAB_ASIO_IPC::kMagic) return 0;
  InterlockedExchange(&g_ipc->resamplingEnabled, enabled ? 1 : 0); return 1;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_GetInputFormat(int* rate, int* bits, int* mismatch) {
  if (!g_ipc || g_ipc->magic!=GAB_ASIO_IPC::kMagic) return 0;
  if (rate) *rate=InterlockedCompareExchange(&g_ipc->inputSampleRate,0,0);
  if (bits) *bits=InterlockedCompareExchange(&g_ipc->inputBits,0,0);
  if (mismatch) *mismatch=InterlockedCompareExchange(&g_ipc->formatMismatch,0,0);
  return 1;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_OpenControlPanel() {
  std::lock_guard<std::mutex> guard(g_lock); if(!g_open.load()) return 0; return ASIOControlPanel()==ASE_OK?1:0;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_IsOpen() { return g_running.load() && g_ipc!=nullptr ? 1 : 0; }
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_GetOutputChannelCount() { return static_cast<int>(g_outChannels); }
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_GetOutputChannelName(int index,wchar_t* dst,int dstChars) {
  if(!dst||dstChars<2||index<0||index>=g_outChannels)return 0;
  ASIOChannelInfo ci{}; ci.channel=index; ci.isInput=ASIOFalse; if(ASIOGetChannelInfo(&ci)!=ASE_OK)return 0;
  int n=MultiByteToWideChar(CP_ACP,0,ci.name,-1,dst,dstChars); return n>0?1:0;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_SetRoutingMatrix(const unsigned char* matrix,int sourceChannels,int outputChannels) {
  if(!matrix || sourceChannels!=g_sourceChannels || outputChannels!=g_outChannels)return 0;
  std::lock_guard<std::mutex> guard(g_lock);
  for(int s=0;s<sourceChannels;s++)for(int o=0;o<outputChannels;o++)g_route[static_cast<size_t>(s)*outputChannels+o]=matrix[static_cast<size_t>(s)*outputChannels+o]?1:0;
  return 1;
}
extern "C" __declspec(dllexport) int __stdcall GAB_ASIO_WriteFloat64(const double* data,int frames,int sourceChannels) {
  if(!data||frames<=0||sourceChannels!=g_sourceChannels||!g_running.load())return 0;
  uint64_t w=g_writeFrame.load(std::memory_order_relaxed); uint64_t r=g_readFrame.load(std::memory_order_acquire);
  int accepted=std::min<int>(frames,static_cast<int>(kQueueFrames-(w-r)));
  for(int f=0;f<accepted;f++) for(int c=0;c<sourceChannels;c++) g_ring[static_cast<size_t>((w+f)%kQueueFrames)*sourceChannels+c]=data[static_cast<size_t>(f)*sourceChannels+c];
  g_writeFrame.store(w+accepted,std::memory_order_release); return accepted;
}
extern "C" __declspec(dllexport) void __stdcall GAB_ASIO_Close() { std::lock_guard<std::mutex> guard(g_lock); releaseDriver(); }
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) { if(reason==DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); } return TRUE; }
