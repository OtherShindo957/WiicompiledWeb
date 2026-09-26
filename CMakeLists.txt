cmake_minimum_required(VERSION 3.25)
project(WiiCompiledWeb LANGUAGES CXX)

option(WIICOMPILED_WEB "Build the game-free WASM/WebGPU platform probe" OFF)
if(NOT WIICOMPILED_WEB)
    message(FATAL_ERROR "For the browser probe set WIICOMPILED_WEB=ON. Desktop builds still use -S runtime.")
endif()
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The browser probe requires Emscripten. Configure using emcmake cmake.")
endif()

# Intentionally exclude native runtime, Aurora, generated code and all inherited assets.
add_executable(wiicompiled-web runtime/src/platform/web/probe.cpp)
target_compile_features(wiicompiled-web PRIVATE cxx_std_17)
target_compile_options(wiicompiled-web PRIVATE -Wall -Wextra -Werror)
target_link_options(wiicompiled-web PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web -sASYNCIFY=1
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=67108864
    -sFILESYSTEM=0 -sASSERTIONS=1)
set_target_properties(wiicompiled-web PROPERTIES
    SUFFIX ".js" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/site")
foreach(asset index.html launcher.js platform.js style.css)
    configure_file("web/${asset}" "${CMAKE_BINARY_DIR}/site/${asset}" COPYONLY)
endforeach()

option(WIICOMPILED_WEB_TESTS "Build selected existing game-free contracts as WASM tests" ON)
if(WIICOMPILED_WEB_TESTS)
    enable_testing()
    add_executable(web_input_expr_tests runtime/tests/test_expr.cpp runtime/src/input_expr.cpp)
    add_executable(web_sc_serial_tests runtime/tests/sc_serial_tests.cpp)
    foreach(test web_input_expr_tests web_sc_serial_tests)
        target_include_directories(${test} PRIVATE runtime/include)
        target_compile_features(${test} PRIVATE cxx_std_17)
        target_compile_options(${test} PRIVATE -fexceptions)
        target_link_options(${test} PRIVATE -fexceptions -sENVIRONMENT=node -sEXIT_RUNTIME=1)
        set_target_properties(${test} PROPERTIES SUFFIX ".cjs")
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
endif()
