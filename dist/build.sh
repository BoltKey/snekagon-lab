#!/usr/bin/env bash
# Build the engine on Linux (or macOS): build/snek
set -e
cd "$(dirname "$0")/.."
mkdir -p build
STD=-std=c++20
if ! echo 'int main(){}' | ${CXX:-g++} -x c++ $STD - -o /dev/null 2>/dev/null; then STD=-std=c++2a; fi
${CXX:-g++} -O3 -march=native $STD -pthread -o build/snek src/main.cpp
./build/snek verify 200
echo "built build/snek"
