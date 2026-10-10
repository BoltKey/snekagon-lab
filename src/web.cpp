// WebAssembly entry point (built by web/build.sh): the `snek serve` protocol as one function call, run in
// a Web Worker by ui/snek-worker.js. Built without threads, so the engine thinks on a single core.
#include <emscripten/emscripten.h>
#include "serve.hpp"

extern "C" EMSCRIPTEN_KEEPALIVE const char* snek_cmd(const char* line) {
    static bool tables = (initTables(), true);
    static ServeSession session(1);
    static std::string out;
    (void)tables;
    out = session.handle(line);
    return out.c_str();
}

int main() { return 0; }
