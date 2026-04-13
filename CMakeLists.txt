cmake_minimum_required(VERSION 3.25)

project(test_optimistic_lock_project LANGUAGES CXX)

# ------------------------------------------------------------
# Compiler Verification
# ------------------------------------------------------------
if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    message(FATAL_ERROR "This project supports only GCC or Clang (Linux/macOS).")
endif()

# ------------------------------------------------------------
# C++ Standard
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------------------------------------
# Build Type Defaults
# ------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

find_package(Threads REQUIRED)

# ============================================================
# TARGET: test_optimistic_lock
# ============================================================

# Explicitly defining source files via variable for consistency
set(TEST_UM_SOURCES
    test_um/test_optimistic_lock.cpp
)

add_executable(test_optimistic_lock ${TEST_UM_SOURCES})

target_include_directories(test_optimistic_lock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/um
    ${CMAKE_CURRENT_SOURCE_DIR}/test_um
    ${CMAKE_CURRENT_SOURCE_DIR}/test_common 
)

# Compiler optimization flags (mirroring your original performance focus)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(test_optimistic_lock PRIVATE
        -O3 -march=native -flto=auto -funroll-loops
        -fomit-frame-pointer -fno-rtti -fexceptions -pthread
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(test_optimistic_lock PRIVATE
        -O3 -march=native -flto -funroll-loops
        -fomit-frame-pointer -fno-rtti -fexceptions -pthread 
    )
endif()

target_link_options(test_optimistic_lock PRIVATE -flto)
target_link_libraries(test_optimistic_lock PRIVATE Threads::Threads)

# Platform-specific dependencies (Linux only)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(NUMA_LIBRARY numa REQUIRED)
    target_link_libraries(test_optimistic_lock PRIVATE ${NUMA_LIBRARY})
endif()

set_target_properties(test_optimistic_lock PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
