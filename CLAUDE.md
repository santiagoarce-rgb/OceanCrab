# OceanCrab — guía para Claude

Recompilación estática (AOT) de **SpongeBob: Truth or Square** (PSP, `ULUS10478`)
sobre el framework **PSPRecomp**. El código MIPS Allegrex del EBOOT se traduce a
C++ antes de compilar; un runtime propio emula el SO/hardware de la PSP (HLE).

## Reglas de trabajo

- **Commits chicos**: un cambio lógico por commit, estilo Conventional Commits
  como el historial (`fix(io): ...`, `feat(ctrl): ...`, `docs: ...`).
- **No subir `profiles/spongebob/generated/`**: es el corpus AOT derivado del
  ejecutable del juego (copyright). Ya está en `.gitignore`; la única excepción
  rastreada es `generated/generated_units.hpp`. Tampoco subir `profiles/*/original/`
  (EBOOT y assets), `dumps/` ni `out/`.
- **Explicar antes de tocar el codegen** (`tools/codegen_main.cpp`,
  `src/program_analysis.cpp`, `src/decoder.cpp`): describir el cambio propuesto y
  por qué, y esperar visto bueno. Un cambio ahí obliga a regenerar el corpus y
  recompilar las 20 units (~1 h).
- Preferir arreglos en el perfil (`profiles/spongebob/host/`, HLE/hooks) sobre
  cambios en el framework, salvo que el bug sea genérico (esos son candidatos a
  upstream).
- El usuario usa **fish**; los comandos que ejecute Claude pueden ser bash.
- `NOTES.md` es la bitácora de sesiones (en español): leerla para contexto
  histórico y añadir hallazgos relevantes ahí.

## Compilar

```bash
cmake -S . -B out/crab -DPSPRECOMP_PROFILE=spongebob -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/crab -j$(nproc)
```

Requiere CMake 3.20+, C++20, Ninja, Vulkan, SDL2 y `glslangValidator`.

### Regenerar el corpus AOT (solo si cambió el codegen/análisis)

Necesita `profiles/spongebob/original/ULUS10478_EBOOT.ELF` (descifrado, del usuario):

```bash
./out/crab/psp_recomp profiles/spongebob/original/ULUS10478_EBOOT.ELF \
    --auto profiles/spongebob/generated 0x08804000 0x20000
```

(El README usa `out/spongebob-config-test/psp_recomp`; cualquier build de
`psp_recomp` sirve.) Si `generated/` está vacío, el perfil compila con
`SPONGEBOB_NO_GENERATED`.

## Correr

```bash
PSPRECOMP_SKIP_BIK=1 PSPRECOMP_AUDIO=1 ./out/crab/profiles/spongebob/SpongeBobNative
```

Los assets se leen de `profiles/spongebob/original/PSP_GAME/USRDIR/`.
Config de host: `profiles/spongebob/config/SpongeBobNative.ini`.

## Fases del proyecto

El roadmap completo está en el `README.md` (sección *Roadmap*). En resumen, son
tres etapas grandes:

1. **Recompilar bien (Corrección)** — 🚧 *actual*. El juego tiene que correr
   con código traducido fiel, sin hacks: VFPU completo en el codegen y quitar
   los workarounds de video/alloc.
2. **Entender el código** — 🔜. Nombrar funciones y estructuras (player,
   actores, cámara, cargador de niveles) en Ghidra y pasarlas a overrides
   nativos en C++ (`register_function`).
3. **Extras** — 🔜/🔭. Menú de opciones, widescreen, alta resolución, 60 FPS,
   remapeo, mods y **co-op online** (jugadores remotos como "puppets"), luego
   Windows/macOS.

Al proponer arreglos: preferir la causa raíz y documentar las estructuras del
juego. Los hooks de `PSPRECOMP_SKIP_BIK` son **deuda técnica** y hay que
marcarlos así.

## Estado actual (2026-11-20)

- **El juego se puede terminar de principio a fin** (verificado por el usuario el
  2026-11-20: niveles, jefe final, créditos y minijuegos), sin `PSPRECOMP_SKIP_BIK`.
  Hub = casa de Bob Esponja.
- Bugs de render abiertos: los láseres de seguridad del museo (nivel 3) salen sin
  textura (**dificulta avanzar → prioridad**); el agua y los arcoíris se ven raros;
  falta un modelo de objeto en el nivel 1. Velocidad: el pacing a reloj real está en `pace_virtual_time_to_wall_clock()`.
