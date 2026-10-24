#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
// ASIO engine internal processing precision is IEEE-754 float64 (double).
// Input to GAB_ASIO_WriteFloat64 is interleaved [frame][source channel].
__declspec(dllexport) int __stdcall GAB_ASIO_GetDriverCount();
__declspec(dllexport) int __stdcall GAB_ASIO_GetDriverName(int index, wchar_t* dst, int dstChars);
__declspec(dllexport) int __stdcall GAB_ASIO_Open(int driverIndex, double sampleRate, long preferredBufferFrames, int sourceChannels);
__declspec(dllexport) int __stdcall GAB_ASIO_CanSampleRate(double rate);
__declspec(dllexport) int __stdcall GAB_ASIO_SetResamplingEnabled(int enabled);
__declspec(dllexport) int __stdcall GAB_ASIO_GetInputFormat(int* rate, int* bits, int* mismatch);
__declspec(dllexport) int __stdcall GAB_ASIO_OpenControlPanel();
__declspec(dllexport) int __stdcall GAB_ASIO_IsOpen();
__declspec(dllexport) int __stdcall GAB_ASIO_GetOutputChannelCount();
__declspec(dllexport) int __stdcall GAB_ASIO_GetOutputChannelName(int index, wchar_t* dst, int dstChars);
// matrix is row-major sourceChannels x outputChannels; any non-zero value enables route.
__declspec(dllexport) int __stdcall GAB_ASIO_SetRoutingMatrix(const unsigned char* matrix, int sourceChannels, int outputChannels);
__declspec(dllexport) int __stdcall GAB_ASIO_WriteFloat64(const double* interleaved, int frames, int sourceChannels);
__declspec(dllexport) void __stdcall GAB_ASIO_Close();
#ifdef __cplusplus
}
#endif
