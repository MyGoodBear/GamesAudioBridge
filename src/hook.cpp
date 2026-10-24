#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <cmath>
#include <tlhelp32.h>
#include <objbase.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <limits>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "asio_ipc.h"

// Game Audio Bridge 2.5: exact sample-format pass-through where OpenAL defines a matching enum; speaker-mask-based channel mapping only. No bit-depth conversion, resampling, or downmix.

namespace {
using ALboolean = char; using ALchar = char; using ALint = int; using ALuint = unsigned int;
using ALsizei = int; using ALenum = int; using ALfloat = float; using ALvoid = void;
struct ALCdevice; struct ALCcontext;
constexpr ALenum AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO16 = 0x1103;
constexpr ALenum AL_FORMAT_MONO_FLOAT32 = 0x10010, AL_FORMAT_STEREO_FLOAT32 = 0x10011;
constexpr ALenum AL_FORMAT_MONO_DOUBLE_EXT = 0x10012, AL_FORMAT_STEREO_DOUBLE_EXT = 0x10013;
constexpr ALenum AL_FORMAT_QUAD32 = 0x1206, AL_FORMAT_51CHN32 = 0x120C, AL_FORMAT_61CHN32 = 0x120F, AL_FORMAT_71CHN32 = 0x1212;
constexpr ALenum AL_EXT_MCFORMATS = 0x1200;
constexpr ALenum ALC_DEVICE_SPECIFIER = 0x1005;
constexpr ALenum AL_VENDOR = 0xB001, AL_VERSION = 0xB002, AL_RENDERER = 0xB003;
constexpr ALenum AL_BUFFERS_PROCESSED = 0x1016, AL_SOURCE_STATE = 0x1010, AL_PLAYING = 0x1012;
using LPALCOPENDEVICE = ALCdevice* (__cdecl*)(const ALchar*);
using LPALCCREATECONTEXT = ALCcontext* (__cdecl*)(ALCdevice*, const ALint*);
using LPALCMAKECONTEXTCURRENT = ALboolean (__cdecl*)(ALCcontext*);
using LPALCDESTROYCONTEXT = void (__cdecl*)(ALCcontext*);
using LPALCCLOSEDEVICE = ALboolean (__cdecl*)(ALCdevice*);
using LPALGENSOURCES = void (__cdecl*)(ALsizei, ALuint*); using LPALDELETESOURCES = void (__cdecl*)(ALsizei, const ALuint*);
using LPALSOURCEI = void (__cdecl*)(ALuint, ALenum, ALint); using LPALSOURCEPLAY = void (__cdecl*)(ALuint);
using LPALSOURCESTOP = void (__cdecl*)(ALuint); using LPALSOURCEQUEUEBUFFERS = void (__cdecl*)(ALuint, ALsizei, const ALuint*);
using LPALSOURCEUNQUEUEBUFFERS = void (__cdecl*)(ALuint, ALsizei, ALuint*); using LPALGENBUFFERS = void (__cdecl*)(ALsizei, ALuint*);
using LPALDELETEBUFFERS = void (__cdecl*)(ALsizei, const ALuint*); using LPALBUFFERDATA = void (__cdecl*)(ALuint, ALenum, const ALvoid*, ALsizei, ALsizei);
using LPALGETSOURCEI = void (__cdecl*)(ALuint, ALenum, ALint*); using LPALGETERROR = ALenum (__cdecl*)();
using LPALCGETSTRING = const ALchar* (__cdecl*)(ALCdevice*, ALenum);
using LPALGETSTRING = const ALchar* (__cdecl*)(ALenum);
struct ALApi {
 HMODULE dll{}; LPALCOPENDEVICE alcOpenDevice{}; LPALCCREATECONTEXT alcCreateContext{}; LPALCMAKECONTEXTCURRENT alcMakeContextCurrent{};
 LPALCDESTROYCONTEXT alcDestroyContext{}; LPALCCLOSEDEVICE alcCloseDevice{}; LPALGENSOURCES alGenSources{}; LPALDELETESOURCES alDeleteSources{};
 LPALSOURCEI alSourcei{}; LPALSOURCEPLAY alSourcePlay{}; LPALSOURCESTOP alSourceStop{}; LPALSOURCEQUEUEBUFFERS alSourceQueueBuffers{};
 LPALSOURCEUNQUEUEBUFFERS alSourceUnqueueBuffers{}; LPALGENBUFFERS alGenBuffers{}; LPALDELETEBUFFERS alDeleteBuffers{}; LPALBUFFERDATA alBufferData{};
 LPALGETSOURCEI alGetSourcei{}; LPALGETERROR alGetError{}; LPALCGETSTRING alcGetString{}; LPALGETSTRING alGetString{};
} al;

std::mutex logMutex;
void Log(const char* msg) {
 char modulePath[MAX_PATH]{}; HMODULE self = nullptr;
 if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&Log), &self) || !GetModuleFileNameA(self, modulePath, MAX_PATH)) return;
 std::string p(modulePath); size_t slash = p.find_last_of("\\/");
 if (slash == std::string::npos) return; p.resize(slash); // DLL directory (x86/x64)
 slash = p.find_last_of("\\/"); if (slash != std::string::npos) p.resize(slash); // Game Audio Bridge root
 p += "\\GameAudioBridge.log";
 std::lock_guard<std::mutex> g(logMutex); FILE* f = nullptr; fopen_s(&f, p.c_str(), "ab"); if (f) { SYSTEMTIME st{}; GetLocalTime(&st); fprintf(f, "%02u:%02u:%02u.%03u [pid %lu] %s\r\n", st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,GetCurrentProcessId(),msg); fclose(f); }
}
struct Chunk { std::vector<uint8_t> bytes; };
std::mutex qMutex; std::deque<Chunk> queue; constexpr size_t MAX_QUEUE = 12;
std::atomic<uint64_t> redirectedBuffers{0}, redirectedFrames{0}, droppedBuffers{0};
std::atomic<bool> outputReady{false}, workerStop{false}; std::thread worker;
std::mutex setupMutex; bool setupAttempted=false; unsigned channels=0, inputChannels=0, sampleRate=0, inputBits=0, inputValidBits=0; DWORD channelMask=0; bool inputFloat=false, inputExtensible=false, outputFloat=false, outputDouble=false, outputPcm32=false, intermediatePcm32=false; ALenum alFormat=0; std::vector<unsigned> openALChannelToInput;
std::mutex stateMutex; std::unordered_map<void*, bool> clientBridge; std::unordered_set<void*> renderBridge;
HANDLE asioMapping=nullptr; GAB_ASIO_IPC::SharedRing* asioRing=nullptr; std::atomic<bool> asioBridgeActive{false};
ALCdevice* device=nullptr; ALCcontext* context=nullptr; ALuint source=0; ALuint buffers[12]{}; bool alReady=false; std::atomic<bool> systemOpenALLoaded{false}; std::atomic<bool> openALDeviceProbeOk{false}; std::atomic<bool> creativeOpenALConfirmed{false}; std::atomic<bool> openALImportsRedirected{false}; std::atomic<bool> openALImportIncompatibleLogged{false};

