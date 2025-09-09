# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

Project: Artemis Qt — a Qt 6 desktop client (C++/Qt Quick) for NVIDIA GameStream-compatible servers (Apollo/Sunshine), forked from Moonlight Qt, with additional Artemis features.

Key context sources: README.md, docs/BUILD_SYSTEM.md, docs/DEVELOPMENT.md, .github/workflows/dev-build.yml, artemis.pro, app/app.pro, scripts/*.


Commands and workflows

- Prerequisites
  - Qt 6.7+ (Qt 6.8+ recommended)
  - FFmpeg, SDL2, SDL2_ttf, OpenSSL, Opus
  - Initialize submodules before building:
    - git submodule update --init --recursive

- Build (macOS/Linux)
  - Release build (Qt 6):
    - qmake6 artemis.pro CONFIG+=release
    - make -j$(sysctl -n hw.ncpu)   # macOS
    - make -j$(nproc)               # Linux
  - Debug build:
    - qmake6 artemis.pro CONFIG+=debug
    - make -j$(sysctl -n hw.ncpu)   # macOS
    - make -j$(nproc)               # Linux
  - Notes
    - Prefer qmake6 (Qt 6). If only qmake is present, ensure it’s the Qt 6 variant.
    - Some older helper scripts reference moonlight-qt.pro; use artemis.pro for this repo.

- Build (Windows, from Dev Prompt)
  - qmake6 artemis.pro CONFIG+=release
  - nmake
  - Packaging and multi-arch builds are scripted (see scripts/build-artemis-arch.bat and scripts/generate-artemis-bundle.bat).

- Run the app
  - macOS: open app/Artemis.app
  - Linux: ./app/artemis
  - Windows: app\release\Artemis.exe (or debug\Artemis.exe for debug builds)

- Clean
  - make clean || true
  - rm -f .qmake.stash .qmake.cache; find . -name "Makefile*" -delete || true

- Packaging (platform-specific)
  - macOS (Universal DMG):
    - ./scripts/generate-dmg.sh Release [VERSION]
  - Linux (AppImage):
    - ./scripts/build-appimage.sh
  - Windows
    - Portable/arch-specific: scripts\build-artemis-arch.bat release
    - Universal installer (bundle): scripts\generate-artemis-bundle.bat release [VERSION]

- Single test / ad-hoc checks
  - There is no formal unit test harness configured. Use the included helper for OTP hashing parity:
    - python3 test_hash.py


High-level architecture and structure

- Build system
  - qmake aggregator project at artemis.pro orchestrates subprojects in SUBDIRS:
    - app — Qt Quick GUI and CLI entrypoints, Artemis feature managers, streaming integration
    - moonlight-common-c — upstream core streaming components
    - qmdnsengine — mDNS/DNS-SD service discovery
    - h264bitstream — codec bitstream utilities
    - soundio (desktop targets), AntiHooking (Windows) — platform-specific components
  - globaldefs.pri enforces debug_and_release and symbols; project targets Qt 6.

- Application layers (app/)
  - UI/UX: Qt Quick (QML compiled at build for macOS/Windows); GUI models expose data to QML
    - gui/ (e.g., computermodel.cpp, appmodel.cpp, sdlgamepadkeynavigation.cpp)
  - CLI tools: cli/ (pair, listapps, startstream, quitstream) built into the same binary via command-line parsing
  - Backend managers (Artemis features): backend/
    - ClipboardManager — Apollo clipboard sync endpoints
    - OTPPairingManager — OTP-based pairing (SHA-256 of pin + salt + passphrase)
    - ServerCommandManager — executes server commands with permission checks
    - QuickMenuManager, ServerPermissions, SystemProperties — client-side feature control and capability detection
  - Streaming pipeline: streaming/
    - Video: FFmpeg-based decoders/renderers with optional VAAPI/VDPAU/libplacebo paths
    - Audio: SDL-based renderers
    - Input: keyboard/mouse/gamepad abstraction, session lifecycle
  - Settings: settings/ with centralized ArtemisSettings and StreamingPreferences (QSettings-backed)
  - Integration points: NvHTTP/NvPairingManager/NvComputer extended to support Apollo features and capability flags

- External integration and packaging
  - Scripts under scripts/ provide platform packaging:
    - macOS DMG (universal), Linux AppImage/Flatpak, Windows MSI + WiX universal bundle
  - GitHub Actions dev-build.yml shows canonical CI steps, including Qt setup and cross-arch packaging; follow those steps for reproducible local builds if needed.

Notes for agents

- Always initialize submodules before building. Use artemis.pro as the project root; avoid older references to moonlight-qt.pro in legacy scripts.
- Use qmake6 for Qt 6. On macOS, the CI uses MACOSX_DEPLOYMENT_TARGET=14.0 during packaging; regular local builds typically don’t require setting this.
- No linting configuration is present in-repo; don’t assume clang-tidy/clang-format.
- For troubleshooting macOS MOC/QML build issues, the CI demonstrates a full clean + qmake_all sequence and per-subproject rebuilds in .github/workflows/dev-build.yml.

