#ifndef _MELEE_SDK2_H_
#define _MELEE_SDK2_H_

// SDK2: PC re-implementation of the GameCube SDK modules that touch
// hardware. Portable SDK logic is compiled from the original sources where
// possible; everything here replaces a hardware-backed original:
//
//   sdk2_os    OS (time, interrupts, heap, alarms, threads, mutex, ...)
//   sdk2_dvd   DVD filesystem over a host directory (disc root)
//   sdk2_card  Memory-card slots backed by host files
//   sdk2_ar    ARAM as a host pool (+ address translation for AX)
//   sdk2_ax    AX voice mixer in software (+ AXFX stubs)
//   sdk2_mtx   MTX/VEC math in portable C (originals are paired-single ASM)
//   sdk2_thp   THP movie player stubs (blank playback, no hangs)
//   sdk2_exi_si EXI/SI stubs
//
// GX/VI/PAD/AI live in src/hal (they drive real host hardware).

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- global SDK2 configuration (set by pc_main from settings) ----
void sdk2_set_disc_root(const char* path); // directory with GALE01 files
const char* sdk2_disc_root(void);
void sdk2_set_card_path(int chan, const char* path); // memcard image/dir
const char* sdk2_card_path(int chan);
void sdk2_set_user_dir(const char* path); // saves, settings, logs
const char* sdk2_user_dir(void);
void sdk2_set_mem_size_mb(unsigned mb); // simulated console RAM (24/48)
unsigned sdk2_mem_size_mb(void);

// GC address translation for sample data etc:
//   sdk2_addr_host(gcaddr) -> host pointer or NULL.
void sdk2_addr_register(u32 gc_addr, void* host, u32 size);
void* sdk2_addr_host(u32 gc_addr);
void sdk2_addr_unregister(u32 gc_addr);
// MRAM-vs-ARAM test (addr < 16MB -> ARAM, else MRAM); replaces retail
// `< 0x80000000` checks in lbmemory/lbfile/ftdata.
int sdk2_addr_is_aram(u32 addr);

#ifdef __cplusplus
}
#endif

#endif