bool LoadAL() {
 if (systemOpenALLoaded.load(std::memory_order_acquire)) return true;
 // Load the system OpenAL runtime explicitly. This avoids accidentally loading a game-local
 // OpenAL Soft copy from the game's current directory when hardware Creative OpenAL is installed.
 wchar_t sysDir[MAX_PATH]{}; UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
 if (!sysLen || sysLen >= MAX_PATH) { Log("GetSystemDirectory failed; cannot select system OpenAL runtime."); return false; }
 std::wstring openalPath(sysDir); openalPath += L"\\OpenAL32.dll";
 al.dll = LoadLibraryW(openalPath.c_str());
 if (!al.dll) { char msg[160]{}; sprintf_s(msg, "System OpenAL32.dll could not be loaded (Win32 error %lu); bridge remains pass-through.", GetLastError()); Log(msg); return false; }
 wchar_t loadedPath[MAX_PATH]{}; GetModuleFileNameW(al.dll, loadedPath, MAX_PATH);
 char loadedMsg[MAX_PATH * 2]{}; WideCharToMultiByte(CP_UTF8, 0, loadedPath, -1, loadedMsg, sizeof(loadedMsg), nullptr, nullptr);
 Log((std::string("Loaded system OpenAL runtime: ") + loadedMsg).c_str());
#define GETAL(field, name) al.field = reinterpret_cast<decltype(al.field)>(GetProcAddress(al.dll, name)); if (!al.field) { Log("Missing OpenAL export; bridge remains pass-through."); return false; }
 GETAL(alcOpenDevice,"alcOpenDevice"); GETAL(alcCreateContext,"alcCreateContext"); GETAL(alcMakeContextCurrent,"alcMakeContextCurrent");
 GETAL(alcDestroyContext,"alcDestroyContext"); GETAL(alcCloseDevice,"alcCloseDevice"); GETAL(alGenSources,"alGenSources"); GETAL(alDeleteSources,"alDeleteSources");
 GETAL(alSourcei,"alSourcei"); GETAL(alSourcePlay,"alSourcePlay"); GETAL(alSourceStop,"alSourceStop"); GETAL(alSourceQueueBuffers,"alSourceQueueBuffers");
 GETAL(alSourceUnqueueBuffers,"alSourceUnqueueBuffers"); GETAL(alGenBuffers,"alGenBuffers"); GETAL(alDeleteBuffers,"alDeleteBuffers"); GETAL(alBufferData,"alBufferData");
 GETAL(alGetSourcei,"alGetSourcei"); GETAL(alGetError,"alGetError"); GETAL(alcGetString,"alcGetString"); GETAL(alGetString,"alGetString");
#undef GETAL
 systemOpenALLoaded.store(true, std::memory_order_release);
 return true;
}
bool LooksLikeCreative(const std::string& vendor, const std::string& renderer, const std::string& device) {
 auto has=[](std::string s, const char* needle) { std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); std::string n(needle); std::transform(n.begin(),n.end(),n.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); return s.find(n)!=std::string::npos; };
 (void)device;
 return has(vendor,"creative") || has(renderer,"creative");
}
void ProbeOpenALDevice() {
 if (!LoadAL()) return;
 ALCdevice* probeDevice = al.alcOpenDevice(nullptr);
 if (!probeDevice) { Log("OpenAL system runtime loaded, but alcOpenDevice(default) failed; native FMOD OpenAL will not be forced."); return; }
 ALCcontext* probeContext = al.alcCreateContext(probeDevice, nullptr);
 if (!probeContext) { Log("OpenAL default device opened, but context creation failed; native FMOD OpenAL will not be forced."); al.alcCloseDevice(probeDevice); return; }
 if (al.alcMakeContextCurrent(probeContext)) {
   auto safeAL = [](const ALchar* p) -> std::string { return p ? std::string(p) : std::string("(unknown)"); };
   std::string vendor = safeAL(al.alGetString(AL_VENDOR));
   std::string renderer = safeAL(al.alGetString(AL_RENDERER));
   std::string version = safeAL(al.alGetString(AL_VERSION));
   std::string devName = safeAL(al.alcGetString(probeDevice, ALC_DEVICE_SPECIFIER));
   Log((std::string("OpenAL probe: vendor=") + vendor + "; renderer=" + renderer + "; version=" + version + "; device=" + devName).c_str());
   bool creative = LooksLikeCreative(vendor, renderer, devName);
   creativeOpenALConfirmed.store(creative, std::memory_order_release);
   openALDeviceProbeOk.store(creative, std::memory_order_release);
   if (!creative) Log("Creative OpenAL mode unavailable: detected OpenAL identity does not identify Creative; bridge will not force OpenAL redirection.");
   else Log("Creative OpenAL implementation/device identified. This does not independently prove hardware acceleration for every path.");
   al.alcMakeContextCurrent(nullptr);
 } else Log("OpenAL probe context could not be made current; native FMOD OpenAL will not be forced.");
 al.alcDestroyContext(probeContext);
 al.alcCloseDevice(probeDevice);
}
void OutputThread() {
 if (!alReady || !al.alcMakeContextCurrent(context)) { Log("OpenAL context could not be made current on output thread."); outputReady=false; return; }
 al.alGenSources(1,&source); al.alGenBuffers(12,buffers);
 std::deque<ALuint> freeBuffers; for (auto b:buffers) freeBuffers.push_back(b);
 outputReady.store(source != 0, std::memory_order_release);
 if (!source) { Log("OpenAL source creation failed."); return; }
 while (!workerStop.load()) {
   ALint processed=0; al.alGetSourcei(source,AL_BUFFERS_PROCESSED,&processed);
   while (processed-- > 0) { ALuint b=0; al.alSourceUnqueueBuffers(source,1,&b); freeBuffers.push_back(b); }
   Chunk c; bool have=false;
   { std::lock_guard<std::mutex> g(qMutex); if (!queue.empty() && !freeBuffers.empty()) { c=std::move(queue.front()); queue.pop_front(); have=true; } }
   if (have) {
     ALuint b=freeBuffers.front(); freeBuffers.pop_front();
     if (c.bytes.size() <= static_cast<size_t>(INT_MAX)) {
       // Exact payload pass-through: do not clamp or quantize Float32 samples.
       // alFormat must describe the bytes actually stored in c.bytes.
       al.alBufferData(b,alFormat,c.bytes.data(),static_cast<ALsizei>(c.bytes.size()),static_cast<ALsizei>(sampleRate));
       ALenum bufferError=al.alGetError();
       if (bufferError==0) { al.alSourceQueueBuffers(source,1,&b); ALint state=0; al.alGetSourcei(source,AL_SOURCE_STATE,&state); if (state!=AL_PLAYING) al.alSourcePlay(source); }
       else {
         static std::atomic<bool> formatErrorLogged{false};
         if (!formatErrorLogged.exchange(true)) { char msg[192]{}; sprintf_s(msg, "OpenAL rejected alBufferData at runtime: error=0x%04X; enum=0x%04X; channels=%u; sampleType=%s; bits=%u; validBits=%u; rate=%u; mask=0x%08lX. Check the selected OpenAL enum, channel mapping, and source format.", static_cast<unsigned>(bufferError), static_cast<unsigned>(alFormat), channels, inputFloat?"IEEE_FLOAT":"PCM", inputBits, inputValidBits, sampleRate, static_cast<unsigned long>(channelMask)); Log(msg); }
         freeBuffers.push_back(b);
       }
     } else freeBuffers.push_back(b);
   } else Sleep(2);
 }
 al.alSourceStop(source); ALint processed=0; al.alGetSourcei(source,AL_BUFFERS_PROCESSED,&processed);
 while (processed-- > 0) { ALuint b=0; al.alSourceUnqueueBuffers(source,1,&b); }
 al.alDeleteSources(1,&source); al.alDeleteBuffers(12,buffers); al.alcMakeContextCurrent(nullptr);
}

