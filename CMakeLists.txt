cmake_minimum_required(VERSION 3.20)
project(aisessions_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
if(NOT _bt STREQUAL "DEBUG")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/O2> $<$<COMPILE_LANGUAGE:C>:/O2>)
endif()
add_compile_options(/utf-8 /W4 /permissive-)
add_compile_definitions(
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)

add_executable(aisessions
  src/main.cpp
  src/resource.h
  resources.rc
  src/core/Session.h
  src/core/Session.cpp
  src/core/Json.h
  src/core/Json.cpp
  src/core/SessionLoader.h
  src/core/SessionLoader.cpp
  src/core/Settings.h
  src/core/Settings.cpp
  src/win/MainWindow.h
  src/win/MainWindow.cpp
  src/win/ShellTree.h
  src/win/ShellTree.cpp
  src/win/Theme.h
  src/win/Theme.cpp
  src/win/ToastWindow.h
  src/win/ToastWindow.cpp
  src/win/StrUtil.h
  third_party/sqlite/sqlite3.c
  third_party/sqlite/sqlite3.h
)

target_include_directories(aisessions PRIVATE
  src
  third_party/sqlite
)

target_link_libraries(aisessions PRIVATE
  user32.lib gdi32.lib shell32.lib comctl32.lib uxtheme.lib dwmapi.lib
  advapi32.lib ole32.lib oleaut32.lib shlwapi.lib ws2_32.lib
)

# Development-only console tool for driving the running window from outside
# (screenshots, synthetic clicks, reading the status line).
add_executable(probe tools/probe/probe.cpp)
target_link_libraries(probe PRIVATE
  user32.lib gdi32.lib comctl32.lib shlwapi.lib gdiplus.lib ole32.lib
)

# Embed application manifest (Common Controls v6 visual styles + PerMonitorV2
# DPI) as an embedded resource; disable the linker's auto-manifest so the
# resource manifest is the only one present.
set_target_properties(aisessions PROPERTIES
  WIN32_EXECUTABLE TRUE
  LINK_FLAGS "/MANIFEST:NO"
)