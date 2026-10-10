#!/usr/bin/env bash
# Builds the browser version into web/site/ (or the folder given): the engine compiled to WebAssembly plus
# the UI, a static site for GitHub Pages or any static server. Needs Emscripten (em++ on PATH).
#   bash web/build.sh [out]
#   python -m http.server -d web/site 8000      # then http://localhost:8000
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-web/site}
mkdir -p "$out"
em++ -O3 -std=c++20 src/web.cpp -o "$out/snek.js" \
  -sMODULARIZE=1 -sEXPORT_NAME=SnekEngine -sENVIRONMENT=worker \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=8MB \
  -sEXPORTED_FUNCTIONS=_snek_cmd,_main -sEXPORTED_RUNTIME_METHODS=cwrap
sed 's/<html lang="en">/<html lang="en" data-static>/' ui/index.html > "$out/index.html"
grep -q 'data-static' "$out/index.html"
cp ui/snek-worker.js "$out/"
echo "built $out"
