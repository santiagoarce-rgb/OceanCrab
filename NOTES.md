# PSPRecomp — SpongeBob Notes

## Estado actual (2026-09-28)

- El juego bota y ejecuta su main loop (40M+ dispatches sin crash).
- `madd`/`maddu`/`msub`/`msubu` lowered.
- Yield cooperativo rompe deadlocks de busy-wait (análogo al "blr yield" de Wii).
- **Aún NO renderiza** (frames=0, `fb=0x00000000`).

## Fix: yield cooperativo (commit `f273833`)

El starvation hook detecta un PC repetido `PSPRECOMP_SPIN_YIELD_TICKS` ticks
(default 4) y fuerza `activate_next_thread()`, cediendo al worker listo.
Rompió 3 busy-waits: `0x08971930`, `0x08909D64`, `0x08905C60`.

## Diagnóstico del ciclo de vida de threads (run 9)

### Secuencia observada
1. `module_start` (uid 1) crea el **main game thread** (uid 2, entry `0x08804238`,
   prio 32, stack 512KB), lo arranca y sale vía `jal 0x00000000` (a0=2 →
   exit_status=2). Hand-off limpio, ya rastreado (sin `WARN: untracked`).
2. El main thread (uid 2) crea dos workers:
   - uid 3: entry `0x08909C8C` (prio 19, stack 2048).
   - uid 4: entry `0x08971878` (prio 16, stack 3072) — worker de audio.
3. Se rompen 3 busy-waits con spin-yield; 732 switches round-robin uid 2 ↔ uid 3.
4. `all PSP threads completed` (EXIT=4) — pero uid 2/3/4 **NO logean "complete"**.

### Hallazgo clave
uid 2/3/4 salen **sin log "complete"** → probablemente por
`sceKernelExitDeleteThread` (`0x809CE29B`), que hace `erase` del thread y
**no tiene log**. Solo se instrumentó `complete_current_thread` y
`sceKernelExitThread` (`0xAA73C935`).

### Próximo paso
1. Añadir log a `sceKernelExitDeleteThread` (`0x809CE29B`) para confirmar
   por qué sale el main thread (uid 2).
2. Decidir: (A) idle-loop — no parar al quedarse sin threads, procesar
   callbacks/VBlank hasta que aparezca un thread nuevo; o (B) arreglar la
   salida prematura del main thread.

## Línea de tiempo de la sesión

- Run 5: fix `madd` → el juego ejecuta 10M dispatches (tope de seguridad).
- Run 6: tope configurable (`PSPRECOMP_MAX_DISPATCHES`, default 4B) → main loop.
- Run 7: instrumentación GE/display → **no renderiza** (frames=0), busy-wait en `0x08971930`.
- Run 8 (análisis): el busy-wait espera `[[0x8A82704]+224] != 2` (decode de audio asíncrono).
- Run 9: yield cooperativo + registro de uid 1 → main thread (uid 2) sí existe,
  pero sale sin "complete".

## Sesión 2026-09-29 — wiring de renderizado (command walker GE + present)

### Implementado
- **Command walker de display lists** en `spongebob_profile.cpp`:
  - Constantes de opcodes GE (`kGeCommand*`), `ge_relative_address()`,
    `execute_ge_list()`.
  - Decodifica NOP / VertexAddr / IndexAddr / Primitive / BBox / Jump / BBoxJump /
    Call / Return / End / Signal / Finish / Base / OffsetAddr / Origin; guarda
    `ge_state.commands[op]` y consume matrices/morph (0x2A–0x3F) vía
    `update_ge_transform_state`. Renderiza con `render_ge_primitive()`.
  - Ejecución **síncrona** en `ge_list_enqueue` (espejo del path sync de VCS).
- **Present wiring**:
  - `sceDisplaySetFrameBuf` → `ge_gpu_backend_set_display_framebuffer()`.
  - `sceDisplayWaitVblank*` → `spongebob_profile_tick()` (drain GE + present) antes
    de dormir.
  - `present_display_frame()`: prefiere el readback del backend GPU
    (`finish_color_frame` + `game_frame_rgba` + `display_window_present_rgba`);
    fallback al blit por software (`display_window_present` sobre VRAM).
