# 🦀 Ocean Crab

A native PC static recompilation of **SpongeBob's Truth or Square** for the PlayStation Portable (`ULUS-10478`).

Ocean Crab takes the original PSP binary (decrypted by the user) and translates the MIPS Allegrex machine code into native C++ ahead-of-time, producing a standalone executable for Linux, Windows, and macOS — no emulator required.

> *Money! Money! Money!* — but for code.

## Status

> **Active development.** The game boots, relocates the EBOOT, runs `module_start`, spawns the worker thread, and executes game code. It stops cleanly at "all PSP threads completed" pending VBlank/timer HLE. First frame rendering is the next milestone.

### Progress

- [x] EBOOT analysis (13,876 functions, 176 imports)
- [x] Vulkan backend integrated (from lcs-recomp, MIT)
- [x] SDL2 platform (window, input, audio)
- [x] SAS audio implementation
- [x] CMake Linux build system
- [x] Profile skeleton (176 imports registered)
- [x] NID database completion (187 entries)
- [x] AOT corpus generation (20 units, 56 MB, 118,530 functions)
- [x] Main runtime + EBOOT loader (relocation + module info + entry call)
- [x] HLE: SysMem + ThreadMan (thread switch working)
- [x] Kernel-syscall handling (`jal 0x00000000` → thread return)
- [ ] HLE: VBlank + timer (Fase B)
- [ ] HLE: IoFileMgr + ModuleMgr (Fase C)
- [ ] HLE: sceGe_user + sceDisplay + sceCtrl (Fase D)
- [ ] HLE: sceSasCore + sceAudio (Fase E)
- [ ] **First frame rendered** 🎯

## Technical Highlights

### Boot sequence (working)

The game currently reaches this flow:

```
EBOOT.ELF load → relocation (116,565 relocs)
   ↓
entry 0x08804124 (module_start)
   ↓
SysMem: AllocPartitionMemory, GetBlockHeadAddr
   ↓
sceKernelSetCompilerVersion, GetSystemTimeLow, GetThreadId
   ↓
sceKernelCreateThread (worker thread @ 0x08804238)
   ↓
sceKernelStartThread
   ↓
sceKernelLoadModule (import stub)
   ↓
jal 0x00000000 (thread return syscall)
   ↓
Thread switch → worker thread runs
   ↓
Worker: CreateCallback, ... → ExitThread
   ↓
Runtime stopped: "all PSP threads completed"
```

### Codegen fixes (contributed upstream)

Five bugs were discovered in `tools/codegen_main.cpp` while building the SpongeBob corpus (they don't trigger with the reference VCS corpus because it uses a different unit strategy):

1. **Infinite loop** on `JAL → import-stub`: `continue` without advancing `pc` → 22 GB RAM in 22 min
2. **Non-dense switch route** missing `local_pc` / `entry_id` declarations
3. **`emit_target` unit index out of range** for targets outside executable range
4. **Same bug in cross-unit `JAL`** (direct_unit)
5. **VFPU register out of range** in `mtv` / `mfv` (`d.word & 0xFF` → `d.word & 0x7F`)

Additionally, an O(N²) string replacement in the constant lowering pass was rewritten to O(N).

### Framework fix (runtime.hpp)

`invoke_chained_direct` didn't set `ctx.pc` before calling the destination unit. SpongeBob's generated units use the "full PC switch" strategy (`switch(local_pc)` with `entry_id = 0`) because its labels span more than 8192 slots — unlike VCS's dense table. The fast path assumed `direct_entry_id` was set, leaving `ctx.pc` stale. Fix:

```cpp
if constexpr (DirectTargetPc != 0u) ctx.pc = DirectTargetPc;
```

## Requirements

- CMake 3.20+
- C++20 compiler (GCC 12+, Clang 15+)
- Vulkan SDK / `vulkan-devel`
- SDL2 (`sdl2-compat` on Arch)
- `glslangValidator` (for shader → SPIR-V compilation)

### Arch Linux

```bash
sudo pacman -S cmake ninja vulkan-devel glslang sdl2-compat spirv-tools
```

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake ninja-build glslang-tools spirv-tools \
    libvulkan-dev vulkan-tools libsdl2-dev
```

## Building

```bash
git clone https://github.com/santiagoarce-rgb/OceanCrab.git
cd OceanCrab

# Generate AOT corpus from your legally obtained EBOOT (must be placed at
# profiles/spongebob/original/ULUS10478_EBOOT.ELF first)
./out/spongebob-config-test/psp_recomp \
    profiles/spongebob/original/ULUS10478_EBOOT.ELF \
    --auto profiles/spongebob/generated \
    0x08804000 0x20000

# Configure and build
cmake -S . -B out/crab -DPSPRECOMP_PROFILE=spongebob -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/crab -j$(nproc)
```

The AOT corpus is ~56 MB (20 units, ~2.3–3.5 MB each) and is gitignored — each user generates it from their own copy of the game.

## Running

Place your legally obtained decrypted `EBOOT.ELF` in:

```
profiles/spongebob/original/ULUS10478_EBOOT.ELF
```

Then run:

```bash
./out/crab/profiles/spongebob/SpongeBobNative
```

Current output:

```
[window] shown 960x544 renderer=software
SpongeBobNative PSP bootstrap
Executable: .../ULUS10478_EBOOT.ELF
Entry:      0x08804124
Relocs:     116565 (invalid 0, unsupported 0)
Functions:  118530
Config:     .../SpongeBobNative.ini (loaded)
Runtime stopped: all PSP threads completed
```

## Architecture

Ocean Crab is built on three layers:

1. **PSPRecomp framework** — MIPS Allegrex decoder, ELF/PRX loader, guest memory, runtime dispatch (reusable, game-neutral)
2. **Host layer** — Vulkan renderer, SDL2 window/input, SAS audio mixer (adapted from [lcs-recomp](https://github.com/elmasas/lcs-recomp))
3. **SpongeBob profile** — game-specific HLE, display-list capture, bootstrap (`profiles/spongebob/`)

See `docs/ARCHITECTURE.md` for details.

## Roadmap

| Phase | Status | Description |
|-------|--------|-------------|
| A | ✅ Done | SysMem + ThreadMan + thread switch |
| B | ⏳ Next | VBlank + timer HLE (so `sceDisplayWaitVblankStart` callbacks fire) |
| C | ⏳ | IoFileMgr + ModuleMgr (asset loading from disc) |
| D | ⏳ | sceGe_user (display lists → Vulkan) + sceDisplay + sceCtrl |
| E | ⏳ | sceSasCore + sceAudio (WAV/PCM audio output) |
| F | 🎯 | **First frame rendered** |

## Contributing

The framework fixes in `tools/codegen_main.cpp` and `include/psprecomp/runtime.hpp` are general-purpose and apply to any PSP recompilation target that uses the "full PC switch" strategy. They should be upstreamed to [jessicanataliagta/PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) — PRs welcome.

The same `continue` → `break` bug exists in `profiles/vcs/tools/vcs_codegen_main.cpp:1133` (latent, not triggered by VCS's current corpus).

## Legal

This repository **does not** contain any copyrighted game assets, ISOs, or game executables. Users must provide their own legally obtained copy of the game.

## Credits

See [THIRD_PARTY.md](THIRD_PARTY.md).

## License

MIT — see [LICENSE](LICENSE).
