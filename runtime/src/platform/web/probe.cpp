#include <cstdio>
#include <emscripten.h>

// Only this platform adapter crosses into browser APIs. No translated game code.
EM_ASYNC_JS(int, initialize_web_platform, (), {
    return await Module['platform'].initialize();
});

EM_JS(int, present_web_frame, (), {
    return Module['platform'].frame();
});

static void Frame() {
    if (!present_web_frame()) {
        emscripten_cancel_main_loop();
    }
}

int main() {
    std::puts("[INFO] WASM main entered (game-free platform probe)");
    if (!initialize_web_platform()) {
        std::fputs("[ERROR] Browser platform initialization failed\n", stderr);
        return 1;
    }
    std::puts("[INFO] WiiCompiled Web runtime initialized");
    // Presentation only: this experiment has no guest simulation clock.
    emscripten_set_main_loop(Frame, 0, false);
    return 0;
}