- `spongebob_profile_tick()` ahora drena la cola GE y presenta.

### Validación
- `-fsyntax-only` OK, compilación + link de `SpongeBobNative` OK (ninja).
- Smoke test headless (`SDL_VIDEODRIVER=dummy`, 60M dispatches, 150s):
  - Bota, crea threads uid 2/3/4.
  - **`[ge] enqueue list=0x08A59300`** → el walker ejecuta la lista sin error
    (no hay `rasterizer failed` ni `safety limit`).
  - `[ge] sync` / `[ge] draw_sync` OK.
  - Después de `[thread] start uid=4` (worker de audio), **se queda girando**:
    `frames=0 enqueues=1 draws=1 fb=0x00000000` repetido.

### Bloqueador real para el primer frame (NO es el rendering)
El juego **nunca llama `sceDisplaySetFrameBuf` ni `sceDisplayWaitVblank*`**: se
queda esperando el decode de audio asíncrono tras crear el worker de audio
(uid 4, entry 0x08971878). Coincide con run 8: busy-wait en
`[[0x8A82704]+224] != 2`. Los stubs de `sceAtrac3plus`/`sceAudio` devuelven 0 y
probablemente el worker nunca marca el flag "decode completo", así que el main
thread no avanza al render loop.

### Próximo paso
1. Instrumentar los stubs de audio (sceAtrac3plus + sceAudio) para ver cuál NID
   llama el worker uid 4 y qué flag espera el main thread (`0x8A82704+224`).
2. Hacer que el stub apropiado señale "listo" (o que el poll avance) para
   desbloquear el paso al render loop → entonces sí entra SetFrameBuf + present.

## Sesión 2026-09-29 (2) — diagnóstico con histograma HLE + IoFileMgr

### Instrumentación
- Añadido `runtime.report_hle_histogram(100)` en `main.cpp` (gated por
  `PSPRECOMP_HLE_HISTOGRAM=1`). Resuelve el nombre de cada NID vía `nids_`.

### Resultado del histograma (3M dispatches)
El juego **NO** está girando en audio. Los NIDs dominantes son de **file I/O**:
- `sceIoGetstat` (0xACE946E8): **444k** llamadas.
- `sceIoWrite` (0x42EC03AC): 222k.
- `StdioForUser` 0xA6BAB2E9: 222k.
- `sceUmdGetDriveStat` (0x6B4A146C): 111k.

Audio apenas: `sceAudio:0x5EC81C55` (4×), `sceSasCore:0x42778A9F` (1×),
**cero `sceAtrac3plus`**. El worker uid 4 no es el cuello de botella.

### Qué sondea el main thread (uid 2)
`sceIoGetstat` en bucle sobre **`fonts.pkg`** en 4 rutas (ra=0x089AEAF8):
1. `disc0:/PSP_GAME/USRDIR/pkg/fonts.pkg`
2. `disc0:/PSP_GAME/USRDIR/sound/common/pkg/fonts.pkg`
3. `disc0:/PSP_GAME/USRDIR/sound/music/pkg/fonts.pkg`
4. `pkg/fonts.pkg`

### Fix aplicado
`sceIoGetstat` ahora rellena un `SceIoStat` real (mode/attr/size, 0x58 bytes) y
devuelve `ENOENT` (0x80010002) si el archivo no está en el host (port de VCS).

### Bloqueador real → **assets no extraídos**
`profiles/spongebob/original/` **solo contiene `ULUS10478_EBOOT.ELF`** — no hay
`PSP_GAME/USRDIR/`. El juego espera `fonts.pkg` en el disco (UMD) y reintenta
para siempre aunque `sceIoGetstat` devuelva ENOENT. Es un problema de **datos**,
no de código.

### Próximo paso
1. Extraer los assets del juego (contenido de `PSP_GAME/USRDIR`) a
   `profiles/spongebob/original/PSP_GAME/USRDIR/` — mínimo `pkg/fonts.pkg`.
2. Implementar `sceIoOpen`/`sceIoRead`/`sceIoClose` (Fase C) para que, una vez
   presente el archivo, el loader pueda leerlo y avanzar al render loop.
