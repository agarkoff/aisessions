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

# The app only reads opencode.db through seven sqlite3_* calls, so the whole
# amalgamation is compiled out of those feature sets. Keep WAL and the core
# query machinery intact; that is what opencode.db actually needs. Marking the
# translation unit /Os also keeps its code column slim without touching the
# interactive parts of the app.
set(_sqlite_omits
  SQLITE_THREADSAFE=0
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_GET_TABLE
  SQLITE_OMIT_TRACE
  SQLITE_OMIT_PROGRESS_CALLBACK
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_AUTOVACUUM
  SQLITE_OMIT_BUILTIN_TEST
  SQLITE_OMIT_COMPILEOPTION_DIAGS
  SQLITE_OMIT_DESERIALIZE
  SQLITE_OMIT_EXPLAIN
  SQLITE_OMIT_LOOKASIDE
  SQLITE_OMIT_DBSTAT_VTAB
  SQLITE_OMIT_JSON
  SQLITE_OMIT_FTS3
  SQLITE_OMIT_FTS4
  SQLITE_OMIT_FTS5
  SQLITE_OMIT_RTREE
  SQLITE_OMIT_UTF16
  SQLITE_OMIT_INTROSPECTION_PRAGMAS
  SQLITE_OMIT_SCHEMA_PRAGMAS
  SQLITE_OMIT_MEMORYDB
  SQLITE_OMIT_FOREIGN_KEY
  SQLITE_OMIT_GENERATED_COLUMNS
  SQLITE_OMIT_DECLTYPE
  SQLITE_OMIT_COLUMN_METADATA
  SQLITE_OMIT_LOCALTIME
)
set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES
  COMPILE_DEFINITIONS "${_sqlite_omits}"
  COMPILE_OPTIONS "/Os"
)
unset(_sqlite_omits)

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
  src/core/SessionActions.h
  src/core/SessionActions.cpp
  src/core/Settings.h
  src/core/Settings.cpp
  src/win/MainWindow.h
  src/win/MainWindow.cpp
  src/win/ShellTree.h
  src/win/ShellTree.cpp
  src/win/Theme.h
  src/win/Theme.cpp
  src/win/Trace.h
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