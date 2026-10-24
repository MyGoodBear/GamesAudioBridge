cmake_minimum_required(VERSION 3.21)
project(GameAudioBridge VERSION 2.5 LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "GameAudioBridge requires Windows")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(GameAudioBridgeHook SHARED src/hook.cpp)
target_compile_definitions(GameAudioBridgeHook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(GameAudioBridgeHook PRIVATE ole32 uuid)
set_target_properties(GameAudioBridgeHook PROPERTIES OUTPUT_NAME "GameAudioBridgeHook")
add_executable(GameAudioBridgeLauncher src/launcher.cpp)
target_compile_definitions(GameAudioBridgeLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(GameAudioBridgeLauncher PRIVATE)

# Optional x64 ASIO engine. The engine uses double/float64 internally and exposes
# an explicit source-to-hardware-output routing matrix. Fetch the SDK first with
# tools/Fetch-ASIO-SDK.ps1 or build.ps1 -FetchAsioSdk.
set(GAB_ASIO_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/asio-sdk" CACHE PATH "Path to Steinberg ASIO SDK source root")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND EXISTS "${GAB_ASIO_SDK_ROOT}/common/asio.h")
  add_library(GameAudioBridgeAsio SHARED
    src/asio_backend.cpp
    "${GAB_ASIO_SDK_ROOT}/common/asio.cpp"
    "${GAB_ASIO_SDK_ROOT}/host/asiodrivers.cpp"
    "${GAB_ASIO_SDK_ROOT}/host/pc/asiolist.cpp"
    "${GAB_ASIO_SDK_ROOT}/common/combase.cpp")
  target_include_directories(GameAudioBridgeAsio PRIVATE
    "${GAB_ASIO_SDK_ROOT}/common"
    "${GAB_ASIO_SDK_ROOT}/host"
    "${GAB_ASIO_SDK_ROOT}/host/pc"
    "${GAB_ASIO_SDK_ROOT}/common")
  target_compile_definitions(GameAudioBridgeAsio PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(GameAudioBridgeAsio PRIVATE ole32 advapi32 uuid)
  set_target_properties(GameAudioBridgeAsio PROPERTIES OUTPUT_NAME "GameAudioBridgeAsio")
else()
  message(WARNING "64-bit ASIO backend is not built: ASIO SDK source not found or target is not x64. Use -FetchAsioSdk.")
endif()