bool GetInputFormat(const WAVEFORMATEX* wf, unsigned& ch, unsigned& bits, unsigned& validBits, bool& isFloat, DWORD& mask, bool& extensible) {
 if (!wf || wf->nChannels < 1 || wf->nSamplesPerSec == 0) return false;
 ch=wf->nChannels; bits=wf->wBitsPerSample; validBits=bits; isFloat=false; mask=0; extensible=false;
 if (wf->wFormatTag==WAVE_FORMAT_IEEE_FLOAT) isFloat=true;
 else if (wf->wFormatTag==WAVE_FORMAT_PCM) isFloat=false;
 else if (wf->wFormatTag==WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
   auto ex=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf); extensible=true; mask=ex->dwChannelMask; validBits=ex->Samples.wValidBitsPerSample;
   if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) isFloat=true;
   else if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) isFloat=false;
   else return false;
 } else return false;
 if (isFloat && bits!=32 && bits!=64) return false;
 if (!isFloat && bits!=8 && bits!=16 && bits!=24 && bits!=32 && bits!=64) return false;
 if (!isFloat && bits==64 && validBits!=64) return false;
 if (!validBits || validBits>bits) return false;
 // Preserve the original channel metadata. Do not apply an app-side channel-mask allowlist.
 return true;
}

std::string DescribeWaveFormat(const WAVEFORMATEX* wf) {
 if (!wf) return "format=null";
 char b[512]{};
 sprintf_s(b, "formatTag=0x%04X; channels=%u; sampleRate=%lu Hz; bitsPerSample=%u; blockAlign=%u; avgBytesPerSec=%lu; cbSize=%u",
   wf->wFormatTag, wf->nChannels, wf->nSamplesPerSec, wf->wBitsPerSample, wf->nBlockAlign, wf->nAvgBytesPerSec, wf->cbSize);
 std::string out(b);
 if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
   const auto ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
   char extra[160]{};
   sprintf_s(extra, "; validBits=%u; channelMask=0x%08lX; subFormat=%s",
     ex->Samples.wValidBitsPerSample, ex->dwChannelMask,
     IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? "IEEE_FLOAT" :
     IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ? "PCM" : "other/unknown");
   out += extra;
 }
 return out;
}

float ReadSample(const uint8_t* p);

bool HasASIOHost() {
 HANDLE h=OpenFileMappingW(FILE_MAP_READ,FALSE,GAB_ASIO_IPC::kMappingName);
 if(!h) return false;
 CloseHandle(h); return true;
}

bool SetupASIOBridge(const WAVEFORMATEX* wf) {
 if (!wf) return false;
 HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,GAB_ASIO_IPC::kMappingName);
 if (!mapping) return false;
 auto* ring=static_cast<GAB_ASIO_IPC::SharedRing*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(GAB_ASIO_IPC::SharedRing)));
 if (!ring || ring->magic!=GAB_ASIO_IPC::kMagic) { if(ring)UnmapViewOfFile(ring); CloseHandle(mapping); return false; }
 unsigned inCh=0,bits=0,valid=0; bool isFloat=false,ext=false; DWORD mask=0;
 if (!GetInputFormat(wf,inCh,bits,valid,isFloat,mask,ext) || inCh>GAB_ASIO_IPC::kMaxChannels) {
   Log("ASIO bridge rejected the stream: unsupported sample format or channel count. Original audio remains active.");
   UnmapViewOfFile(ring); CloseHandle(mapping); return false;
 }
 const LONG targetRate=InterlockedCompareExchange(&ring->sampleRate,0,0);
 const bool rateMismatch=wf->nSamplesPerSec!=static_cast<UINT>(targetRate);
 InterlockedExchange(&ring->inputSampleRate,static_cast<LONG>(wf->nSamplesPerSec));
 InterlockedExchange(&ring->inputBits,static_cast<LONG>(bits));
 InterlockedExchange(&ring->formatMismatch,rateMismatch?1:0);
 if(rateMismatch && !InterlockedCompareExchange(&ring->resamplingEnabled,0,0)) {
   char msg[256]{}; sprintf_s(msg,"ASIO FORMAT MISMATCH: game=%u Hz/%u-bit; ASIO=%ld Hz. SRC disabled; stream will not be redirected.",wf->nSamplesPerSec,bits,targetRate); Log(msg);
   UnmapViewOfFile(ring); CloseHandle(mapping); return false;
 }
 if(rateMismatch) {
   char msg[256]{}; sprintf_s(msg,"ASIO sample-rate mismatch: game=%u Hz/%u-bit, ASIO output=%ld Hz. High-quality windowed-sinc SRC enabled; input samples are converted before routing.",wf->nSamplesPerSec,bits,targetRate); Log(msg);
 }
 if (asioRing) UnmapViewOfFile(asioRing);
 if (asioMapping) CloseHandle(asioMapping);
 asioMapping=mapping; asioRing=ring; inputChannels=inCh; inputBits=bits; inputValidBits=valid; inputFloat=isFloat; inputExtensible=ext; channelMask=mask; sampleRate=wf->nSamplesPerSec;
 asioBridgeActive.store(true,std::memory_order_release);
 Log("ASIO shared-memory bridge connected: game WASAPI buffers will be converted to float64 and sent to the selected ASIO driver.");
 return true;
}

