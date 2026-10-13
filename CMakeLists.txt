cmake_minimum_required(VERSION 3.20)
project(Kernova LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Lets editors (VS Code / clangd) use the same flags as the build: C++20, defines, includes.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(Kernova WIN32
  res/app.rc
  src/main.cpp
  src/common.cpp
  src/table.cpp
  src/graph.cpp
  src/theme.cpp
  src/icons.cpp
  src/processes.cpp
  src/sysinfo.cpp
  src/apps.cpp
  src/msix.cpp
  src/benchmarks.cpp
  src/net.cpp
  src/defrag.cpp
  src/optimize_panel.cpp
  src/power.cpp
  src/recorder.cpp
  src/health.cpp
  src/page_summary.cpp
  src/page_processes.cpp
  src/page_performance.cpp
  src/page_users.cpp
  src/page_recorder.cpp
  src/page_health.cpp
  src/page_connections.cpp
  src/page_power.cpp
  src/page_startup.cpp
  src/page_services.cpp
  src/page_apps.cpp
  src/page_diskspace.cpp
  src/page_benchmarks.cpp
)

target_compile_definitions(Kernova PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C)

# C++ only: the resource compiler (res/app.rc) does not understand these.
target_compile_options(Kernova PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W3;/permissive-;/utf-8;/bigobj;/EHsc;/MP>")

# Static CRT: the .exe runs on any Windows 10/11 PC without installing a runtime.
set_property(TARGET Kernova PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(Kernova PRIVATE
  comctl32 uxtheme shell32 shlwapi ole32 advapi32
  iphlpapi ws2_32 pdh powrprof msi
  d3d11 dxgi d3dcompiler windowsapp
  gdiplus dwmapi psapi wtsapi32 winhttp wevtapi)
