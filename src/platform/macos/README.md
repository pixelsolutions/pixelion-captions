# macOS platform implementation

This directory holds the macOS platform adapters for Pixelion Captions: the TLS backend (`tls-backend.mm`), the auth service (`auth-service.mm`), and the live session (`live-session.mm`). These are built by CMake for macOS 13.7 (Ventura) and later as a universal binary (arm64 and x86_64), against the OBS Studio 31.1.1 dependencies. Pixelion Captions supports Windows x64 and macOS only.
