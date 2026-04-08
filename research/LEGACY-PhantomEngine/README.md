# Phantom Engine

Phantom Engine is a Windows x86_64 game engine written in C++23. It uses `Vulkan` or `DirectX` for graphics rendering and plays PVX video files.

## Features

- **Dual Renderer Support**: Vulkan and DirectX (D2D) backends
- **PVX Video Playback**: Custom video format with ZSTD compression
- **Menu System**: Animated video backgrounds with clickable areas
- **Raycaster Mode**: First-person maze exploration with lighting effects
- **XMI/MIDI Music**: Background music support

## Architecture

### Video Format (PVX)

Phantom Engine uses the PVX (Phantom Video eXtended) format for video content. PVX files are created using the companion `phantom-codec` tool.

### Game Flow

1. **Intro**: Plays `lakeshore.pvx` intro video
2. **Main Menu**: Displays `menu.pvx` as animated background with menu options
3. **Gameplay**: Raycaster-based exploration mode

### Configuration

Engine settings are stored in `config.json`:
- Renderer selection (Vulkan/DirectX)
- Window dimensions
- Audio settings

## Building

See `build.sh` for build instructions.

## Developers

### Libraries

- Vulkan SDK
- DirectX/Direct2D
- ZSTD (compression)
- nlohmann/json (configuration)