bool WriteASIOBuffer(const BYTE* src, UINT32 frames) {
 if (!asioBridgeActive.load(std::memory_order_acquire) || !asioRing || asioRing->magic!=GAB_ASIO_IPC::kMagic || !src || !frames) return false;
 if (InterlockedCompareExchange(&asioRing->writerLock,1,0)!=0) return false;
 LONG64 w=InterlockedCompareExchange64(&asioRing->writeFrame,0,0);
 LONG64 r=InterlockedCompareExchange64(&asioRing->readFrame,0,0);
 const size_t bytes=inputBits/8; if(!bytes || inputChannels==0 || inputChannels>GAB_ASIO_IPC::kMaxChannels) { InterlockedExchange(&asioRing->writerLock,0); return false; }
 const LONG targetRate=InterlockedCompareExchange(&asioRing->sampleRate,0,0);
 const bool mismatch=sampleRate!=static_cast<unsigned>(targetRate);
 if(mismatch && !InterlockedCompareExchange(&asioRing->resamplingEnabled,0,0)) { InterlockedExchange(&asioRing->writerLock,0); return false; }
 // 32-tap Blackman-windowed sinc interpolation. Internal samples stay float64.
 const double step=static_cast<double>(sampleRate)/static_cast<double>(targetRate);
 const UINT32 outFrames=mismatch?static_cast<UINT32>(std::max(1.0,std::floor(static_cast<double>(frames)/step))):frames;
 if(w-r+outFrames>GAB_ASIO_IPC::kCapacityFrames) { InterlockedExchange(&asioRing->writerLock,0); return false; }
 auto srcAt=[&](long frame,LONG c)->double { frame=std::max<long>(0,std::min<long>(static_cast<long>(frames)-1,frame)); return c<static_cast<LONG>(inputChannels)?ReadSample(src+(static_cast<size_t>(frame)*inputChannels+c)*bytes):0.0; };
 const double cutoff=0.95*std::min(1.0,static_cast<double>(targetRate)/static_cast<double>(sampleRate));
 for(UINT32 of=0;of<outFrames;of++) {
   const double pos=mismatch?static_cast<double>(of)*step:static_cast<double>(of);
   const long center=static_cast<long>(std::floor(pos));
   for(LONG c=0;c<GAB_ASIO_IPC::kMaxChannels;c++) {
     double value=0.0,weightSum=0.0;
     if(c<static_cast<LONG>(inputChannels)) {
       if(!mismatch) value=srcAt(center,c);
       else for(long tap=-15;tap<=16;tap++) {
         const long ix=center+tap; const double x=(pos-static_cast<double>(ix))*cutoff;
         const double sinc=std::abs(x)<1e-12?1.0:std::sin(3.14159265358979323846*x)/(3.14159265358979323846*x);
         const double z=static_cast<double>(tap)/16.0;
         const double window=0.42+0.5*std::cos(3.14159265358979323846*z)+0.08*std::cos(2.0*3.14159265358979323846*z);
         const double weight=cutoff*sinc*window; value+=srcAt(ix,c)*weight; weightSum+=weight;
       }
       if(mismatch && std::abs(weightSum)>1e-12) value/=weightSum;
     }
     asioRing->samples[GAB_ASIO_IPC::SampleIndex(w+of,c)]=std::clamp(value,-1.0,1.0);
   }
 }
 MemoryBarrier(); InterlockedExchange64(&asioRing->writeFrame,w+outFrames);
 InterlockedExchange(&asioRing->writerLock,0);
 return true;
}

