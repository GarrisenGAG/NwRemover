cmake_minimum_required(VERSION 3.15)
project(NwRemover CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SDK ${ROOT}/third_party/AETK)
set(WHISPER ${ROOT}/third_party/whisper.cpp)
set(WBUILD ${ROOT}/build/whisper)

add_library(NwRemover SHARED
  src/NwRemover.cpp
  src/Analysis.cpp
  src/Profanity.cpp
  src/Dsp.cpp
  src/Config.cpp
  src/Groq.cpp
  src/Util.cpp
  NwRemover.rc
)

target_include_directories(NwRemover PRIVATE
  ${ROOT}/src
  ${SDK}/Headers
  ${SDK}/Util
  ${WHISPER}/include
  ${WHISPER}/ggml/include
)

target_compile_definitions(NwRemover PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINDOWS
)
target_compile_options(NwRemover PRIVATE -O2 -Wall -Wno-unknown-pragmas)

target_link_libraries(NwRemover PRIVATE
  -Wl,--start-group
  ${WBUILD}/src/libwhisper.a
  ${WBUILD}/ggml/src/ggml.a
  ${WBUILD}/ggml/src/ggml-cpu.a
  ${WBUILD}/ggml/src/ggml-base.a
  -Wl,--end-group
  winhttp
)
target_link_options(NwRemover PRIVATE -static -static-libgcc -static-libstdc++ -fopenmp)

set_target_properties(NwRemover PROPERTIES
  PREFIX ""
  OUTPUT_NAME "NwRemover"
  SUFFIX ".aex"
)