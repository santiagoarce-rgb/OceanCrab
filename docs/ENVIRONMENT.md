# Configuration reference

Ocean Crab is configured in three places:

1. **CMake options** at configure time.
2. **`profiles/spongebob/config/SpongeBobNative.ini`** for display, rendering and timing.
3. **Environment variables** at run time, mostly for workarounds and diagnostics.

Boolean environment variables are enabled by being set to any value (`=1`).

## CMake options

| Option | Default | Effect |
|---|---|---|
| `PSPRECOMP_PROFILE` | *(empty)* | Profile to build. `spongebob` builds `SpongeBobNative`; empty builds only the framework. |
| `PSPRECOMP_BUILD_TESTS` | `ON` | Build the framework test suite (`psprecomp_tests`). |
| `PSPRECOMP_BUILD_PROFILE_TESTS` | `ON` | Build tests supplied by the selected profile. |
| `PSPRECOMP_GENERATED_OPT_LEVEL` | `2` | `-O` level for the generated AOT units (`0`–`3`). |
| `PSPRECOMP_NULL_JUMP_CHECK` | `ON` in Debug | Trap `jr`/`jalr` to address 0 in generated code with a full register dump. |
| `PSPRECOMP_LTO` | `OFF` | Link-time optimization. |
| `PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH` | `OFF` | Remove the per-store write-watch branch (production builds). |
| `PSPRECOMP_AOT_PRODUCTION_FASTPATHS` | `OFF` | Compile out chain diagnostics and counters (production builds). |
| `PSPRECOMP_NATIVE_AVX2`, `PSPRECOMP_HOST_AVX`, `PSPRECOMP_MSVC_*` | — | MSVC/Windows only. |

## Runtime: game and workarounds

| Variable | Effect |
|---|---|
| `PSPRECOMP_SKIP_BIK` | Skip the Bink videos and enable the related compatibility hooks (fake Movie object, missing menu background). Required until VFPU is supported. |
| `PSPRECOMP_AUDIO` | Enable audio output. |
| `PSPRECOMP_CONFIG=<path>` | Use a different `.ini` file. |
| `PSPRECOMP_SIMULATE_BUTTON=<mask>` | OR a PSP button bitmask into every pad read (headless testing). |
| `PSPRECOMP_MAX_DISPATCHES=<n>` | Stop after `n` dispatches (default 4,000,000,000). |
| `PSPRECOMP_SPIN_YIELD_TICKS=<n>` | Ticks on the same PC before the scheduler forces a yield (default 4). |

## Runtime: dispatch and debugging

| Variable | Effect |
|---|---|
| `PSPRECOMP_TRACE` | Log every dispatch. Very verbose. |
| `PSPRECOMP_TRACE_PC=<addr>` | Log registers each time `addr` is dispatched. Combine with `PSPRECOMP_NO_CHAIN` to catch chained calls. |
| `PSPRECOMP_TRACE_MEM` | Log memory accesses while tracing. |
| `PSPRECOMP_TRACE_ON_ERROR` | On a runtime error, print the most recent dispatches (PC, args, `sp`, `ra`). |
| `PSPRECOMP_STRICT_PC_PROGRESS` | Stop when the PC stops making progress. |
| `PSPRECOMP_PROGRESS_EVERY=<n>` | Print a progress line every `n` dispatches. |
| `PSPRECOMP_COUNT_PC=<a,b,...>` | Count how often each listed PC is dispatched. |
| `PSPRECOMP_REPORT_DISPATCH_COUNT` | Print the total dispatch count at exit. |
| `PSPRECOMP_PROFILE_DISPATCH`, `..._START`, `..._COUNT` | Profile a window of dispatches. |
| `PSPRECOMP_HLE_HISTOGRAM` | Print a histogram of HLE calls by NID. |
| `PSPRECOMP_CHAIN_DEPTH=<n>` | Limit direct chaining between generated units. |
| `PSPRECOMP_NO_CHAIN` | Disable direct chaining (every call goes through the dispatcher). |
| `PSPRECOMP_WATCH_WRITE=<addr>`, `PSPRECOMP_WATCH_WRITE_SIZE=<n>` | Report writes to a guest memory range. |
| `PSPRECOMP_FILE_OBJECT_DIAG`, `PSPRECOMP_FILE_OBJECT_STOP_ON_NULL` | File-object diagnostics. |
| `PSPRECOMP_REALTIME_SPEED_DIAG`, `PSPRECOMP_REALTIME_SPEED_INTERVAL` | Report emulated vs. real-time speed. |

