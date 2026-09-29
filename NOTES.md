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
