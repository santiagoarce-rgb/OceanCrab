#pragma once

// sceAtrac3plus HLE stubs — Opción A (sin FFmpeg por ahora).
//
// SpongeBob importa 8 funciones de sceAtrac3plus (codec ATRAC3+ para música).
// El análisis del ISO mostró que el juego es WAV-dominante (818 .wav, y solo
// SND0.AT3/ICON1.PMF de arranque), así que estos stubs permiten arrancar sin
// música ATRAC3+ hasta integrar un decodificador (FFmpeg) si hace falta.
//
// NIDs importados (ULUS10478_report_imports.csv), nombres aún sin resolver:
//   0x5D268707  0x61EB33F5  0x6A8C3CD5  0x7A20E7AF
//   0x7DB31251  0x868120B5  0x9AE849A7  0xE88F759B
//
// TODO(Fase 5): registrar estos stubs en spongebob_profile.cpp vía
//   runtime.register_hle("sceAtrac3plus", <nid>, <handler>);
// devolviendo 0/valores neutros para que el juego no dependa del codec.

namespace spongebob {

// Los handlers reales se implementan y registran en spongebob_profile.cpp
// (Fase 5). Este header documenta los NIDs pendientes y mantiene el namespace
// coherente mientras el perfil no usa ATRAC3+.

} // namespace spongebob
