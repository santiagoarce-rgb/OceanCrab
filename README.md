# 🦀 Ocean Crab

A native PC static recompilation of **SpongeBob's Truth or Square** for the PlayStation Portable (`ULUS-10478`).

Ocean Crab takes the original PSP binary (decrypted by the user) and translates the MIPS Allegrex machine code into native C++ ahead-of-time, producing a standalone executable for Linux, Windows, and macOS — no emulator required.

> *Money! Money! Money!* — but for code.

## Status

> **Active development.** Core infrastructure is complete; HLE implementation and first boot are in progress.

## Features (target)

- Native Vulkan renderer via the PSP GE (Graphics Engine) display-list emulation
- Software SAS audio (sceSasCore) with WAV/PCM playback
- SDL2 window, input, and audio output
- Cross-platform: Linux (primary), Windows, macOS

## Requirements

- CMake 3.20+
- C++20 compiler (GCC 12+, Clang 15+)
- Vulkan SDK / `vulkan-devel`
- SDL2 (`sdl2-compat` on Arch)
- `glslangValidator` (for shader → SPIR-V compilation)

### Arch Linux

    sudo pacman -S cmake ninja vulkan-devel glslang sdl2-compat spirv-tools

### Debian / Ubuntu

    sudo apt install build-essential cmake ninja-build glslang-tools spirv-tools libvulkan-dev vulkan-tools libsdl2-dev

## Building

    git clone https://github.com/santiagoarce-rgb/OceanCrab.git
    cd OceanCrab
    cmake -S . -B out/crab -DPSPRECOMP_PROFILE=spongebob -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build out/crab -j$(nproc)

## Running

Place your legally obtained decrypted `EBOOT.ELF` in:

    profiles/spongebob/original/ULUS10478_EBOOT.ELF

Then run:

    ./out/crab/profiles/spongebob/SpongeBobNative

## Progress

- [x] EBOOT analysis (13,876 functions, 176 imports)
- [x] Vulkan backend integrated (from lcs-recomp, MIT)
- [x] SDL2 platform (window, input, audio)
- [x] SAS audio implementation
- [x] CMake Linux build system
- [x] Profile skeleton (176 imports registered)
- [ ] NID database completion
- [ ] AOT corpus generation
- [ ] Main runtime + EBOOT loader
- [ ] HLE implementation (IoFileMgr, ThreadMan, sceDisplay, sceCtrl)
- [ ] **First frame rendered** 🎯

## Architecture

Ocean Crab is built on three layers:

1. **PSPRecomp framework** — MIPS Allegrex decoder, ELF/PRX loader, guest memory, runtime dispatch
2. **Host layer** — Vulkan renderer, SDL window/input, SAS audio mixer
3. **SpongeBob profile** — game-specific HLE, display-list capture, bootstrap

See `docs/ARCHITECTURE.md` for details.

## Legal

This repository **does not** contain any copyrighted game assets, ISOs, or game executables. Users must provide their own legally obtained copy of the game.

## Credits

See [THIRD_PARTY.md](THIRD_PARTY.md).

## License

MIT — see [LICENSE](LICENSE).