- Heap del motor: `CPSPSystem::InitializeHeap` usa `sceKernelMaxFreeMemSize() - 1 MiB`.
  La memoria libre se calcula de verdad (arena después del módulo, `0x08B70000`,
  hasta los stacks). `[0x08B61000, 0x08B70000)` es scratch del host.
- Abierto: los videos Bink no tienen canal azul y no tienen audio (el "Bink audio
  thread" termina apenas arranca).
- Bink corre nativo. Aún quedan ~1 950 sitios `vfpu*/cop2 not lowered` en el corpus; no se ejecutan
  en las intros, pero hay que auditarlos (¿código real o datos?).
- Los hooks de `register_function` **deben** poner `ctx.pc = ctx.gpr[31]` al
  regresar; si no, el dispatcher los vuelve a llamar en loop.
- `PSPRECOMP_SKIP_BIK=1` (deuda, ya no hace falta): header Bink falso,
  `sub_08943694` → objeto Movie falso, `sub_08943060` → 0 (falta `menu_bkg.pvs`).
  El Memory Stick falso sí se usa siempre.
- `PSPRECOMP_NULL_JUMP_CHECK` (commit `cf37156`) atrapa `jr`/`jalr` a 0 y vuelca
  los registros.

## Estructura

| Ruta | Qué es |
|---|---|
| `include/psprecomp/`, `src/` | Framework: decoder, ELF, memoria guest, runtime/dispatch, análisis |
| `tools/codegen_main.cpp` | Generador AOT (`psp_recomp`) |
| `tools/analyzer_main.cpp`, `tools/dump_function.cpp` | Análisis y volcado de funciones |
| `tools/ghidra/` | Script de Ghidra para exportar funciones |
| `profiles/spongebob/host/` | Perfil: HLE (`spongebob_profile.cpp`), Vulkan GE, SDL, audio SAS, `main.cpp` |
| `profiles/spongebob/analysis/` | Reportes de funciones/seeds/imports del EBOOT |
| `profiles/spongebob/generated/` | Corpus AOT (ignorado por git) |
| `tests/test_main.cpp` | Tests del framework (`psprecomp_tests`, vía ctest) |
| `profiles/vcs/` | Perfil heredado de VCS (ignorado; no es parte de OceanCrab) |

## Variables `PSPRECOMP_*`

### Opciones de CMake

| Opción | Efecto |
|---|---|
| `PSPRECOMP_PROFILE` | Perfil a compilar (`spongebob`) |
| `PSPRECOMP_NULL_JUMP_CHECK` | Atrapa `jr`/`jalr` a 0 en código generado (ON por defecto en Debug) |
| `PSPRECOMP_GENERATED_OPT_LEVEL` | Nivel `-O` de las units generadas (0–3, default 2) |
| `PSPRECOMP_BUILD_TESTS` / `PSPRECOMP_BUILD_PROFILE_TESTS` | Tests del framework / del perfil |
| `PSPRECOMP_LTO`, `PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH`, `PSPRECOMP_AOT_PRODUCTION_FASTPATHS` | Optimizaciones de producción |
| `PSPRECOMP_NATIVE_AVX2`, `PSPRECOMP_HOST_AVX`, `PSPRECOMP_MSVC_*` | Solo MSVC/Windows |

### Variables de entorno (runtime)

**Juego / workarounds**
- `PSPRECOMP_SKIP_BIK` — salta los videos Bink y activa los hooks de Movie/menú.
- `PSPRECOMP_AUDIO` — activa salida de audio.
- `PSPRECOMP_CONFIG` — ruta alternativa al `.ini`.
- `PSPRECOMP_SIMULATE_BUTTON=<mask>` — OR de botones PSP en cada lectura de pad (pruebas headless).
- `PSPRECOMP_INPUT_SCRIPT=<first-last=mask,...>` — botones por rango de frames (flancos para menús).
  Llegar a gameplay en headless: `1300-1305=0x8,1500-1505=0x4000,1700-1705=0x4000,1900-1905=0x4000,2100-2105=0x4000`.
- `PSPRECOMP_NO_FRAME_PACING` — sin límite a tiempo real (**usar en pruebas headless**, van ~1.7× más rápido);
  `PSPRECOMP_PACE_DIAG` — imprime la razón tiempo virtual / reloj real.
- `PSPRECOMP_GE_UNSUPPORTED_DIAG` — trabajo del GE que el renderer descarta (incluye puntos/líneas que no llegan a Vulkan).
- `PSPRECOMP_GAME_LOG` — imprime el log de debug del motor (`[game] ...`). **Primer recurso para depurar.**
- `PSPRECOMP_MAX_DISPATCHES` — tope de dispatches (default 4 000 000 000).
- `PSPRECOMP_SPIN_YIELD_TICKS` — ticks con el mismo PC antes de forzar yield (default 4).

**Depuración del runtime / dispatch**
- `PSPRECOMP_TRACE`, `PSPRECOMP_TRACE_PC=<addr>`, `PSPRECOMP_TRACE_MEM`, `PSPRECOMP_TRACE_ON_ERROR`
- `PSPRECOMP_STRICT_PC_PROGRESS`, `PSPRECOMP_PROGRESS_EVERY=<n>`
- `PSPRECOMP_COUNT_PC=<lista de PCs>`, `PSPRECOMP_REPORT_DISPATCH_COUNT`
- `PSPRECOMP_PROFILE_DISPATCH`, `PSPRECOMP_PROFILE_DISPATCH_START`, `PSPRECOMP_PROFILE_DISPATCH_COUNT`
- `PSPRECOMP_HLE_HISTOGRAM` — histograma de llamadas HLE por NID.
- `PSPRECOMP_CHAIN_DEPTH=<n>`, `PSPRECOMP_NO_CHAIN` — límite / desactivar encadenado directo.
- `PSPRECOMP_WATCH_WRITE=<addr>`, `PSPRECOMP_WATCH_WRITE_SIZE=<n>` — vigila escrituras a memoria guest.
- `PSPRECOMP_HEAP_DIAG`, `PSPRECOMP_REQUEST_ALLOC_DIAG`
- `PSPRECOMP_FILE_OBJECT_DIAG`, `PSPRECOMP_FILE_OBJECT_STOP_ON_NULL`
- `PSPRECOMP_WORLD_STREAM_DIAG`, `PSPRECOMP_WORLD_STREAM_STOP_AT_CALLBACK`, `PSPRECOMP_WORLD_STREAM_MANAGER=<addr>` (default `0x08E91200`)
- `PSPRECOMP_REALTIME_SPEED_DIAG`, `PSPRECOMP_REALTIME_SPEED_INTERVAL`

**Video / GE**
- `PSPRECOMP_DUMP_FRAME=<dir>` — vuelca frames presentados a `<dir>/frame_N.ppm`;
  `PSPRECOMP_DUMP_FRAME_AT=<n,n,...>` elige cuáles.
- `PSPRECOMP_GE_BACKEND`, `PSPRECOMP_INTERNAL_WIDTH`, `PSPRECOMP_INTERNAL_HEIGHT` (normalmente vienen del `.ini`)
- `PSPRECOMP_GE_STRICT`, `PSPRECOMP_GE_PHASE_DIAG`, `PSPRECOMP_GE_DIRECT_NONINDEXED_DRAW`
- `PSPRECOMP_GE_PARALLEL_VERTEX_DECODE`, `PSPRECOMP_GE_PARALLEL_VERTEX_THRESHOLD`, `PSPRECOMP_GE_PARALLEL_VERTEX_MAX_WORKERS`, `PSPRECOMP_GE_PARALLEL_TEXTURE_DECODE`
- `PSPRECOMP_GE_GPU_*`: `HW_TRANSFORM`, `HW_LIT`, `HW_CULL`, `STAGE_VERTICES`, `TEXTURE_CACHE_MB`, `TEXTURE_DECODE_LIMIT`, `COLOR_PREVIEW`, `GEOMETRY_DEBUG_COLORS`, `DUMP_VBLANK`, `FORCE_WHITE_VERTEX`, `SKIP_SOFTWARE_RASTER`, `SKIP_OWNED_RASTER`, `SKIP_DISPLAYED_RASTER`
- `PSPRECOMP_RASTER_THREADS`, `PSPRECOMP_RASTER_PARALLEL_PIXELS`
- `PSPRECOMP_DX12_*` — heredadas de VCS/DX12, no aplican en Linux.

**Audio**
- `PSPRECOMP_AUDIO_DIAG`, `PSPRECOMP_SAS_DIAG`, `PSPRECOMP_AUDIO_SUMMARY`
- `PSPRECOMP_AUDIO_PREBUFFER_BLOCKS=<n>`, `PSPRECOMP_AUDIO_WAV=<ruta>` (graba la salida a WAV)

**Host (`SPONGEBOB_*`)**: `SPONGEBOB_WINDOW_SCALE`, `SPONGEBOB_VULKAN_VALIDATION`,
`SPONGEBOB_HUD_DIAG`, `SPONGEBOB_GE_NO_DEFER`.

Para la lista exacta: `grep -rn 'getenv("PSPRECOMP_' src include tools profiles/spongebob/host`.
