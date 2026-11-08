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

# Guest access + add-on credits tests run against the real shared headers, so
# they need QtCore/QtNetwork. Built only when Qt6 is available.
find_package(Qt6 QUIET COMPONENTS Core Network)
if(Qt6_FOUND)
    add_executable(pixelion-captions-guest-tests guest-addon-tests.cpp)
    target_include_directories(
        pixelion-captions-guest-tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/shared"
    )
    target_compile_definitions(
        pixelion-captions-guest-tests
        PRIVATE PIXELION_API_BASE_URL="https://api.pixelion.ai"
    )
    target_link_libraries(
        pixelion-captions-guest-tests
        PRIVATE Qt6::Core Qt6::Network
    )
    add_test(
        NAME pixelion-captions-guest-tests
        COMMAND pixelion-captions-guest-tests
    )
endif()
