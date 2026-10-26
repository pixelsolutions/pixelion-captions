cmake_minimum_required(VERSION 3.28...3.30)

include(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake"
    NO_POLICY_SCOPE
)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(
    PIXELION_BUILD_TESTS
    "Build the host-runnable pure-logic test target"
    OFF
)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
    target_link_libraries(
        ${CMAKE_PROJECT_NAME}
        PRIVATE Qt6::Core Qt6::Widgets Qt6::Network
    )
    set_target_properties(
        ${CMAKE_PROJECT_NAME}
        PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
endif()

set(PIXELION_COGNITO_CLIENT_ID
    "2e36gab6phga88jnn0ia2qd69s"
    CACHE STRING
    "Pixelion native Windows Cognito public client id"
)
set(PIXELION_COGNITO_REDIRECT_URI
    "http://localhost:19432/auth/callback"
    CACHE STRING
    "Pixelion native Windows OAuth callback"
)
set(PIXELION_API_BASE_URL
    "https://9yw5ciothk.execute-api.us-east-1.amazonaws.com/prod"
    CACHE STRING
    "Pixelion API base URL"
)

target_compile_definitions(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
        PIXELION_COGNITO_CLIENT_ID="${PIXELION_COGNITO_CLIENT_ID}"
        PIXELION_COGNITO_REDIRECT_URI="${PIXELION_COGNITO_REDIRECT_URI}"
        PIXELION_API_BASE_URL="${PIXELION_API_BASE_URL}"
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src src/shared)
target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
        src/plugin-main.cpp
        src/shared/plugin-debug-log.cpp
        src/shared/feedback-dialog.cpp
        resources/pixelion.qrc
)

if(WIN32)
    target_sources(
        ${CMAKE_PROJECT_NAME}
        PRIVATE
            src/platform/windows/auth-service.cpp
            src/platform/windows/live-session.cpp
            src/platform/windows/tls-backend.cpp
    )
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Advapi32 Winhttp)
elseif(APPLE)
    target_sources(
        ${CMAKE_PROJECT_NAME}
        PRIVATE
            src/platform/macos/tls-backend.mm
            src/platform/macos/auth-service.mm
            src/platform/macos/live-session.mm
    )
    target_link_libraries(
        ${CMAKE_PROJECT_NAME}
        PRIVATE
            "-framework Security"
            "-framework Foundation"
            "-framework CoreFoundation"
    )
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.7")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")

    # OBS bundles Qt6 but omits the Qt TLS backend plugins. Ship Qt's macOS TLS
    # backend dylib inside this plugin's bundle (beside the binary, in a "tls"
    # subdirectory) so QNetworkAccessManager (HTTPS) has a working TLS backend
    # with no user-installed dependency. (WSS uses NSURLSessionWebSocketTask and
    # the system TLS stack, since obs-deps Qt6 ships no Qt6::WebSockets.) registerBundled-
    # QtTlsBackend() in src/platform/macos/tls-backend.mm resolves the binary via
    # dladdr and registers this directory on Qt's library path when it exists.
    get_filename_component(PIXELION_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)
    set(PIXELION_QT_TLS_BACKEND "")
    foreach(
        _pixelion_tls_candidate
        "libqsecuretransportbackend.dylib"
        "libqopensslbackend.dylib"
    )
        if(
            NOT PIXELION_QT_TLS_BACKEND
            AND EXISTS "${PIXELION_QT_ROOT}/plugins/tls/${_pixelion_tls_candidate}"
        )
            set(PIXELION_QT_TLS_BACKEND
                "${PIXELION_QT_ROOT}/plugins/tls/${_pixelion_tls_candidate}"
            )
        endif()
    endforeach()
    if(PIXELION_QT_TLS_BACKEND)
        # A nested dylib must be signed with the bundle's identity or bundle
        # signing fails. Developer ID builds also need hardened runtime and a
        # secure timestamp (notarization); ad-hoc ("-") builds use neither.
        if(CODESIGN_IDENTITY AND NOT CODESIGN_IDENTITY STREQUAL "-")
            set(PIXELION_TLS_SIGN_ARGS --options runtime --timestamp)
        else()
            set(PIXELION_TLS_SIGN_ARGS --timestamp=none)
        endif()
        get_filename_component(
            PIXELION_QT_TLS_BACKEND_NAME
            "${PIXELION_QT_TLS_BACKEND}"
            NAME
        )
        add_custom_command(
            TARGET ${CMAKE_PROJECT_NAME}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/MacOS/tls"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                "${PIXELION_QT_TLS_BACKEND}"
                "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/MacOS/tls/${PIXELION_QT_TLS_BACKEND_NAME}"
            COMMAND
                codesign --force --sign "$<IF:$<BOOL:${CODESIGN_IDENTITY}>,${CODESIGN_IDENTITY},->"
                ${PIXELION_TLS_SIGN_ARGS}
                "$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/MacOS/tls/${PIXELION_QT_TLS_BACKEND_NAME}"
            VERBATIM
        )
    else()
        message(
            WARNING
            "Qt macOS TLS backend dylib not found under ${PIXELION_QT_ROOT}/plugins/tls; "
            "HTTPS/WSS may fail at runtime. Confirm the obs-deps Qt6 TLS backend (Task 2.1)."
        )
    endif()
else()
    message(
        FATAL_ERROR
        "Pixelion Captions supports Windows x64 and macOS (arm64/x86_64) only. The current target platform is not supported."
    )
endif()

# OBS bundles Qt6 but omits the Qt TLS backend plugins. Ship Qt's Windows
# SChannel backend beside this plugin so QNetworkAccessManager can use the
# Windows certificate store without requiring OpenSSL DLLs.
get_filename_component(PIXELION_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)
set(PIXELION_QSCHANNEL_BACKEND
    "${PIXELION_QT_ROOT}/plugins/tls/qschannelbackend.dll"
)
if(WIN32 AND EXISTS "${PIXELION_QSCHANNEL_BACKEND}")
    install(
        FILES "${PIXELION_QSCHANNEL_BACKEND}"
        DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit/tls"
    )
    add_custom_command(
        TARGET ${CMAKE_PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/tls"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${PIXELION_QSCHANNEL_BACKEND}"
            "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/tls/qschannelbackend.dll"
        VERBATIM
    )
endif()
install(FILES LICENSE README.md DESTINATION "${CMAKE_PROJECT_NAME}")
set_target_properties_plugin(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES OUTPUT_NAME ${_name}
)

# Host-runnable pure-logic tests (Task 8). Opt-in; standalone subproject that
# links no OBS/Qt, so it does not affect the plugin build or packaging.
if(PIXELION_BUILD_TESTS)
    add_subdirectory(tests)
endif()
