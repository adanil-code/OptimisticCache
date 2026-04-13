#!/bin/bash

set -e

# Defaults
BUILD_TYPE="Release"
COMPILER="g++"
CLEAN_BUILD=false

SCRIPT_DIR=$(dirname "$(realpath "$0")")

# 0. Clean in-source cache if it exists (prevents configuration pollution)
if [ -f "$SCRIPT_DIR/CMakeCache.txt" ] || [ -d "$SCRIPT_DIR/CMakeFiles" ]; then
    echo "Cleaning in-source CMake cache..."
    rm -f "$SCRIPT_DIR/CMakeCache.txt"
    rm -rf "$SCRIPT_DIR/CMakeFiles"
fi

# 1. Parse Arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -c|--clean) CLEAN_BUILD=true; shift ;;
        -t|--type) BUILD_TYPE="$2"; shift 2 ;;
        --compiler) COMPILER="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: ./build.sh [OPTIONS]"
            echo "  -c, --clean     Wipe build directory before starting"
            echo "  -t, --type      Build type (Release/Debug)"
            echo "  --compiler      Compiler choice (g++, clang++)"
            exit 0
            ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
done

# 2. Wipe old build folder if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo "Wiping build directory..."
    rm -rf build
fi

echo "========================================"
echo " Building test_optimistic_lock          "
echo " Build Type:  $BUILD_TYPE               "
echo " Compiler:    $COMPILER                 "
echo "========================================"

mkdir -p build
cd build

# 3. Configure and Build
echo "-> Configuring CMake..."
CXX=$COMPILER cmake -S .. -B . -DCMAKE_BUILD_TYPE=$BUILD_TYPE

echo "-> Compiling..."
# Automatically uses all available cores on Linux or Mac
cmake --build . -j $(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo "========================================"
echo " Build successful!                      "
echo " Run via: ./build/bin/test_optimistic_lock "
echo "========================================"