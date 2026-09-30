#!/usr/bin/env bash
# Build the engine on Linux: build/snek
# Uses the newest installed g++ (g++-13 ... g++-7, g++) and the newest C++ standard it knows
# (the code needs C++17, i.e. g++ 7 or newer).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CXX=${CXX:-}
if [ -z "$CXX" ]; then
  for c in g++-14 g++-13 g++-12 g++-11 g++-10 g++-9 g++-8 g++-7 g++ clang++; do
    if command -v "$c" > /dev/null 2>&1; then CXX=$c; break; fi
  done
fi
if [ -z "$CXX" ]; then echo "no C++ compiler found: sudo apt install build-essential"; exit 1; fi
STD=""
for s in c++20 c++2a c++17 c++1z; do
  if echo 'int main(){}' | $CXX -x c++ -std=$s -fsyntax-only - 2>/dev/null; then STD=$s; break; fi
done
echo "compiler: $($CXX --version | head -1), standard: ${STD:-none}"
if [ -z "$STD" ] || ! $CXX -std=$STD -x c++ -fsyntax-only - 2>/dev/null <<'EOF'
#include <utility>
inline int x = 1;
int main() { auto [a, b] = std::pair<int, int>(1, 2); return a + b - 3 + x - 1; }
EOF
then
  echo "This compiler is too old (C++17 needed, g++ 7 or newer). On Ubuntu:"
  echo "  sudo add-apt-repository ppa:ubuntu-toolchain-r/test && sudo apt update && sudo apt install g++-9"
  echo "then run this script again."
  exit 1
fi
$CXX -O3 -march=native -std=$STD -pthread -o build/snek src/main.cpp
./build/snek verify 200
echo "built build/snek"
