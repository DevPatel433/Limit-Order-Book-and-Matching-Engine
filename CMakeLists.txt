cmake_minimum_required(VERSION 3.21)
project(limit_order_book VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_BENCHMARKS "Build the benchmark suite" ON)
option(LOB_ENABLE_SANITIZERS "Enable AddressSanitizer + UBSan" OFF)
option(LOB_ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with LOB_ENABLE_SANITIZERS)" OFF)
option(LOB_WERROR "Treat warnings as errors" OFF)

if(LOB_ENABLE_SANITIZERS AND LOB_ENABLE_TSAN)
  message(FATAL_ERROR "LOB_ENABLE_SANITIZERS (ASan/UBSan) and LOB_ENABLE_TSAN cannot be combined")
endif()

find_package(Threads REQUIRED)

# Warnings and sanitizer flags apply to our targets only (not to GoogleTest).
add_library(lob_options INTERFACE)
if(MSVC)
  target_compile_options(lob_options INTERFACE /W4)
else()
  target_compile_options(lob_options INTERFACE -Wall -Wextra -Wpedantic)
  if(LOB_WERROR)
    target_compile_options(lob_options INTERFACE -Werror)
  endif()
  if(LOB_ENABLE_SANITIZERS)
    target_compile_options(lob_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lob_options INTERFACE -fsanitize=address,undefined)
  endif()
  if(LOB_ENABLE_TSAN)
    target_compile_options(lob_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(lob_options INTERFACE -fsanitize=thread)
  endif()
endif()

add_library(lob STATIC
  src/arena.cpp
  src/order_book.cpp
  src/events.cpp
  src/matching_engine.cpp
  src/commands.cpp
  src/checker.cpp
  src/replay.cpp
  src/generator.cpp
  src/multi.cpp
  src/multi_generator.cpp
  src/pipeline.cpp
  src/multi_replay.cpp
  src/wal.cpp)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PUBLIC Threads::Threads PRIVATE lob_options)

add_executable(lob_replay apps/lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob lob_options)
add_executable(lob_shell apps/lob_shell.cpp)
target_link_libraries(lob_shell PRIVATE lob lob_options)
add_executable(lob_gen apps/lob_gen.cpp)
target_link_libraries(lob_gen PRIVATE lob lob_options)
add_executable(lob_mreplay apps/lob_mreplay.cpp)
target_link_libraries(lob_mreplay PRIVATE lob lob_options)
add_executable(lob_mgen apps/lob_mgen.cpp)
target_link_libraries(lob_mgen PRIVATE lob lob_options)
add_executable(lob_recover apps/lob_recover.cpp)
target_link_libraries(lob_recover PRIVATE lob lob_options)

if(LOB_BUILD_TESTS)
  enable_testing()
endif()

if(LOB_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(LOB_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)
  add_subdirectory(tests)
endif()
