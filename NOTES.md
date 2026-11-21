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

## Sesión 2026-09-29 (3) — extracción de assets + IoFileMgr mínimo

### Extracción de assets
- ISO UMD en `/run/media/arce/BIG_GAMES/.../Spongebob_Truth_Or_Square_USA_PSP.iso`
  (1.4 GB, ISO9660 en sector 16). `bsdtar` no lo lee; usado `pycdlib` (pip) para
  listar y extraer.
- Extraído `PSP_GAME/` (1.3 GB, 968 archivos) a `profiles/spongebob/original/`.
  `USRDIR/{movies,movies_de,moviescom,pkg,sound}` + `SYSDIR` + `PARAM.SFO`.
- `game_root` por defecto apunta a `out/.../profiles/spongebob/original/...`;
  creado symlink `out/spongebob-config-test/profiles/spongebob/original` →
  `profiles/spongebob/original` para que el binario encuentre los assets.

### IoFileMgr mínimo (Fase C)
- `sceIoOpen` (0x109F50BC), `sceIoRead` (0x6A638D83), `sceIoClose` (0x810C4BC3),
  `sceIoWrite` (0x42EC03AC) implementados con `std::fstream` + `FileTable`
  (fd → stream). Port mínimo de VCS.

### Resultado — el juego ya renderiza
- Carga en orden `fonts.pkg`, `scripts.pkg`, `common.pkg`, `strdb_en.pkg`,
  `logos.pkg`, ... (los 968 assets).
- Entra al main loop (uid 2/3/4 conmutando) y **enqueue 598+ display lists GE**
  (`enqueues=926 draws=926` y subiendo), con `sceGeDrawSync`.

### Bloqueador final para el primer frame → display no se presenta
- `[frame] frames=0 ... fb=0x00000000`: **`sceDisplaySetFrameBuf` NO se llama**
  (o con framebuf 0) y **`sceDisplayWaitVblank*` tampoco**. El path de present
  (`wait_vblank` → `spongebob_profile_tick` → `present_display_frame`) nunca
  dispara porque `display_state.frame_buffer == 0`.

### Próximo paso
1. Ver por qué el main loop no llama `SetFrameBuf`/`WaitVblank`: revisar
   `sceDisplayGetFrameBuf` (0xEEDA2E54) y los 8 NIDs de sceDisplay; quizá el
   swap de buffer va por callback (`WaitVblankStartCB`) o por `sceDisplaySetFrameBuf`
   con sync distinto.
2. Posible fix: presentar desde `sceGeDrawSync`/starvation hook en vez de
   `wait_vblank`, o inicializar `display_state.frame_buffer` a un buffer válido.

## Sesión 2026-09-29 (4) — 🎯 PRIMER FRAME RENDERIZADO

### Causa raíz del display muerto
`register_other_stubs()` (llamado **después** de `register_display_hle`) incluía
los 5 NIDs de `sceDisplay` en `kOtherStubs` y los **sobrescribía** con stubs
genéricos (return 0):
- `0x0E20F177` SetMode, `0x289D82FE` SetFrameBuf, `0x46F186C3` WaitVblankStartCB,
  `0x984C27E7` WaitVblankStart, `0x9C6EAAD7` GetVcount.

Como `register_hle` sobreescribe, el handler real de SetFrameBuf/WaitVblank nunca
se ejecutaba: `display_state.frame_buffer` quedaba en 0 y el present no disparaba.
(El histograma mostraba `count=4` en SetFrameBuf pero `[display] framebuf` = 0
porque iba al stub genérico.)

### Fix
- Quitados los 5 NIDs de `sceDisplay` de `kOtherStubs`.
- Añadido `sceDisplayGetVcount` (`0x9C6EAAD7`) en `register_display_hle`,
  devolviendo `virtual_time_us / kVblankPeriodUs` (contador monotónico).

### Resultado
- `[display] framebuf=0x04000000` ↔ `0x04044000` (doble buffering, 512×272).
- **191 frames presentados** (`frames=191`), 193 GE draws, pacing vblank
  (`wait_vblank count=100`), cero errores de rasterizer.

