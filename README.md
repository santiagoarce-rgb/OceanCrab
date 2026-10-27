<div align="center">

# 🦀 Ocean Crab

### SpongeBob SquarePants: Truth or Square — native PC static recompilation (PSP)

*No emulator. No JIT. The PSP's MIPS code, translated to C++ ahead of time.*

![Platform: Linux](https://img.shields.io/badge/platform-Linux-informational?logo=linux&logoColor=white)
![Language: C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus&logoColor=white)
![Graphics: Vulkan](https://img.shields.io/badge/graphics-Vulkan-red?logo=vulkan&logoColor=white)
![License: MIT](https://img.shields.io/badge/license-MIT-green)
![Status: Main Menu](https://img.shields.io/badge/status-main%20menu%20reached-brightgreen)

</div>

---

Ocean Crab takes the PSP binary of **SpongeBob's Truth or Square** (`ULUS-10478`, decrypted by you) and translates its MIPS Allegrex machine code into native C++ **ahead of time**. The result is a standalone executable that runs the game's own code directly on your CPU, with a custom runtime standing in for the PSP's operating system and hardware.

> *Code! Code! Code!* — for cashy.

## 🏆 Status

> ### 🎉 Main Menu reached — first playable build!
>
> The game boots, relocates the EBOOT, runs its main loop and renders the **main menu** with working **audio** and **input**. Story Mode, Credits and Options are fully navigable.

### 🚧 Current blocker: "New Game"

Selecting **New Game** freezes the game. This is the active investigation. Two hypotheses:

1. **VFPU coverage.** The level's 3D engine likely needs PSP **VFPU** (vector FPU) instructions that the AOT codegen does not translate yet.
2. **Asset-loading synchronization.** A timing or ordering problem while streaming the level's assets.

Neither is confirmed yet, so this section will be updated as the cause is pinned down.

### 🎬 The "Skip Bik" workaround

The intro videos (`.bik`) use a Bink decoder that also depends on VFPU. Until VFPU is supported, setting `PSPRECOMP_SKIP_BIK=1` makes the HLE return `ENOENT` when the game tries to open those files, so the game skips them cleanly and goes straight to the menu.

## ✨ Features

| Area | Status | Details |
|---|:---:|---|
| **Boot** | ✅ | EBOOT loading and relocation, `module_start`, worker threads |
| **Rendering (3D + 2D)** | ✅ | Custom Vulkan backend (display lists → Vulkan), SDL2 window |
| **Audio** | ✅ | Background music and button sound effects, minimal WAV/SAS decoding |
| **Input** | ✅ | Keyboard and gamepads through SDL2 (tested with an 8BitDo pad) |
| **File I/O** | ✅ | `sceIoOpen`, `sceIoRead`, `sceIoLseek` streaming from your game files |
| **Threading** | ✅ | Virtual scheduler; three game threads (uid 2, 3, 4) switch without deadlocks |
| **Menus** | ✅ | Main menu, Story Mode, Credits and Options |
| **Gameplay / levels** | ⏳ | Blocked on "New Game" (see above) |
| **VFPU** | ⏳ | Not translated by the codegen yet |
| **Intro videos (Bink)** | ⏭️ | Skipped via `PSPRECOMP_SKIP_BIK=1` |

## 🚀 Getting Started

### Requirements

- CMake 3.20+
- A C++20 compiler (GCC 12+ or Clang 15+)
- Ninja
- Vulkan SDK / `vulkan-devel`
- SDL2 (`sdl2-compat` on Arch)
- `glslangValidator` (shaders are compiled to SPIR-V at build time)

**Arch Linux**

```bash
sudo pacman -S cmake ninja vulkan-devel glslang sdl2-compat spirv-tools
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential cmake ninja-build glslang-tools spirv-tools \
    libvulkan-dev vulkan-tools libsdl2-dev
```

### Build

You must provide your own legally obtained, **decrypted** `EBOOT.ELF`. Place it at:

```
profiles/spongebob/original/ULUS10478_EBOOT.ELF
```

Then generate the AOT corpus from it and build:

```bash
git clone https://github.com/santiagoarce-rgb/OceanCrab.git
cd OceanCrab

# 1. Generate the AOT corpus from your own EBOOT
./out/spongebob-config-test/psp_recomp \
    profiles/spongebob/original/ULUS10478_EBOOT.ELF \
    --auto profiles/spongebob/generated \
    0x08804000 0x20000

# 2. Configure and build
cmake -S . -B out/crab -DPSPRECOMP_PROFILE=spongebob -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/crab -j$(nproc)
```

> The `psp_recomp` tool is built as part of the project. If `out/spongebob-config-test/psp_recomp` does not exist yet, build the framework first.
>
> The generated corpus (about 56 MB, 20 units) is derived from the game binary, so it is **gitignored**. Every user generates it from their own copy.

### Run

```bash
PSPRECOMP_SKIP_BIK=1 PSPRECOMP_AUDIO=1 ./out/crab/profiles/spongebob/SpongeBobNative
```

| Variable | Purpose |
|---|---|
| `PSPRECOMP_SKIP_BIK=1` | Skip the `.bik` intro videos (the HLE returns `ENOENT`). Needed for now, since the Bink decoder requires VFPU. |
| `PSPRECOMP_AUDIO=1` | Enable audio output. |

## 🔧 Technical Highlights

### Static recompilation, not emulation

The codegen decodes the Allegrex binary, discovers **13,876 functions** (about 118,000 once the full corpus is generated), and emits C++ units that are compiled natively. A small runtime provides the PSP side: memory, threads, kernel services and hardware.

### Jump tables and function pointers

Indirect control flow was one of the hardest parts. The codegen now detects **jump tables** and **functions reached through pointers**, so indirect jumps and calls land in the right generated code. SpongeBob's units use a "full PC switch" strategy because their labels span more than 8,192 slots.

### I/O mapping

Game file access goes through an HLE layer (`sceIoOpen` / `sceIoRead` / `sceIoLseek`) that maps the PSP's paths to files on disk, enabling streaming of the game's assets.

### HLE for audio and controls

Audio runs through a minimal WAV/SAS path (music and sound effects), and PSP controller input is mapped to keyboard and SDL2 gamepads.

### Scheduler

A virtual thread scheduler with proper thread lifecycle handling (including `sceKernelWaitThreadEnd` and wake-ups on thread completion) keeps the game's threads switching without deadlocks.

### Upstream-worthy fixes

Bringing this game up exposed bugs in the base framework. These fixes are general-purpose and are intended to go upstream:

- **Codegen** (`tools/codegen_main.cpp`):
  - infinite loop on `JAL` → import stub (`continue` without advancing `pc`)
  - non-dense switch route missing `local_pc` / `entry_id` declarations
  - unit index out of range in `emit_target` and in cross-unit `JAL`
  - VFPU register out of range in `mtv` / `mfv`
  - an O(N²) string replacement in constant lowering, rewritten as O(N)
- **Runtime** (`include/psprecomp/runtime.hpp`): `invoke_chained_direct` did not set `ctx.pc` before calling the destination unit, which breaks the "full PC switch" strategy.

## 🏗️ Architecture

Ocean Crab has three layers:

1. **PSPRecomp framework**: MIPS Allegrex decoder, ELF/PRX loader, guest memory, runtime dispatch (reusable and game-neutral).
2. **Host layer**: Vulkan renderer, SDL2 window and input, SAS audio mixer (adapted from [lcs-recomp](https://github.com/elmasas/lcs-recomp)).
3. **SpongeBob profile**: game-specific HLE, display-list capture and bootstrap (`profiles/spongebob/`).

See `docs/ARCHITECTURE.md` for details.

## 🗺️ Roadmap

| Phase | Status | Description |
|---|:---:|---|
| A | ✅ Done | SysMem + ThreadMan + thread switching |
| B | ✅ Done | VBlank + timer HLE |
| C | ✅ Done | I/O (`sceIoOpen`, `sceIoRead`, `sceIoLseek`) |
| D | ✅ Done | Rendering: display lists → Vulkan, display, controls |
| E | ✅ Done | Audio (music, SFX, WAV/SAS) |
| F | ✅ Done | **Main menu rendered and navigable** 🎯 |
| G | ⏳ Next | **Gameplay / level loading (VFPU support in the codegen)** |
| H | 🔜 | Bink video decoding (needs VFPU) |
| I | 🔜 | Polish, performance, Windows/macOS builds |

## ⚖️ Legal

This repository **does not** contain any copyrighted game assets, ISOs, or game executables. You must provide your own legally obtained copy of the game and decrypt the EBOOT yourself. The generated AOT corpus is derived from the game binary and is never committed.

*SpongeBob SquarePants and all related properties are trademarks of their respective owners. This is an unofficial fan project for educational and preservation purposes.*

## 🙏 Credits

Ocean Crab stands on the shoulders of other open-source work. See [THIRD_PARTY.md](THIRD_PARTY.md) for the full list, including:

- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) — the recompilation framework
- [lcs-recomp](https://github.com/elmasas/lcs-recomp) — Vulkan + SDL2 + SAS host layer
- [Yakumo](https://github.com/TeamGDB/Yakumo) — reference for a PSP recompilation

## 🤝 Contributing

Issues and PRs are welcome, especially around **VFPU translation** in the codegen. The general-purpose fixes in `tools/codegen_main.cpp` and `include/psprecomp/runtime.hpp` should also be upstreamed to PSPRecomp.

## 📄 License

MIT. See [LICENSE](LICENSE).
