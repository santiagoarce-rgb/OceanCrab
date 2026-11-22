# Third-party notices

Ocean Crab is distributed under the [MIT License](LICENSE). It builds on the
open-source projects below. Their copyright notices are preserved in
[`LICENSE`](LICENSE) and listed here with what each one contributes.

| Project | License | Used for | Where |
|---|---|---|---|
| [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) | MIT — © 2026 PSPRecomp contributors | Recompilation framework: Allegrex decoder, ELF/PRX loader, guest memory, runtime dispatch, analyzer and code generator | `include/psprecomp/`, `src/`, `tools/`, `tests/` |
| [lcs-recomp](https://github.com/elmasas/lcs-recomp) | MIT — © 2026 PSPRecomp contributors / elmasas | Host layer: Vulkan GE renderer, SDL2 window and input, SAS audio mixer, render configuration | `profiles/spongebob/host/` |
| [Yakumo](https://github.com/TeamGDB/Yakumo) | MIT — © 2026 PSPRecomp contributors, Yakumo contributors | Technical reference for PSP static recompilation (no code copied) | — |

## Build-time dependencies

These are linked or invoked at build time and are not vendored in this repository:

| Dependency | License | Purpose |
|---|---|---|
| [SDL2](https://www.libsdl.org/) | zlib | Window, input, audio device |
| [Vulkan loader / headers](https://www.vulkan.org/) | Apache-2.0 | Graphics API |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others | Compiles the GE shaders to SPIR-V at build time |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | F1 enhancements menu (fetched at configure time, pinned to v1.91.9b) |

## Reference material

Public PSP hardware documentation and observable program behavior are used as
technical references. Source code from projects under incompatible licenses
(for example GPL emulators) is **not** copied into this repository; see
[`docs/SOURCE_PROVENANCE.md`](docs/SOURCE_PROVENANCE.md).

## Game content

No game code, assets or executables are included. *SpongeBob SquarePants* and
related properties are trademarks of their respective owners. Ocean Crab is an
unofficial fan project and is not affiliated with or endorsed by them.