### Nota siguiente
Tras ~191 frames el main loop cede y el worker de audio (uid 4) queda girando
en `0x08A09B4C` — siguiente fase: audio (Fase E, sceSasCore/sceAudio ya semi-hechos).

### Tooling de verificación visual
- `PSPRECOMP_DUMP_FRAME=<dir>`: vuelca los frames presentados (en los counts
  1, 40, 80, 120, 160, 190) a `<dir>/frame_N.ppm` para inspeccionar el contenido
  sin display (headless). Confirma que se renderiza contenido real (texto/logos),
  no solo clear o pantalla negra. Los PNG se derivan con ffmpeg y van en `dumps/`
  (ignorado por git).

## Sesión 2026-09-29 (5) — sceAudio mínimo (Fase E) + gap de codegen

### sceAudio (reserve + output + pacing)
- Añadido `AudioChannelState` + `audio_channels[8]` y `register_audio_hle()`:
  - `sceAudioChReserve` (0x5EC81C55), `sceAudioOutputBlocking` (0x136CAF51),
    `sceAudioOutputPannedBlocking` (0x13F592BC), `sceAudioOutput` (0xE2D56B2D).
  - `audio_queue_buffer` (pacing en la línea de tiempo virtual) + `suspend_for_wakeup`
    para que el worker de audio duerma en vez de girar.
  - Envía PCM a `audio_output_submit` cuando `PSPRECOMP_AUDIO` está activo.
- Se registra **después** de `register_other_stubs` (sobrescribe los stubs).

### Resultado
- El worker de audio ya no gira: `sceAudioOutput*` 2081×, `__sceSasCore` 2081×.
- El juego avanzó de **191 → 713 frames** (mucha más carga/loading).

### Nuevo bloqueador → gap de codegen
- `Runtime stopped: Unsupported ... 0x00000000 at 0x0892802C: invalid internal
  function entry`. `0x0892802C` **no está** en `ULUS10478_report_functions_auto.csv`
  → el `--auto` codegen no generó esa entrada (probable salto computado / jump
  table no resuelto).

### Próximo paso
1. Investigar `0x0892802C`: añadir como seed en `functions.csv` y regenerar el
   corpus AOT (o ver si es un salto computado que el análisis perdió).

## Sesión 2026-09-29 (6) — detección de jump tables / funciones tras salto

### Causa raíz del gap de codegen
`0x0892802C` era una entrada de **jump table** (`j 0x0891C8D4`, precedida de otro
salto). El análisis (`collect_initial_seeds` en `program_analysis.cpp`) solo
sembraba targets de `jal` y punteros R_MIPS_32 relocados — NO detectaba entradas
de jump table ni funciones alcanzadas por salto computado (`jr $reg`).

### Fix (reusable en el framework)
En `collect_initial_seeds`:
1. Sembrar también targets de `j` (tail calls) — `direct_j_target`.
2. Sembrar la **dirección** de cualquier instrucción inmediatamente después de un
   salto sin enlace (`j`/`jr`, con su delay slot) — `after_jump_entry`. Cubre:
   - entradas de jump table (`j`/`jal` consecutivos);
   - funciones alcanzadas por puntero/computed jump (prólogo `addiu $sp,...` tras
     `jr $ra`).

### Resultado
- `0x0892802C` y `0x08828268` ahora están en el corpus (units 0009/0001).
- El juego avanza más allá de cada gap (719 frames), pero van apareciendo más
  gaps de salto computado — el fix general los cubre en una sola pasada de
  regeneración.

### Nota
Regenerar el corpus + recompilar las 20 units lleva ~1 h (units de ~3.4 MB con
-O2). Es un costo único por cambio en `program_analysis.cpp`.

## Sesión 2026-09-29 (7) — "problem reading the disc" (estado UMD)

### Síntoma
El juego mostraba su pantalla de error **"there was a problem reading the disc,
press any button to try again"**, con el font del juego (¡el text rendering
funciona perfecto!). NO es crash: es el error handling del propio engine.

