cmake_minimum_required(VERSION 3.16)
project(sve_core LANGUAGES C CXX)

# Portable core for Summaverick Everyday.
#  - On Linux/ARM (the device) this builds libsvecore + the sve CLI, with the whisper ASR backend.
#  - On iOS the same sources are compiled inside the Xcode target (see ios/project.yml); this file is not used there.
#
# Dependencies are expected as sibling checkouts or via -DLLAMA_DIR / -DWHISPER_DIR:
#   git clone https://github.com/ggml-org/llama.cpp   ../deps/llama.cpp
#   git clone https://github.com/ggml-org/whisper.cpp ../deps/whisper.cpp

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SVE_WITH_WHISPER "Build the whisper.cpp speech-recognition backend (device builds)" ON)
option(SVE_BUILD_CLI "Build the sve command-line tool" ON)

set(LLAMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/llama.cpp" CACHE PATH "llama.cpp checkout")
set(WHISPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/whisper.cpp" CACHE PATH "whisper.cpp checkout")

# llama.cpp (brings ggml). Keep the build lean: no examples, tests, tools or server.
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
add_subdirectory(${LLAMA_DIR} ${CMAKE_BINARY_DIR}/llama EXCLUDE_FROM_ALL)

if(SVE_WITH_WHISPER)
  # whisper.cpp must reuse llama's ggml, otherwise two copies of ggml get linked.
  set(WHISPER_USE_SYSTEM_GGML OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
  # whisper.cpp's CMake checks for an existing ggml target and reuses it when present.
  add_subdirectory(${WHISPER_DIR} ${CMAKE_BINARY_DIR}/whisper EXCLUDE_FROM_ALL)
endif()

find_package(SQLite3 REQUIRED)

add_library(svecore STATIC
  src/sve_core.cpp
  src/sve_assistant.cpp
  src/sve_suggest.cpp
  src/sve_llm.cpp
  src/sve_extract.cpp
  src/sve_context.cpp
  src/sve_medcheck.cpp
  src/sve_learn.cpp
  src/sve_store.cpp
  src/sve_audio.cpp
)
target_include_directories(svecore PUBLIC include PRIVATE src)
target_link_libraries(svecore PUBLIC llama SQLite::SQLite3)
if(SVE_WITH_WHISPER)
  target_compile_definitions(svecore PUBLIC SVE_WITH_WHISPER=1)
  target_link_libraries(svecore PUBLIC whisper)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(svecore PUBLIC pthread)
endif()

if(SVE_BUILD_CLI)
  add_executable(sve cli/sve_cli.cpp)
  target_link_libraries(sve PRIVATE svecore)
endif()
