# Game-free WebGPU/WASM probe

This is the smallest Milestone 1 experiment, **not a port of the full WiiCompiled runtime**. It compiles handwritten C++ `main` to wasm32, calls a web adapter, asynchronously initializes WebGPU, clears a canvas, and prints `WiiCompiled Web runtime initialized` only after initial GPU completion/validation. An Emscripten main-loop callback keeps presenting without blocking the page. It has no simulation clock, game loader, Aurora, audio or persistent saves.

## Build and serve

Install/activate Emscripten **6.0.10**, then use its environment:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -DWIICOMPILED_WEB=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j2
ctest --test-dir build-web --output-on-failure
python3 web/serve.py
```

Open `http://localhost:8000`, then Click to Start. Use modern desktop Chromium with WebGPU enabled and supported by the system. Errors remain in the log. Fullscreen and Copy/Download Log are user-triggered. Device loss stops rendering; reload to retry. No game-file picker is presented because there is no game-loading implementation yet. The web logo was not provided; the text heading is temporary, not a recreated logo.

Desktop builds continue to use `cmake -S runtime ...`; their CMake and source are unchanged. The new root build intentionally rejects non-Emscripten toolchains. `-DWIICOMPILED_WEB_TESTS=OFF` builds just the probe. Tests otherwise compile the existing input-expression and SC serial contract tests to WASM and execute them in Node through CTest. They do not validate a complete browser input/storage implementation.

The server binds only to localhost and serves `build-web/site`, including COOP/COEP headers. Do not serve the source repository, private generated code or game extraction directories. The probe itself is single-threaded and does not require SharedArrayBuffer; headers establish the future threading test environment. Host the six site files together over HTTPS for remote testing. This is not a deployment script or a production hosting configuration.

## Browser automation

With Node and Playwright 1.62.1 installed separately, start the server and run:

```sh
NODE_PATH=/path/to/node_modules node web/tests/smoke.cjs
```

Set `CHROMIUM_PATH` to test an installed Chromium. For headless environments with no usable GPU:

```sh
NODE_PATH=/path/to/node_modules WEBGPU_SOFTWARE=1 node web/tests/smoke.cjs
```

The software option adds explicit Chromium test flags, uses SwiftShader, and must not be used as evidence of hardware support or frame-rate targets. `PROBE_URL` overrides localhost:8000. `PROBE_SCREENSHOT` optionally saves a test screenshot. Tests require actual WebGPU success for the positive path and read canvas pixels; they do not replace the GPU with a mock to manufacture success. Error scenarios inject missing GPU, null adapter, device-request failure and a missing WASM request; device-loss testing destroys a real test device.

## Verified locally on 2026-09-26

- Emscripten 6.0.10, CMake Release build: passed.
- Existing input-expression and SC serial tests compiled as wasm32 and ran in Node: **2/2 passed**.
- Playwright 1.62.1 driving Linux Chromium 153.0.8010.36 with software WebGPU: passed actual WASM entry, GPU clear completion, canvas center pixel `[6, 41, 61, 255]`, resize, cross-origin isolation, and all five failure scenarios.
- Linux Chromium without software/test GPU flags: **no adapter available**. It displayed explicit initialization errors and never logged success. Hardware-backed rendering is therefore **not verified** on this host.
- Initial Release outputs: WASM 12,568 bytes; generated JS 43,411 bytes. These describe only the probe, not translated game size or startup cost.
- Native runtime/Aurora and .NET translator suites were not built here. No supported PAL dump was supplied or used. No game boot, physics, audio, persistence or performance claim follows from this probe.

CI in `.github/workflows/web-probe.yml` installs the pinned Emscripten release, builds and runs the two WASM tests, performs software-WebGPU browser tests and allowlists exactly six game-free files for artifact upload. CI execution status is separate from local results; inspect the actual workflow run before claiming remote success. Test executables, inherited assets and generated game code are not part of that upload.

## Next work

Use the [audit](web-port-audit.md) and [architecture decision](web-architecture.md). Resolve checked guest-memory backing, floating-point helpers, cooperative contexts and browser Aurora integration in separately reviewed milestones. Do not replace the probe's native success log with a simulated game screenshot or report full Milestone 1 complete.