### Causa raíz
`sceUmdGetDriveStat` devolvía `2` (solo "media presente", sin bit de "ready"), y
`sceUmdCheckMedium` devolvía `0` (stub genérico). El juego lo interpretaba como
"disco no listo" y mostraba el error.

### Fix
- `sceUmdGetDriveStat` (0x6B4A146C) → devuelve `0x32` (media + ready + reading),
  igual que VCS.
- `sceUmdCheckMedium` (0x46EBB729) → devuelve `1` (igual que VCS).

### Resultado
El juego avanza más (1712 frames) y el contenido evoluciona (loading → pantalla
con logo naranja + azul claro #73BDFF). El error de disco ya no bloquea.

### Próximo paso
`sceCtrl` (input): la pantalla de error pide "press any button", y el juego en
general necesita input. Los stubs de sceCtrl (0x1F4011E6, 0x1F803938, 0x3A622550)
devuelven 0.

## Sesión 2026-09-29 (8) — sceCtrl (input) + confirmación del estado

### Diagnóstico (logging UMD + ctrl)
- `[umd] CheckMedium/Activate/WaitDriveStat/GetDriveStat` se llaman **una sola
  vez** y pasan (0x32, 1, 0, 0). El fix de UMD funciona.
- `[ctrl] pad` se llama repetidamente (el juego sondea input). Confirma que el
  juego quedó esperando "press any button", NO re-chequeando el UMD.

### sceCtrl implementado
- `sceCtrlSetSamplingMode` (0x1F4011E6) → 0.
- `sceCtrlPeekBufferPositive` (0x3A622550) y `sceCtrlReadBufferPositive`
  (0x1F803938) → rellenan `SceCtrlData` (16 bytes) desde `display_window_input()`:
  TimeStamp, Buttons (bitmask PSP), Lx/Ly, rx/ry.

### Verificación visual (para el usuario)
Correr LIVE (sin SDL_VIDEODRIVER=dummy) y presionar un botón → el juego debería
salir de la pantalla "press any button" y continuar.

## Sesiones 2026-10-26/27 — menú principal y workarounds de Bink (resumen)

Reconstruido a partir de los commits; esas sesiones no se anotaron en su momento.

- `190f8d3`: se corrigió el mapeo de `game_root`, se añadió `sceIoLseek` y los
  `.bik` se saltan en modo headless → **llega al menú principal**.
- Bink (videos enlazados estáticamente en el EBOOT): pruebas sucesivas con un
  header falso (`db7bb46`, `5413cdb`, `b6ee145`: tamaño 44, frames 1 → 0). La
  solución estable fue `faef3e8`: hook a `sub_08943694` (accessor del Movie
  manager) que devuelve un objeto Movie falso en RAM guest, con una vtable que
  reporta "completo". Va condicionado a `PSPRECOMP_SKIP_BIK`.
- `63f7718`: Memory Stick falso; el diálogo de savedata termina al instante.
- `9f22e2d`: `sub_08943060` → 0. El fondo animado del menú lee la lista
  `obj+0x180`, que se llena desde `menu_bkg.pvs`, y ese archivo no existe suelto
  (las mallas están dentro de `mm.pkg`).
- Después de "New Game", los personajes 3D no se crean: en la cadena de
  constructores `0x0894C094 → 0x0894E4E4 → 0x08956928` llega `this = null`
  porque `operator new` (`0x089C4998`) devuelve 0. Pendiente: rastrear el heap.

## Sesión 2026-11-20 — docs, CI, null-jump check y VFPU vi2x

### Proyecto / docs
- Se reescribió el README (banner, estado, pasos de build, **roadmap**) y se
  añadieron `CLAUDE.md`, `CONTRIBUTING.md`, `THIRD_PARTY.md` y
  `docs/ENVIRONMENT.md`. CI con GCC y Clang en un contenedor Arch.
- **Fases del proyecto** (ver README → Roadmap): (1) recompilar bien
  (corrección: VFPU y quitar hacks), (2) entender el código (simbolizar
  estructuras y pasarlas a overrides nativos), (3) extras (mejoras, mods,
  co-op online, otras plataformas).

### Depuración
- `cf37156`: `PSPRECOMP_NULL_JUMP_CHECK` atrapa `jalr` y `jr $reg≠ra` a 0 y llama
  a `Runtime::null_indirect_jump`, que vuelca los GPR. `jr $ra` queda exento
  porque las entradas de thread regresan legítimamente a `ra=0`. ON por defecto
  en Debug.

### VFPU
- Un escaneo del `.text` con el decoder encontró que **`vi2s` era la única
  instrucción VFPU que el codegen no manejaba**: 64 sitios en
  `0x089D5DE8–0x089D6D68`, el camino de audio de Bink de float a PCM s16
  (`vscl → vf2iz → vi2s.q → sv.q`).
- `11c2c47`: `OpcodeKind::Vi2x` + `execute_vfpu_vi2x` (con prefijos de origen y
  destino) + codegen + tests.
- El corpus anterior es del 2026-11-15 (anterior a vi2x). Se regeneró y
  recompiló en esta sesión.

### Próximo paso
1. Probar "New Game" **sin** `PSPRECOMP_SKIP_BIK` para ver si Bink decodifica de
   verdad.
2. Si funciona, retirar uno por uno los hooks falsos de Movie/Bink.
3. Rastrear el `operator new` que devuelve 0 en la creación de personajes.

## Sesión 2026-11-20 (2) — Bink corre nativo; bugs del scheduler

- El corpus ya incluía `vi2x` (`rewritten units: 0`); se recompiló igual.
- **Sin `PSPRECOMP_SKIP_BIK`**, el main thread (uid 2) saltaba a 0 en el IDCT
  de Bink (`0x089E58C8`) y el runtime lo daba por terminado. Causa:
  `yield_current_thread` (que solo usa el spin-yield) guardaba
  `pc = $ra`, como si viniera de una HLE, cuando en realidad estaba en un
  boundary arbitrario. Eso forzaba un retorno prematuro. Fix: retomar en
  `ctx.pc` (`a48f903`).
- Ese bug era el que "hacía funcionar" los hooks de SKIP_BIK: no ponían
  `ctx.pc = ra` y se ejecutaban en loop hasta que el spin-yield los sacaba.
  Fix: retorno explícito (`6d1b87d`). **Regla: todo `register_function` debe
  setear `ctx.pc`.**
- Segundo cuelgue: `sceKernelExitDeleteThread` no despertaba a quien estaba en
  `sceKernelWaitThreadEnd`. Bink crea un helper por video (entrada `0x089CCAE4`)
  y el main lo espera. Fix en `a5e200b` (además, el waiter recibe el exit status
  en v0).
- Resultado: nick.bik → thq.bik → blt.bik → title screen "Press Start", sin
  hooks. Con SKIP_BIK también llega al title.
- **Abierto:** los videos se ven solo en luminancia y en un canal (verde en
  nick, rojo en Barking Lizards). Sospechas: la conversión YUV→RGB (¿VFPU mal
  bajado?) o el formato de textura/CLUT con el que el juego sube el frame al GE.
- **Abierto:** ~1 950 sitios `vfpu0/vfpu3/vfpu/cop2 not lowered` en el corpus,
  más ~6 900 `unknown`; probablemente sean datos decodificados como código.
  Auditarlos.
- Nuevo: `PSPRECOMP_DUMP_FRAME_AT=<n,n,...>` (`d579f2a`).

## Sesión 2026-11-20 (3) — 🎯 New Game llega a gameplay

- El usuario confirmó en vivo que los videos se ven (sin azul, sin audio) y que
  New Game reproduce la cinemática y truena al terminar.
- Para reproducirlo en headless se añadió `PSPRECOMP_INPUT_SCRIPT` (ver CLAUDE.md
  para el guion que llega a gameplay).
- Crash en `0x08943728`: `sub_08943694` dereferencia el 0 que devuelve
  `0x089A7690` (búsqueda de recurso; llama al printf de debug si falla).
- **`PSPRECOMP_GAME_LOG`**: hook a `sub_089C9E34`, el printf de debug del motor
  ("Whiptail Engine"). El log dijo directamente:
  `CWGraphics3D::LoadMesh - Could not load patrick.msh` (el archivo sí está en
  hub.pkg/lvlcmn.pkg).
- Causa raíz: `CPSPSystem::InitializeHeap` crea una FPL de
  `MaxFreeMemSize - 1 MiB`; el HLE devolvía 8 MiB fijos → heap de 6.6 MiB. Fix
  (`e023fe3`): memoria libre real (arena desde `0x08B70000`), reuso de stacks
  de threads borrados y objeto Movie falso movido a scratch. Heap: 19.5 MiB.
  Esto también explica el viejo `operator new` → 0 al crear personajes.
- Resultado: hub (la casa de Bob Esponja) con HUD y Patrick; 32k+ frames sin crash.
- Pistas falsas descartadas: `[io] open/read/getstat` se truncan tras 30 líneas
  (`traced < 30`); `sceIoReadAsync/PollAsync` (stubs) solo los usa el módulo de
  E/S de Bink (`0x089E9xxx`), no el cargador de paquetes.
- Abierto: Bink sin canal azul; "Bink audio thread" (entrada `0x089CCAE4`)
  termina al instante → videos sin audio. Overrides con `register_function` no
  atrapan llamadas que caen dentro de la misma unit (goto interno).

## Sesión 2026-11-20 (4) — 🏁 juego completable

- Crash al pegar en el nivel 1: `Unsupported 0x0043002E at 0x0897AA94`. El
  decoder tenía `msub/msubu` en SPECIAL funct `0x1E/0x1F`, pero en Allegrex van
  en `0x2E/0x2F` (`0x1E/0x1F` no aparece en el `.text`; `0x2E` aparece 9 veces).
  Fix en el decoder + tests (`680441d`), corpus regenerado (6 units, ~6 min de build).
  La msub está en `sub_0897AA30` (llamada por puntero), en un cálculo de índice
  envuelto (`LO = t1 - v0*v1`).
- El usuario jugó los 3 primeros niveles, usó el cheat de niveles desbloqueados,
  pasó el nivel final y llegó a los créditos y la cinemática final. Los minijuegos funcionan.
- Abierto (por prioridad): láseres de seguridad del museo (nivel 3) sin textura;
  velocidad del juego acelerada; agua y arcoíris raros; un modelo del nivel 1;
  Bink sin canal azul ni audio.

## Sesión 2026-11-21 — velocidad (pacing) y láseres

- Láseres del museo: `PSPRECOMP_GE_UNSUPPORTED_DIAG` no registró sprites 3D
  descartados. El log del motor muestra `PVS mesh not found: sl03_*.msh` (13
  mallas que de verdad no están en sl03_bms.pkg; probablemente también pasa en la
  PSP real). Sospecha viva: puntos/líneas solo pasan por el raster de software y
  nunca llegan a Vulkan (`gpu_lost_*` en el diag). El usuario lo pospuso; la
  primera captura se truncó (faltó `grep --line-buffered`).
- Velocidad: el tiempo virtual salta al siguiente VBlank cuando todo espera y
  nada lo amarraba al reloj real → 1.71× en menús. `precise_sleep_until` existía
  pero no se llamaba. Fix: `pace_virtual_time_to_wall_clock()` en cada espera de
  VBlank, más quitar la doble cuenta de `advance_system_time`. Medido: 1.00× en
  menús y en el hub (~15 % de margen en headless).

## Sesión 2026-11-21 (2) — colores de Bink

- `PSPRECOMP_GE_TEXTURE_LOG` mostró que Bink dibuja Y (480 px) y U/V (240 px) como
  sprites CLUT8 (`fmt=5`, `tfunc=3`) y hace YUV→RGB en el GE: cada plano de
  crominancia se dibuja dos veces, con `blend=0x0AA` (FIX/FIX ADD, suma la
  contribución positiva) y con `0x2AA` (FIX/FIX REVERSE_SUBTRACT, resta la
  negativa). FIXA = FIXB = blanco.
- Bug: `blend_variant()` en el backend Vulkan solo conocía 5 combinaciones ADD;
  todo lo demás → blending apagado (sobrescribe). La pasada de resta pisaba el
  frame y se perdía el azul.
- Fix (`f1271c4`): `generic_blend()` mapea las ops (add/sub/rev-sub/min/max) y
  los factores (FIX → ONE/ZERO/constante) para los estados no cubiertos. Las 5
  variantes viejas no cambian. Videos y cinemáticas con color correcto; menú y
  hub sin regresiones. Puede afectar al agua y los arcoíris (pendiente de ver).

## Sesión 2026-11-21 (3) — resolución interna (pendiente, fase 3)

- El renderer (heredado de lcs-recomp) soporta `InternalResolutionMode=Scale`
  + `InternalScale=N` en el .ini. En headless a 3× el dump sale a 1440×816 y el
  2D (logo del menú) se ve nítido.
- El usuario lo probó en vivo y **el 3D se sigue viendo en baja resolución**.
  Investigar en la fase 3 si la geometría 3D realmente se rasteriza al target
  escalado o si solo se escala la composición final.

## Sesión 2026-11-21 (4) — 🔊 audio de Bink

- El "Bink audio thread" (`0x089CCAE4`) hace en bucle
  `sceKernelLockMutex("Bink shutdown Mutex", 1, timeout=5000)`: mientras le da
  `WAIT_TIMEOUT` (el main lo tiene) decodifica y envía audio; si lo consigue, el
  video terminó y sale. Los mutex eran stubs que devolvían 0 → salía al instante.
  Fix (`bb9af3e`): mutex reales (dueño/cuenta, waiters FIFO, recursivo con attr
  0x200, timeout por el camino de deadlines del scheduler). `PSPRECOMP_MUTEX_DIAG=1`.
- El hilo de sonido del motor (`0x089E05C8`, mismo patrón con "Shutdown Mutex")
  recibía `StartThread(uid, 4, sp)`: `argp` apuntaba al frame del main, ya
  reutilizado cuando el hilo corría → mutex 0 → salía. Fix (`711826b`): copiar el
  bloque de args a la pila del hilo nuevo, como el kernel real.
- Bink sale por **`sceAudioOutput2*`** (canal SRC estéreo 44.1 kHz), que era un stub
  mudo. Fix (`1efe63a`): Reserve/OutputBlocking/GetRestSample/Release, mezclado en
  el canal 8 del host. Verificado: nick/thq/blt envían 38/73/64 bloques de 4096
  (= 3.5/6.8/6.0 s, igual que los headers); el usuario lo escuchó en vivo.
- Pendiente menor: el main hace `LockMutex(0)` en `0x089E0D0C` (struct sin mutex;
  devuelve UNKNOWN_MUTEX, inofensivo por ahora). Los event flags siguen siendo stubs.
- Ojo en pruebas: `timeout` (SIGTERM) no siempre mata el binario; usar `timeout -s KILL`.

## Sesión 2026-11-21 (5) — 🎨 matriz de textura y alfa del blend

- Reporte del usuario (nivel 9, "24 Hours of Fun at the Krusty Krab"): ácido
  estomacal y agua de color plano, lámparas con halo sólido; además láseres
  invisibles (nivel 3) y nubes blancas duras en el menú.
- `PSPRECOMP_GE_TEXTURE_LOG` + `WHITE FALLBACK` (nuevo) descartaron texturas
  faltantes: 19 materiales usaban `uvgen=1` (UV × matriz de textura).
- Bug 1 (`4dac56b`): `execute_ge_list` solo pasaba `0x2A–0x3F` al estado de
  transformación; TMS/TMD (`0x40/0x41`) se perdían y la matriz quedaba en ceros →
  UV (0,0) → un solo texel. Arregló agua, lava, ácido, cielos y láseres.
- Bug 2 (`ffc91aa`): el juego dibuja en framebuffer **5551**; `ge.frag`
  cuantizaba el alfa a 0/1 (≥0.5) *antes* de salir, y Vulkan usa esa salida como
  `SRC_ALPHA`. Con blend activo ahora se deja el alfa completo. Arregló nubes
  (textura `0x041A9A00`, alfa suave de 64 niveles), flores-nube del cielo,
  sombras, halos de lámparas, luces y un objeto del nivel 1.
- Herramientas nuevas (`333b053`): `PSPRECOMP_GE_SKIP_TEXTURE` (bisección de
  texturas comparando `PSPRECOMP_DUMP_FRAME`), `PSPRECOMP_GE_DUMP_TEXTURE` (.pam).
- Las nubes del menú salen en headless hacia el frame 1000 (escena de la isla,
  antes de que baje la cámara); el título con casas, hacia el 1250.
- Abierto: detalles estéticos en algunas texturas de los peces del hub.
- Se eliminó `PSPRECOMP_SKIP_BIK` (header Bink falso, objeto Movie falso en
  `sub_08943694`, `sub_08943060` → 0). `[0x08B61000, 0x08B70000)` queda libre.

## Sesión 2026-11-21 (6) — 🧭 fase 2: simbolización automática

- `tools/dump_image` vuelca la imagen reubicada; `tools/symbolize/symbolize.py`
  la analiza sin Ghidra. Primer resultado: **399 clases** (RTTI de GCC), 255 con
  padre, 399 vtables, **3 120 funciones nombradas** de 13 876.
- Fuentes: RTTI (`type_info`, `__si`/`__vmi`), vtables (`{0, &ti, vfuncs}`),
  ctors/dtors (guardan `&vtable`), mensajes `Clase::Método`, tablas `luaL_reg` y
  propagación de nombres por slot de vtable.
- Ojo: muchos mensajes empiezan con `\n`; los mensajes se copian y pegan
  (`AddStrike` dice ser `CCharType::AddAttack`), así que la tabla de Lua manda.
- Coincide con lo ya sabido: `0x0890A6FC` = `CPSPSystem::InitializeHeap`,
  `0x089A7690` = `CWGraphics3D::LoadMesh`, `0x089C9E34` = printf de debug.
- **API de Lua de los niveles** (39 tablas): `Ent`, `Char`, `CharType`, `Plat`,
  `Trig`, `Timer`, `Var`, `Game` (32), `Cam`, `Cheat`, `Sound`, `Laser`, `Wardrobe`,
  `Dialogue`, `PickUp`… más `math`/`string`/`table`/`debug`: motor con Lua 5.x
  embebido. Base para mods (fase 3).
- Jerarquía: `CWEntity` → `CWRenderable` → `CWStatic` → `CSBStatic` → entidades
  del juego (`CTikiEnt`, `CLaserEnt`, `CBombEnt`…); `CSBWorld` → `CWSceneMesh` →
  `CWWorld` → `CWScene`; armas `CWPWeaponBase` (11); enemigos
  `CStdEnemyControl` (11); UI `CLayerBase` (30).
- Runtime con nombres: `psprecomp/symbolizer.hpp` (fuera de `runtime.hpp` a
  propósito: tocarlo recompila las 20 units). Crash dumps, `TRACE_ON_ERROR` y
  `GAME_LOG` muestran `Clase::Método+0xN`. `sub_0890D7C8` = init del motor
  ("Whiptail Engine Version 1.1.0 Initializing").
- Tip: si un `git checkout` toca `runtime.hpp`, `touch -r` con un header viejo
  evita la recompilación completa (Ninja usa mtime).
- Siguiente: nombrar el jugador (`CCharEntBase`/`CCharControlBase`), la cámara
  (`CWStandardCamera`) y el cargador de niveles (`CWWorld::LoadManifest`
  `0x088143FC`) en Ghidra con `ImportSymbols.java`, y pasar los nombres al runtime
  (trazas con nombre).

## Sesión 2026-11-21 (7) — 🧽 CSpongeBobEnt (el jugador)

- Método: `PSPRECOMP_OBJECT_DUMP` (vuelca objetos con la vtable `0x08A74978`) +
  `PSPRECOMP_INPUT_SCRIPT` con stick (`mask@lx:ly`), y diff entre frames quieto
  vs. caminando. Layout en `host/game/spongebob_player.hpp`.
- Fábrica `0x08A195A4`: `new(0x928)` + ctor `0x0882A14C` (base `CCharEntBase`
  ctor `0x0882851C`). El ctor construye `CSpongeBobController` **embebido en
  +0x470** con back-pointer, `CWMorphController` en +0x7C0, `CEffectHandler` en
  +0x8EC, y se registra en el **gestor de sesión**: `[[0x08A82488] + 0x124] = this`.
  El gestor (316 B, sin RTTI) se crea en `0x08805680` y se borra en `0x08804704`.
- Confirmado en vivo (`PSPRECOMP_PLAYER_LOG`): posición `+0xE8` (fila 3 de la
  matriz del mundo `+0xB8`), yaw `+0x4C`, velocidad `+0x160` (~4.4 u/s caminando,
  y = -0.25/-0.51 en el suelo), stick `controller+0x94` (0..1).
- `sub_08943694` (el viejo hook "Movie manager" de SKIP_BIK) es en realidad
  `CWAnimated::vfunc_30`.
- En headless, el jugador aparece hacia el frame presentado ~3000–4900 según la
  corrida (la cinemática de New Game dura); el control responde poco después.
- Sesión jugada por el usuario (nivel 1) grabada con `PSPRECOMP_PLAYER_RECORD`
  y cruzada con `tools/symbolize/player_record.py`: salto (vy +5.07, doble +4.46,
  gravedad ~0.25/frame), estado de movimiento `controller+0x08` (0 quieto, 1
  camina, 3 aire, 7 espátula (△), 9 spin slap (□), 18 globo de agua (○)), acción `controller+0xD0`,
  flags `controller+0x64` (0x40 suelo, 0x10 moviéndose, 0x04 doble salto usado),
  combo `controller+0x6C` (1..3) y velocidad máxima `controller+0xA4` (4.40 →
  2.60 atacando).
- Cerrar la ventana ahora termina el juego limpio (antes `g_closed` no se leía).
- Salud: `controller+0x80` = 4 → 3 en el único golpe recibido (probablemente los
  calzoncillos del HUD). Al entrar al nivel el objeto se reinicializó en la misma
  dirección (no era una muerte); ver abajo: no siempre pasa.
- 2.ª grabación (2 muertes, 2 vidas ganadas): salud confirmada, 4→3→2→1→0 y
  vuelve a 4 al reaparecer en el checkpoint; golpes cada ~215 frames
  (invulnerabilidad ~3.6 s).
- Vidas/puntos NO están en el jugador: Lua `Lives.AddLives` (`0x088A00F8`) llama
  `SetLives(G, [G+4]+n)` (`0x0881CFE0`, tope 99) con `G = [0x08A8248C]`; la función
  vecina `0x0881CFFC` pone `G+8` con tope 999999 (los puntos del HUD). En New Game:
  vidas 3, puntos 0 (`PSPRECOMP_PLAYER_LOG`).
- La dirección del jugador NO es fija: en la 2.ª grabación (con créditos antes)
  el hub usó `0x08E81FE8` y el nivel creó uno nuevo en `0x08DF603C`. Buscarlo
  siempre por la sesión (`find_player`).
- Cámara de juego = `CCameraStory` (vtable `0x08A741D0`; `CWStandardCamera` vive
  desde el arranque y es la vista de plataforma). Ojo `+0x08`, objetivo `+0x84` =
  jugador + 1.3 en y, distancia fija ~3.11; L recentra detrás del jugador. Layout en
  `host/game/spongebob_camera.hpp` (`PSPRECOMP_OBJECT_RECORD`).
- Pendiente: grabar cámara con el usuario moviéndola, vidas/puntos en una grabación,
  enemigos (`CStdEnemyEnt`).

## Próxima sesión
1. Peces del hub (texturas raras), si vale la pena.
2. Fase 2: simbolización con ayuda de `PSPRECOMP_GAME_LOG`.