> `PSPRECOMP_HEAP_DIAG`, `PSPRECOMP_REQUEST_ALLOC_DIAG` and the
> `PSPRECOMP_WORLD_STREAM_*` variables are inherited from the VCS profile and
> use VCS-specific addresses. They do nothing useful for SpongeBob yet.

## Runtime: video and GE

| Variable | Effect |
|---|---|
| `PSPRECOMP_DUMP_FRAME=<dir>` | Write selected presented frames to `<dir>/frame_N.ppm`. |
| `PSPRECOMP_GE_BACKEND` | Renderer backend (normally taken from the `.ini`). |
| `PSPRECOMP_INTERNAL_WIDTH`, `PSPRECOMP_INTERNAL_HEIGHT` | Internal render resolution (normally taken from the `.ini`). |
| `PSPRECOMP_GE_STRICT` | Treat unknown GE commands as errors. |
| `PSPRECOMP_GE_PHASE_DIAG` | Log GE phase timings. |
| `PSPRECOMP_GE_DIRECT_NONINDEXED_DRAW` | Draw non-indexed primitives directly. |
| `PSPRECOMP_GE_PARALLEL_VERTEX_DECODE`, `..._THRESHOLD`, `..._MAX_WORKERS` | Parallel vertex decoding. |
| `PSPRECOMP_GE_PARALLEL_TEXTURE_DECODE` | Parallel texture decoding. |
| `PSPRECOMP_GE_GPU_HW_TRANSFORM`, `..._HW_LIT`, `..._HW_CULL` | Move transform, lighting and culling to the GPU. |
| `PSPRECOMP_GE_GPU_STAGE_VERTICES` | Vertex staging on the GPU path. |
| `PSPRECOMP_GE_GPU_TEXTURE_CACHE_MB`, `..._TEXTURE_DECODE_LIMIT` | Texture cache size and per-frame decode budget. |
| `PSPRECOMP_GE_GPU_COLOR_PREVIEW`, `..._GEOMETRY_DEBUG_COLORS`, `..._FORCE_WHITE_VERTEX` | Visual debugging aids. |
| `PSPRECOMP_GE_GPU_DUMP_VBLANK` | Dump GPU state at a given vblank. |
| `PSPRECOMP_GE_GPU_SKIP_SOFTWARE_RASTER`, `..._SKIP_OWNED_RASTER`, `..._SKIP_DISPLAYED_RASTER` | Skip parts of the software rasterizer. |
| `PSPRECOMP_RASTER_THREADS`, `PSPRECOMP_RASTER_PARALLEL_PIXELS` | Software rasterizer threading. |
| `PSPRECOMP_DX12_*` | Inherited from the Windows/DX12 path; not used on Linux. |

## Runtime: audio

| Variable | Effect |
|---|---|
| `PSPRECOMP_AUDIO_DIAG`, `PSPRECOMP_SAS_DIAG` | Audio and SAS mixer diagnostics. |
| `PSPRECOMP_AUDIO_SUMMARY` | Periodic audio summary. |
| `PSPRECOMP_AUDIO_PREBUFFER_BLOCKS=<n>` | Blocks to prebuffer before playback. |
| `PSPRECOMP_AUDIO_WAV=<path>` | Record the audio output to a WAV file. |

## Runtime: host window

| Variable | Effect |
|---|---|
| `SPONGEBOB_WINDOW_SCALE=<n>` | Initial window scale. |
| `SPONGEBOB_VULKAN_VALIDATION` | Enable Vulkan validation layers. |
| `SPONGEBOB_HUD_DIAG` | HUD diagnostics. |
| `SPONGEBOB_GE_NO_DEFER` | Disable deferred GE list execution. |

To list every variable the code reads:

```bash
grep -rhoE 'getenv\("[A-Z0-9_]+"\)' src include tools profiles/spongebob/host | sort -u
```
