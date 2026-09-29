# 🦀 Ocean Crab

A native PC static recompilation of **SpongeBob's Truth or Square** for the PlayStation Portable (`ULUS-10478`).

Ocean Crab takes the original PSP binary (decrypted by the user) and translates the MIPS Allegrex machine code into native C++ ahead-of-time, producing a standalone executable for Linux, Windows, and macOS — no emulator required.

> *Money! Money! Money!* — but for code.

## Status

> **Active development.** The game boots, relocates the EBOOT (116,565 relocs), runs `module_start`, spawns the worker thread, switches threads correctly, and executes **10 million dispatches of real game code without a single unsupported instruction**. VBlank/timer scheduling, SysMem, ThreadMan, FPL, and the `madd`/`maddu`/`msub`/`msubu` multiply-accumulate instructions are all working. The runtime currently stops at its dispatch limit (`0x08971930`) — a safety cap, not a crash. Next milestones: IoFileMgr (asset loading), sceGe_user (display lists → Vulkan), and first frame rendering.

### Progress

- [x] EBOOT analysis (13,876 functions, 176 imports)
- [x] Vulkan backend integrated (from [lcs-recomp](https://github.com/elmasas/lcs-recomp), MIT)
- [x] SDL2 platform (window, input, audio)
- [x] SAS audio implementation (sceSasCore, 27 imports)
- [x] CMake Linux build system
- [x] Profile skeleton (176 imports registered)
- [x] NID database completion (187 entries)
- [x] AOT corpus generation (20 units, 56 MB, 118,530 functions)
- [x] Main runtime + EBOOT loader (relocation + module info + entry call)
- [x] HLE: SysMem (partition allocator)
- [x] HLE: ThreadMan (thread switch, context save/restore)
- [x] HLE: FPL (CreateFpl / TryAllocateFpl / FreeFpl / DeleteFpl)
- [x] HLE: sceDisplay (VBlank / SetFrameBuf / SetMode)
- [x] Kernel-syscall handling (`jal 0x00000000` → thread return)
- [x] Instruction: `madd` / `maddu` / `msub` / `msubu`
- [x] VBlank + timer scheduling (starvation hook, virtual time)
- [x] Game runs 10M dispatches without errors
- [ ] Configurable dispatch limit + busy-wait detection
- [ ] HLE: IoFileMgr + ModuleMgr (Fase C — asset loading)
- [ ] HLE: sceGe_user (display lists → Vulkan) (Fase D)
- [ ] HLE: sceCtrl (input) (Fase D)
- [ ] HLE: sceAudio (PCM output) (Fase E)
- [ ] **First frame rendered** 🎯

## Technical Highlights

### Boot sequence (working)

The game currently reaches this flow:

```
EBOOT.ELF load → relocation (116,565 relocs, 0 invalid)
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
jal 0x00000000 (thread return syscall) → register_function(0, ...)
   ↓
Thread switch → worker thread runs
   ↓
Worker: CreateFpl (7 MB heap), TryAllocateFpl, madd-heavy math
   ↓
... 10M dispatches of real game code
   ↓
Runtime stopped: Dispatch limit reached at 0x08971930
```

### Codegen fixes (contributed upstream)

Six bugs were discovered and fixed in `tools/codegen_main.cpp` while building the SpongeBob corpus. They don't trigger with the reference VCS corpus because it uses a different unit strategy:

1. **Infinite loop on `JAL → import-stub`**: `continue` without advancing `pc` → 22 GB of RAM in 22 min
2. **Non-dense switch route**: missing `local_pc` / `entry_id` declarations
3. **`emit_target` unit index out of range**: for targets outside the executable range
4. **Same bug in cross-unit `JAL`** (`direct_unit`)
5. **VFPU register out of range** in `mtv` / `mfv`: `d.word & 0xFF` → `d.word & 0x7F`
6. **Missing `madd` / `maddu` / `msub` / `msubu`** (`SPECIAL` funct 0x1C–0x1F): now lowered to 64-bit `acc = (hi << 32) | lo`

Additionally, an O(N²) string replacement in the constant lowering pass was rewritten to O(N).

### Framework fix (runtime.hpp)

`invoke_chained_direct` didn't set `ctx.pc` before calling the destination unit. SpongeBob's generated units use the "full PC switch" strategy (`switch(local_pc)` with `entry_id = 0`) because its labels span more than 8192 slots — unlike VCS's dense table. The fast path assumed `direct_entry_id` was set, leaving `ctx.pc` stale. Fix:

```cpp
if constexpr (DirectTargetPc != 0u) ctx.pc = DirectTargetPc;
```

### Thread scheduler

The profile implements a full PSP-style thread system:

- **ThreadTable** with `context`, `state` (Ready / Running / Waiting / Completed), `wake_at_us`
- **`activate_next_thread`**: FIFO scheduling; if idle, advances `virtual_time_us` to the earliest deadline (mirrors VCS's pattern)
- **`suspend_for_wakeup`**: saves the suspended context (`pc = gpr[31]`, `v0 = 0`) before switching — without this, threads restart from entry on wake
- **`sceKernelExitThread`**: completes the current thread and switches (registered as a `jal 0x00000000` target)
- **`sceKernelDelayThread` / `sceDisplayWaitVblankStart`**: suspend with `wake_at_us`, resumed by the starvation hook

### HLE subsystems implemented

| Library | Functions | Status |
|---|---|---|
| `SysMemUserForUser` | 8 | ✅ Partition allocator |
| `ThreadManForUser` | 10 | ✅ ThreadTable + scheduling |
| `sceDisplay` | 8 | ✅ VBlank / frame buffer / mode |
| FPL | 5 | ✅ Fixed pool allocator |
| `ModuleMgrForUser` | 3 | 🟡 Stubs (return UID) |

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

# 1. Generate the AOT corpus from your legally obtained EBOOT
#    (place it first at profiles/spongebob/original/ULUS10478_EBOOT.ELF)
cmake -S . -B out/framework -DPSPRECOMP_PROFILE="" -G Ninja
cmake --build out/framework --target psp_recomp -j$(nproc)

mkdir -p profiles/spongebob/generated
./out/framework/psp_recomp \
    profiles/spongebob/original/ULUS10478_EBOOT.ELF \
    --auto profiles/spongebob/generated \
    0x08804000 0x20000

# 2. Configure and build SpongeBobNative
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
# Default dispatch limit
./out/crab/profiles/spongebob/SpongeBobNative

# Or with a higher limit (env variable, like VCS)
PSPRECOMP_MAX_DISPATCHES=100000000 \
    ./out/crab/profiles/spongebob/SpongeBobNative
```

Current output:

```
[window] shown 960x544 renderer=software
SpongeBobNative PSP bootstrap
Executable: .../ULUS10478_EBOOT.ELF
Entry:      0x08804124
Relocs:     116565 (invalid 0, unsupported 0)
Functions:  118531
Config:     .../SpongeBobNative.ini (loaded)
Runtime stopped: Dispatch limit reached at 0x08971930
```

## Architecture

Ocean Crab is built on three layers:

1. **PSPRecomp framework** — MIPS Allegrex decoder, ELF/PRX loader, guest memory, runtime dispatch (reusable, game-neutral)
2. **Host layer** — Vulkan renderer, SDL2 window/input, SAS audio mixer (adapted from [lcs-recomp](https://github.com/elmasas/lcs-recomp))
3. **SpongeBob profile** — game-specific HLE, thread scheduler, display-list capture, bootstrap (`profiles/spongebob/`)

See `docs/ARCHITECTURE.md` for details.

## Roadmap

| Phase | Status | Description |
|-------|--------|-------------|
| A | ✅ Done | SysMem + ThreadMan + thread switch |
| B | ✅ Done | VBlank + timer HLE + FPL + `madd` family |
| B.5 | 🟡 In progress | Configurable dispatch limit + busy-wait detection |
| C | ⏳ Next | IoFileMgr + ModuleMgr (asset loading from disc) |
| D | ⏳ | sceGe_user (display lists → Vulkan) + sceCtrl |
| E | ⏳ | sceAudio + sceSasCore (PCM playback) |
| F | 🎯 | **First frame rendered** |

## Contributing

The framework fixes in `tools/codegen_main.cpp` and `include/psprecomp/runtime.hpp` are general-purpose and apply to any PSP recompilation target that uses the "full PC switch" strategy. They should be upstreamed to [jessicanataliagta/PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) — PRs welcome.

Known latent bugs in the reference profiles (not triggered by their current corpora, but present in the code):

- `profiles/vcs/tools/vcs_codegen_main.cpp:1133`: same `continue` → `break` bug
- `profiles/vcs/tools/vcs_codegen_main.cpp`: same `mtv`/`mfv` VFPU mask bug

## Legal

This repository **does not** contain any copyrighted game assets, ISOs, or game executables. Users must provide their own legally obtained copy of the game.

## Credits

See [THIRD_PARTY.md](THIRD_PARTY.md).

## License

MIT — see [LICENSE](LICENSE).