bool SetupOpenAL(const WAVEFORMATEX* wf) {
 std::lock_guard<std::mutex> g(setupMutex);
 Log((std::string("WASAPI render stream format: ") + DescribeWaveFormat(wf)).c_str());
 unsigned inCh=0,bits=0,valid=0; bool isFloat=false,ext=false; DWORD mask=0;
 if (!GetInputFormat(wf,inCh,bits,valid,isFloat,mask,ext)) { Log((std::string("Unsupported/ambiguous WASAPI format; original output retained. Details: ") + DescribeWaveFormat(wf)).c_str()); return false; }
 if (setupAttempted) return outputReady.load() && inputChannels==inCh && sampleRate==wf->nSamplesPerSec && inputBits==bits && inputFloat==isFloat && channelMask==mask;
 setupAttempted=true; inputChannels=inCh; inputBits=bits; inputValidBits=valid; inputFloat=isFloat; inputExtensible=ext; channelMask=mask; sampleRate=wf->nSamplesPerSec;
 if (!LoadAL()) return false;
 device=al.alcOpenDevice(nullptr); if (!device) { Log("OpenAL could not open default device; original output retained."); return false; }
 context=al.alcCreateContext(device,nullptr); if (!context) { Log("OpenAL context creation failed; original output retained."); al.alcCloseDevice(device); device=nullptr; return false; }
 al.alcMakeContextCurrent(context);
 auto safeAL = [](const ALchar* p) -> std::string { return p ? std::string(p) : std::string("(unknown)"); };
 std::string vendor = safeAL(al.alGetString(AL_VENDOR));
 std::string renderer = safeAL(al.alGetString(AL_RENDERER));
 std::string version = safeAL(al.alGetString(AL_VERSION));
 std::string devName = safeAL(al.alcGetString(device, ALC_DEVICE_SPECIFIER));
 Log((std::string("OpenAL runtime identity: vendor=") + vendor + "; renderer=" + renderer + "; version=" + version + "; device=" + devName).c_str());
 // The user explicitly selected OpenAL output. Do not require a Creative vendor string:
 // any successfully opened OpenAL device may be used. Device/format API failures are still logged.
 creativeOpenALConfirmed.store(LooksLikeCreative(vendor, renderer, devName), std::memory_order_release);
 al.alcMakeContextCurrent(nullptr);
 // GAB 2.5: preserve sample representation. Float32 and PCM16 support standard
 // layouts; Float64 is pass-through only for mono/stereo when AL_EXT_double exists.
 // Signed PCM Int64 has no standard OpenAL buffer enum, so it must not be mislabeled.
 const bool exactFloat32 = isFloat && bits==32 && valid==32;
 const bool exactFloat64 = isFloat && bits==64 && valid==64 && inCh<=2;
 const bool exactPcm16 = !isFloat && bits==16 && valid==16;
 if (!exactFloat32 && !exactFloat64 && !exactPcm16) {
   char msg[512]{};
   sprintf_s(msg, "No OpenAL enum supports exact pass-through for this source format/layout; no sample conversion attempted, original WASAPI path retained: %s.", DescribeWaveFormat(wf).c_str());
   Log(msg);
   al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
 }
 channels=inCh;
 outputFloat=exactFloat32;
 outputDouble=exactFloat64;
 outputPcm32=false;
 intermediatePcm32=false;
 if (exactFloat32) {
   if (channels==1) alFormat=AL_FORMAT_MONO_FLOAT32;
   else if (channels==2) alFormat=AL_FORMAT_STEREO_FLOAT32;
   else if (channels==4) alFormat=AL_FORMAT_QUAD32;
   else if (channels==6) alFormat=AL_FORMAT_51CHN32;
   else if (channels==7) alFormat=AL_FORMAT_61CHN32;
   else if (channels==8) alFormat=AL_FORMAT_71CHN32;
 } else if (exactFloat64) {
   if (channels==1) alFormat=AL_FORMAT_MONO_DOUBLE_EXT;
   else if (channels==2) alFormat=AL_FORMAT_STEREO_DOUBLE_EXT;
 } else {
   if(channels==1) alFormat=AL_FORMAT_MONO16;
   else if(channels==2) alFormat=AL_FORMAT_STEREO16;
   else if(channels==4) alFormat=0x1205; // AL_EXT_MCFORMATS QUAD16
   else if(channels==6) alFormat=0x120B; // AL_EXT_MCFORMATS 5.1 16-bit
   else if(channels==7) alFormat=0x120E; // AL_EXT_MCFORMATS 6.1 16-bit
   else if(channels==8) alFormat=0x1211; // AL_EXT_MCFORMATS 7.1 16-bit
 }
 // WASAPI WAVEFORMATEXTENSIBLE stores samples in ascending speaker-mask bit order.
 // OpenAL MCFORMATS uses its documented canonical order. Build an explicit output->input
 // map so channels are routed by speaker identity, not merely by channel count.
 openALChannelToInput.clear();
 if (channels<=2) {
   openALChannelToInput.resize(channels);
   for (unsigned i=0;i<channels;i++) openALChannelToInput[i]=i;
 } else {
   unsigned maskChannels=0; for (unsigned bit=0;bit<32;bit++) if (mask & (DWORD(1u)<<bit)) ++maskChannels;
   if (!ext || mask==0 || maskChannels!=channels) {
     Log("Multichannel OpenAL redirect refused: WASAPI speaker mask is absent or does not match channel count; refusing ambiguous channel routing.");
     al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
   }
   std::vector<DWORD> sourceSpeakers;
   for (unsigned bit=0;bit<32;bit++) if (mask & (DWORD(1u)<<bit)) sourceSpeakers.push_back(DWORD(1u)<<bit);
   std::vector<DWORD> targetSpeakers;
   const DWORD FL=0x1, FR=0x2, FC=0x4, LFE=0x8, BL=0x10, BR=0x20, BC=0x100, SL=0x200, SR=0x400;
   if (channels==4) targetSpeakers={FL,FR,BL,BR};
   else if (channels==6) targetSpeakers={FL,FR,FC,LFE,SL,SR};
   else if (channels==7) targetSpeakers={FL,FR,FC,LFE,BC,SL,SR};
   else if (channels==8) targetSpeakers={FL,FR,FC,LFE,BL,BR,SL,SR};
   if (targetSpeakers.size()!=channels) {
     Log("OpenAL channel mapping refused: unsupported multichannel layout; no downmix or format conversion attempted.");
     al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
   }
   for (DWORD target : targetSpeakers) {
     DWORD wanted=target;
     auto findSpeaker=[&](DWORD sp)->int { for (size_t i=0;i<sourceSpeakers.size();++i) if(sourceSpeakers[i]==sp) return static_cast<int>(i); return -1; };
     int idx=findSpeaker(wanted);
     // 5.1 Back and Side pairs are equivalent positional slots for AL_EXT_MCFORMATS.
     if (idx<0 && channels==6 && target==SL) idx=findSpeaker(BL);
     if (idx<0 && channels==6 && target==SR) idx=findSpeaker(BR);
     if (idx<0 && channels==4 && target==BL) idx=findSpeaker(SL);
     if (idx<0 && channels==4 && target==BR) idx=findSpeaker(SR);
     if (idx<0) {
       char msg[512]{}; sprintf_s(msg,"OpenAL channel mapping refused: speaker 0x%08lX required by %u-channel OpenAL layout is absent from WASAPI mask 0x%08lX.",static_cast<unsigned long>(wanted),channels,static_cast<unsigned long>(mask)); Log(msg);
       al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
     }
     openALChannelToInput.push_back(static_cast<unsigned>(idx));
   }
 }
 if (alFormat==0) {
   Log((std::string("OpenAL exact-format redirect refused: no OpenAL enum selected for ")+DescribeWaveFormat(wf)).c_str());
   al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
 }
 // OpenAL's alBufferData sample-rate parameter is signed ALsizei; reject only values
 // that cannot be represented by that API type, not a preset audio-rate range.
 if (wf->nSamplesPerSec > static_cast<DWORD>(INT_MAX)) {
   Log((std::string("OpenAL exact-format redirect refused: sample rate cannot be represented by ALsizei; original format: ")+DescribeWaveFormat(wf)).c_str());
   al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
 }
 // Probe the exact enum/rate before HookInitialize allows the game stream to be silenced.
 // This catches format rejection early; it still cannot guarantee every later buffer succeeds.
 if (!al.alcMakeContextCurrent(context)) {
   Log((std::string("OpenAL exact-format probe failed: context could not be made current; original format: ")+DescribeWaveFormat(wf)).c_str());
   al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
 }
 while (al.alGetError()!=0) {} // discard stale errors so the result belongs to this probe
 ALuint probeBuffer=0;
 al.alGenBuffers(1,&probeBuffer);
 const size_t probeSampleBytes = inputBits / 8;
 std::vector<uint8_t> probeData(static_cast<size_t>(channels)*probeSampleBytes,0);
 if (probeBuffer) al.alBufferData(probeBuffer,alFormat,probeData.data(),static_cast<ALsizei>(probeData.size()),static_cast<ALsizei>(sampleRate));
 ALenum probeError = al.alGetError();
 if (probeBuffer) al.alDeleteBuffers(1,&probeBuffer);
 al.alcMakeContextCurrent(nullptr);
 if (!probeBuffer || probeError!=0) {
   char msg[640]{};
   sprintf_s(msg, "OpenAL exact-format preflight rejected stream before redirect: alBufferData error=0x%04X; enum=0x%04X; original=%s; encoding=%s; bits=%u; validBits=%u; mask=0x%08lX. Original WASAPI path retained; no conversion attempted.", static_cast<unsigned>(probeError), static_cast<unsigned>(alFormat), DescribeWaveFormat(wf).c_str(), isFloat?"IEEE_FLOAT":"PCM", bits, valid, static_cast<unsigned long>(mask));
   Log(msg);
   al.alcDestroyContext(context); al.alcCloseDevice(device); context=nullptr; device=nullptr; return false;
 }
 Log((std::string("OpenAL exact-format preflight accepted enum/rate for one-frame test; source format: ")+DescribeWaveFormat(wf)+". Later buffers are still checked separately.").c_str());
 alReady=true; workerStop=false; worker=std::thread(OutputThread);
 for (int i=0;i<100 && !outputReady.load();++i) Sleep(2);
 if (!outputReady.load()) { Log("OpenAL output thread did not become ready; original output retained."); workerStop=true; if(worker.joinable()) worker.join(); al.alcDestroyContext(context); al.alcCloseDevice(device); alReady=false; return false; }
 char msg[320];
 sprintf_s(msg,"GAB 2.5 OpenAL route configured: %u Hz, %u ch, %u-bit %s; no bit-depth conversion, resampling, or downmix. WASAPI speaker-mask mapping applied where needed.",sampleRate,inputChannels,inputBits,inputFloat?"float":"PCM");
 Log(msg);
 return true;
}

float ReadSample(const uint8_t* p) {
 double v=0.0;
 if (inputFloat && inputBits==32) { float f; memcpy(&f,p,4); v=std::isfinite(f)?f:0.0; }
 else if (inputFloat && inputBits==64) { double d; memcpy(&d,p,8); v=std::isfinite(d)?d:0.0; }
 else if (inputBits==8) v=(static_cast<int>(*p)-128)/128.0;
 else if (inputBits==16) { int16_t x; memcpy(&x,p,2); v=x/32768.0; }
 else if (inputBits==24) { int32_t x=(int32_t(p[0]) | (int32_t(p[1])<<8) | (int32_t(p[2])<<16)); if(x&0x800000)x|=~0xffffff; v=x/8388608.0; }
 else if (inputBits==32) { int32_t x; memcpy(&x,p,4); v=x/2147483648.0; }
 else if (inputBits==64 && !inputFloat) { int64_t x; memcpy(&x,p,8); v=static_cast<double>(x)/9223372036854775808.0; }
 return static_cast<float>(std::clamp(v,-1.0,1.0));
}

