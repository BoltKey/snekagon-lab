// WebAssembly entry point (built by web/build.sh): the `snek serve` protocol as one function call, run in
// a Web Worker by ui/snek-worker.js. Built without threads, so the engine thinks on a single core.
#include <emscripten/emscripten.h>
#include "serve.hpp"

// solve's progress lines go straight to the page while the worker is still busy with the command
EM_JS(void, post_progress, (const char* json), { postMessage({ progress: UTF8ToString(json) }); });

extern "C" EMSCRIPTEN_KEEPALIVE const char* snek_cmd(const char* line) {
    static bool tables = (initTables(), true);
    static ServeSession session(1);
    static bool hooked = (session.progress = [](const std::string& p) { post_progress(p.c_str()); }, true);
    static std::string out;
    (void)tables;
    (void)hooked;
    out = session.handle(line);
    return out.c_str();
}

int main() { return 0; }
