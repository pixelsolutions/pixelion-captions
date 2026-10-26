# Feature: macos-platform-support
#
# Host-runnable test target (Task 8). Standalone: no OBS, Qt, or network. Built
# only when PIXELION_BUILD_TESTS is ON so the plugin build is unaffected.

cmake_minimum_required(VERSION 3.20)

project(pixelion-captions-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pixelion-captions-tests test-main.cpp)
target_include_directories(
    pixelion-captions-tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

enable_testing()
add_test(NAME pixelion-captions-tests COMMAND pixelion-captions-tests)