bool ConvertBuffer(const BYTE* src, UINT32 frames, std::vector<uint8_t>& dst) {
 if (!src || !frames || !inputChannels || !channels || channels!=inputChannels || openALChannelToInput.size()!=channels) return false;
 const size_t sampleBytes=inputBits/8;
 if (!sampleBytes || frames > SIZE_MAX/(sampleBytes*inputChannels)) return false;
 const size_t frameBytes=sampleBytes*inputChannels;
 const size_t byteCount=static_cast<size_t>(frames)*frameBytes;
 dst.resize(byteCount);
 bool identity=true;
 for (unsigned outCh=0;outCh<channels;outCh++) if(openALChannelToInput[outCh]!=outCh) { identity=false; break; }
 if (identity) {
   // True bit-for-bit pass-through for the common WASAPI 7.1 mask 0x63F.
   memcpy(dst.data(),src,byteCount);
   return true;
 }
 // Reorder complete samples only; the sample bytes themselves are never converted.
 for (UINT32 frame=0;frame<frames;frame++) {
   const uint8_t* srcFrame=src+static_cast<size_t>(frame)*frameBytes;
   uint8_t* dstFrame=dst.data()+static_cast<size_t>(frame)*frameBytes;
   for (unsigned outCh=0;outCh<channels;outCh++) {
     const unsigned inCh=openALChannelToInput[outCh];
     memcpy(dstFrame+static_cast<size_t>(outCh)*sampleBytes,
            srcFrame+static_cast<size_t>(inCh)*sampleBytes,sampleBytes);
   }
 }
 return true;
}

// COM vtable patching. Original functions are retained once per interface implementation.
template<class T> bool PatchSlot(T* obj, size_t slot, void* replacement, void** original) {
 if (!obj) return false; void** vt=*reinterpret_cast<void***>(obj); DWORD old=0;
 if (!VirtualProtect(&vt[slot],sizeof(void*),PAGE_EXECUTE_READWRITE,&old)) return false;
 if (original && !*original) *original=vt[slot]; vt[slot]=replacement; DWORD ignored=0; VirtualProtect(&vt[slot],sizeof(void*),old,&ignored); FlushInstructionCache(GetCurrentProcess(),&vt[slot],sizeof(void*)); return true;
}
using CoCreateFn = HRESULT (WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*); CoCreateFn origCoCreate=nullptr;
using EnumFn = HRESULT (STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, DWORD, IMMDeviceCollection**);
using DefaultFn = HRESULT (STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);
using GetDeviceFn = HRESULT (STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, LPCWSTR, IMMDevice**);
using ItemFn = HRESULT (STDMETHODCALLTYPE*)(IMMDeviceCollection*, UINT, IMMDevice**);
using ActivateFn = HRESULT (STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
using InitializeFn = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
using GetServiceFn = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
using GetBufferFn = HRESULT (STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
using ReleaseBufferFn = HRESULT (STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);
EnumFn origEnum=nullptr; DefaultFn origDefault=nullptr; GetDeviceFn origGetDevice=nullptr; ItemFn origItem=nullptr; ActivateFn origActivate=nullptr;
InitializeFn origInitialize=nullptr; GetServiceFn origGetService=nullptr; GetBufferFn origGetBuffer=nullptr; ReleaseBufferFn origReleaseBuffer=nullptr;
// FMOD Ex 4.x (including the 4.22.00 version shown in the user's crash log) has a native
// OpenAL output backend. FMOD_OUTPUTTYPE_OPENAL is enum value 8 in FMOD Ex 4.24 headers;
// this legacy 4.x layout is used here only for FMOD Ex 4.x imports.
using FmodResult = int;
using FmodSystemCreateFn = FmodResult (__stdcall*)(void**);
using FmodSystemSetOutputFn = FmodResult (__stdcall*)(void*, int);
FmodSystemCreateFn origFmodSystemCreate = nullptr;
FmodSystemSetOutputFn origFmodSystemSetOutput = nullptr;
constexpr int FMOD_OUTPUTTYPE_OPENAL = 8;
std::atomic<bool> fmodSeen{false};
FmodResult __stdcall HookFmodSystemSetOutput(void* system, int output);
FmodResult __stdcall HookFmodSystemCreate(void** system);

void HookDevice(IMMDevice* d); void HookClient(IAudioClient* c); void HookRender(IAudioRenderClient* r);
HRESULT STDMETHODCALLTYPE HookItem(IMMDeviceCollection* s, UINT i, IMMDevice** d);
HRESULT STDMETHODCALLTYPE HookEnum(IMMDeviceEnumerator* s,EDataFlow f,DWORD m,IMMDeviceCollection** c) { HRESULT h=origEnum(s,f,m,c); if(SUCCEEDED(h)&&c&&*c){ void** vt=*reinterpret_cast<void***>(*c); if(!origItem) origItem=(ItemFn)vt[4]; PatchSlot(*c,4,(void*)HookItem,nullptr); } return h; }
HRESULT STDMETHODCALLTYPE HookItem(IMMDeviceCollection* s, UINT i, IMMDevice** d) { HRESULT h=origItem(s,i,d); if(SUCCEEDED(h)&&d&&*d) HookDevice(*d); return h; }
HRESULT STDMETHODCALLTYPE HookDefault(IMMDeviceEnumerator* s,EDataFlow f,ERole r,IMMDevice** d) { HRESULT h=origDefault(s,f,r,d); if(SUCCEEDED(h)&&d&&*d) HookDevice(*d); return h; }
HRESULT STDMETHODCALLTYPE HookGetDevice(IMMDeviceEnumerator* s,LPCWSTR id,IMMDevice** d) { HRESULT h=origGetDevice(s,id,d); if(SUCCEEDED(h)&&d&&*d) HookDevice(*d); return h; }
HRESULT STDMETHODCALLTYPE HookActivate(IMMDevice* s,REFIID iid,DWORD cls,PROPVARIANT* p,void** out) {
 HRESULT h=origActivate(s,iid,cls,p,out);
 if(SUCCEEDED(h)&&out&&*out&&IsEqualIID(iid,__uuidof(IAudioClient))) {
   auto c=reinterpret_cast<IAudioClient*>(*out); HookClient(c);
 }
 return h;
}
HRESULT STDMETHODCALLTYPE HookGetService(IAudioClient* s, REFIID iid, void** out);
HRESULT STDMETHODCALLTYPE HookInitialize(IAudioClient* s,AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME dur,REFERENCE_TIME period,const WAVEFORMATEX* fmt,LPCGUID ses) {
   char detail[160]{}; sprintf_s(detail, "IAudioClient::Initialize requested: shareMode=%s; flags=0x%08lX; bufferDuration=%lld 100-ns units; periodicity=%lld 100-ns units",
     mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? "Exclusive" : "Shared", flags, static_cast<long long>(dur), static_cast<long long>(period)); Log(detail);
   bool supported=false;
   if (HasASIOHost()) supported=SetupASIOBridge(fmt); // Do not silently fall back to OpenAL when ASIO was explicitly selected.
   else supported=SetupOpenAL(fmt);
   HRESULT h=origInitialize(s,mode,flags,dur,period,fmt,ses);
   char result[128]{}; sprintf_s(result, "IAudioClient::Initialize result=0x%08lX; bridgeRedirectEligible=%s", static_cast<unsigned long>(h), supported ? "yes" : "no"); Log(result);
   if(SUCCEEDED(h)) { { std::lock_guard<std::mutex> g(stateMutex); clientBridge[s]=supported; } void** vt=*reinterpret_cast<void***>(s); if(!origGetService) origGetService=(GetServiceFn)vt[14]; PatchSlot(s,14,(void*)HookGetService,nullptr); if(supported) Log(HasASIOHost()?"IAudioClient initialized; ASIO redirect eligible.":"IAudioClient initialized; OpenAL redirect eligible."); }
   return h;
}
HRESULT STDMETHODCALLTYPE HookGetService(IAudioClient* s,REFIID iid,void** out) {
 HRESULT h=origGetService(s,iid,out); if(SUCCEEDED(h)&&out&&*out&&IsEqualIID(iid,__uuidof(IAudioRenderClient))) { auto r=reinterpret_cast<IAudioRenderClient*>(*out); bool enabled=false; { std::lock_guard<std::mutex> g(stateMutex); auto it=clientBridge.find(s); enabled=(it!=clientBridge.end() && it->second); } if(enabled) { { std::lock_guard<std::mutex> g(stateMutex); renderBridge.insert(r); } HookRender(r); } } return h;
}
thread_local BYTE* tlsBuffer=nullptr; thread_local UINT32 tlsFrames=0;
HRESULT STDMETHODCALLTYPE HookGetBuffer(IAudioRenderClient* s,UINT32 frames,BYTE** data) { HRESULT h=origGetBuffer(s,frames,data); if(SUCCEEDED(h)){tlsBuffer=(data?*data:nullptr);tlsFrames=frames;} return h; }
HRESULT STDMETHODCALLTYPE HookReleaseBuffer(IAudioRenderClient* s,UINT32 frames,DWORD flags) {
  bool copied=false;
  bool enabled=false; { std::lock_guard<std::mutex> g(stateMutex); enabled=renderBridge.find(s)!=renderBridge.end(); }
  if(enabled && tlsBuffer && frames==tlsFrames && !(flags&AUDCLNT_BUFFERFLAGS_SILENT) && asioBridgeActive.load(std::memory_order_acquire)) {
    if (WriteASIOBuffer(tlsBuffer,frames)) {
      memset(tlsBuffer,0,static_cast<size_t>(frames)*inputChannels*(inputBits/8));
      uint64_t n=redirectedBuffers.fetch_add(1,std::memory_order_relaxed)+1;
      uint64_t totalFrames=redirectedFrames.fetch_add(frames,std::memory_order_relaxed)+frames;
      if ((n%256)==0) { char msg[192]{}; sprintf_s(msg,"WASAPI -> ASIO: buffers=%llu; totalFrames=%llu; sampleRate=%u; inputChannels=%u; internal=float64",static_cast<unsigned long long>(n),static_cast<unsigned long long>(totalFrames),sampleRate,inputChannels); Log(msg); }
    } else {
      uint64_t dropped=droppedBuffers.fetch_add(1,std::memory_order_relaxed)+1;
      if ((dropped%32)==1) Log("ASIO shared ring is full or busy; source buffer was not silenced to avoid losing audio.");
    }
  } else if(enabled && outputReady.load(std::memory_order_acquire) && tlsBuffer && frames==tlsFrames && !(flags&AUDCLNT_BUFFERFLAGS_SILENT)) {
    Chunk c;
    if(ConvertBuffer(tlsBuffer,frames,c.bytes)) {
      { std::lock_guard<std::mutex> g(qMutex); if(queue.size()<MAX_QUEUE){queue.emplace_back(std::move(c));copied=true;} }
      if(copied) {
        memset(tlsBuffer,0,static_cast<size_t>(frames)*inputChannels*(inputBits/8));
        uint64_t n = redirectedBuffers.fetch_add(1, std::memory_order_relaxed) + 1;
        uint64_t totalFrames = redirectedFrames.fetch_add(frames, std::memory_order_relaxed) + frames;
        if ((n % 256) == 0) { char msg[192]{}; sprintf_s(msg, "WASAPI audio redirected: buffers=%llu; totalFrames=%llu; sampleRate=%u; inputChannels=%u", static_cast<unsigned long long>(n), static_cast<unsigned long long>(totalFrames), sampleRate, inputChannels); Log(msg); }
      } else {
        uint64_t dropped = droppedBuffers.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((dropped % 32) == 1) { char msg[128]{}; sprintf_s(msg, "WASAPI bridge queue full; source buffer left unchanged. droppedBuffers=%llu", static_cast<unsigned long long>(dropped)); Log(msg); }
      }
    }
  }
  tlsBuffer=nullptr; tlsFrames=0;
  return origReleaseBuffer(s,frames,flags);
}

// Hook functions are installed lazily when COM objects are returned.
void HookEnumerator(IMMDeviceEnumerator* e) {
 if(!e) return; void** vt=*reinterpret_cast<void***>(e);
 if(!origEnum) { origEnum=(EnumFn)vt[3]; origDefault=(DefaultFn)vt[4]; origGetDevice=(GetDeviceFn)vt[5]; }
 PatchSlot(e,3,(void*)HookEnum,nullptr); PatchSlot(e,4,(void*)HookDefault,nullptr); PatchSlot(e,5,(void*)HookGetDevice,nullptr);
}
void HookDevice(IMMDevice* d) { if(!d) return; void** vt=*reinterpret_cast<void***>(d); if(!origActivate) origActivate=(ActivateFn)vt[3]; PatchSlot(d,3,(void*)HookActivate,nullptr); }
void HookClient(IAudioClient* c) { if(!c) return; void** vt=*reinterpret_cast<void***>(c); if(!origInitialize) origInitialize=(InitializeFn)vt[3]; PatchSlot(c,3,(void*)HookInitialize,nullptr); }
void HookRender(IAudioRenderClient* r) { if(!r) return; void** vt=*reinterpret_cast<void***>(r); if(!origGetBuffer) origGetBuffer=(GetBufferFn)vt[3]; if(!origReleaseBuffer) origReleaseBuffer=(ReleaseBufferFn)vt[4]; PatchSlot(r,3,(void*)HookGetBuffer,nullptr); PatchSlot(r,4,(void*)HookReleaseBuffer,nullptr); }

FmodResult __stdcall HookFmodSystemSetOutput(void* system, int output) {
    if (!origFmodSystemSetOutput) return 31; // FMOD_ERR_UNSUPPORTED
    if (!openALDeviceProbeOk.load(std::memory_order_acquire)) {
        Log("FMOD System_SetOutput left unchanged because the system OpenAL device probe failed.");
        return origFmodSystemSetOutput(system, output);
    }
    if (output != FMOD_OUTPUTTYPE_OPENAL) {
        char msg[160]{}; sprintf_s(msg, "FMOD System_SetOutput requested type %d; forcing legacy FMOD Ex OpenAL type %d.", output, FMOD_OUTPUTTYPE_OPENAL); Log(msg);
    }
    FmodResult r = origFmodSystemSetOutput(system, FMOD_OUTPUTTYPE_OPENAL);
    char msg[160]{}; sprintf_s(msg, "FMOD System_SetOutput(OpenAL) returned %d.", r); Log(msg);
    return r;
}
FmodResult __stdcall HookFmodSystemCreate(void** system) {
    if (!origFmodSystemCreate) return 31;
    FmodResult r = origFmodSystemCreate(system);
    if (r == 0 && system && *system) {
        fmodSeen.store(true);
        if (!openALDeviceProbeOk.load(std::memory_order_acquire)) {
            Log("FMOD Ex detected, but OpenAL device probe failed; leaving FMOD output at the game's requested/default backend.");
            return r;
        }
        if (!origFmodSystemSetOutput) {
            HMODULE fmod = GetModuleHandleW(L"fmodex.dll");
            if (fmod) origFmodSystemSetOutput = reinterpret_cast<FmodSystemSetOutputFn>(GetProcAddress(fmod, "FMOD_System_SetOutput"));
        }
        if (origFmodSystemSetOutput) {
            FmodResult sr = origFmodSystemSetOutput(*system, FMOD_OUTPUTTYPE_OPENAL);
            char msg[192]{}; sprintf_s(msg, "FMOD_System_Create succeeded; requested native OpenAL output (type %d), SetOutput result=%d.", FMOD_OUTPUTTYPE_OPENAL, sr); Log(msg);
        } else Log("FMOD_System_Create detected, but FMOD_System_SetOutput export was not found; native OpenAL forcing unavailable.");
    }
    return r;
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID cls,LPUNKNOWN outer,DWORD ctx,REFIID iid,LPVOID* out) {
 HRESULT h=origCoCreate(cls,outer,ctx,iid,out);
 if(SUCCEEDED(h)&&out&&*out&&IsEqualIID(iid,__uuidof(IMMDeviceEnumerator))) { Log("WASAPI IMMDeviceEnumerator created; WASAPI interception path is active."); HookEnumerator(reinterpret_cast<IMMDeviceEnumerator*>(*out)); }
 return h;
}
// Patch imports for WASAPI, legacy FMOD Ex, and static OpenAL32.dll calls in already loaded modules.
void PatchImports(HMODULE mod) {
 if(!mod) return; auto base=(uint8_t*)mod; auto dos=(IMAGE_DOS_HEADER*)base; if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return;
 auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew); if(nt->Signature!=IMAGE_NT_SIGNATURE) return;
 auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if(!dir.VirtualAddress) return;
 auto imp=(IMAGE_IMPORT_DESCRIPTOR*)(base+dir.VirtualAddress);
 for(;imp->Name; ++imp) {
   auto names=(IMAGE_THUNK_DATA*)(base+imp->OriginalFirstThunk); auto funcs=(IMAGE_THUNK_DATA*)(base+imp->FirstThunk); if(!imp->OriginalFirstThunk) continue;
   const char* importedDll = (const char*)(base + imp->Name);
   bool openALDll = (_stricmp(importedDll, "openal32.dll") == 0);
   if (openALDll) {
     if (!creativeOpenALConfirmed.load(std::memory_order_acquire) || !al.dll) continue;
     bool allExportsAvailable = true;
     for (auto* check = names; check->u1.AddressOfData; ++check) {
       if (IMAGE_SNAP_BY_ORDINAL(check->u1.Ordinal)) { allExportsAvailable = false; break; }
       auto* checkName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + check->u1.AddressOfData);
       if (!GetProcAddress(al.dll, reinterpret_cast<const char*>(checkName->Name))) { allExportsAvailable = false; break; }
     }
     if (!allExportsAvailable) {
       if (!openALImportIncompatibleLogged.exchange(true)) Log("An OpenAL32.dll import table requests exports missing from the validated Creative runtime; that module is left unchanged to avoid mixing OpenAL implementations.");
       continue;
     }
   }
   for(;names->u1.AddressOfData; ++names,++funcs) {
     if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
     auto byName=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
     const char* importName = (const char*)byName->Name;
     bool legacyFmodEx = (_stricmp(importedDll, "fmodex.dll") == 0);
     void* replacement = nullptr;
     if (openALDll && creativeOpenALConfirmed.load(std::memory_order_acquire) && al.dll) {
       replacement = reinterpret_cast<void*>(GetProcAddress(al.dll, importName));
     }
     else if(strcmp(importName,"CoCreateInstance")==0) replacement = (void*)HookCoCreateInstance;
     else if(legacyFmodEx && strcmp(importName,"FMOD_System_Create")==0) replacement = (void*)HookFmodSystemCreate;
     else if(legacyFmodEx && strcmp(importName,"FMOD_System_SetOutput")==0) replacement = (void*)HookFmodSystemSetOutput;
     if (replacement) {
       DWORD old=0; if(VirtualProtect(&funcs->u1.Function,sizeof(void*),PAGE_READWRITE,&old)) {
         void* current=(void*)funcs->u1.Function;
         if (current != replacement) {
           if (replacement == (void*)HookCoCreateInstance && !origCoCreate) origCoCreate=(CoCreateFn)current;
           if (replacement == (void*)HookFmodSystemCreate && !origFmodSystemCreate) origFmodSystemCreate=(FmodSystemCreateFn)current;
           if (replacement == (void*)HookFmodSystemSetOutput && !origFmodSystemSetOutput) origFmodSystemSetOutput=(FmodSystemSetOutputFn)current;
           funcs->u1.Function=(ULONG_PTR)replacement;
           if (openALDll && !openALImportsRedirected.exchange(true)) Log("Static OpenAL32.dll imports are being redirected to the validated Creative OpenAL runtime.");
         }
         DWORD ignored=0; VirtualProtect(&funcs->u1.Function,sizeof(void*),old,&ignored);
       }
     }
   }
 }
}
std::mutex detectedModuleMutex;
std::unordered_set<std::string> detectedAudioModules;
bool LooksLikeAudioModule(std::string name) {
 std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
 static const char* needles[] = { "fmod", "xaudio", "x3daudio", "openal", "dsound", "winmm", "wwise", "ak sound", "aksound", "miles", "bink", "cri_", "criaudio", "wwise", "irrklang", "steam_audio", "phonon", "sdl", "unityplayer", "wwise", "audioengine", "sound", "opus", "vorbis", "bass.dll", "portaudio", "miniaudio", "avrt.dll" };
 for (const char* n : needles) if (name.find(n) != std::string::npos) return true;
 return false;
}
void InstallImports() {
 HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId()); if(snap==INVALID_HANDLE_VALUE) return;
 MODULEENTRY32W me{}; me.dwSize=sizeof(me); if(Module32FirstW(snap,&me)) do {
   char moduleName[MAX_PATH * 2]{}; WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, moduleName, sizeof(moduleName), nullptr, nullptr);
   if (LooksLikeAudioModule(moduleName)) {
     std::string key(moduleName); std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
     bool first=false; { std::lock_guard<std::mutex> g(detectedModuleMutex); first=detectedAudioModules.insert(key).second; }
     if (first) Log((std::string("Audio-related module detected (diagnostic only; not proof of interception): ") + moduleName).c_str());
   }
   PatchImports(me.hModule);
 } while(Module32NextW(snap,&me)); CloseHandle(snap);
}
DWORD WINAPI InitThread(void*) {
 Sleep(50);
 // Preload the system OpenAL runtime before the game/FMOD initializes its output backend.
 // The probe is diagnostic; it does not keep a device open.
 ProbeOpenALDevice();
 // Modules such as fmodex.dll may be loaded after process start. Rescan imports briefly so
 // the FMOD output selection can be changed before the engine initializes its mixer.
 for (int i=0; i<80; ++i) { InstallImports(); if (fmodSeen.load()) break; Sleep(250); }
 InstallImports();
 Log("Hook initialization scan finished. Check this log for FMOD Ex OpenAL selection or WASAPI interception. If neither appears, this game's audio API was not intercepted.");
 return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
 if(reason==DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); HANDLE t=CreateThread(nullptr,0,InitThread,nullptr,0,nullptr); if(t) CloseHandle(t); }
 if(reason==DLL_PROCESS_DETACH) { workerStop=true; /* Avoid joining threads or unloading OpenAL while the Windows loader lock is held. */ }
 return TRUE;
}
