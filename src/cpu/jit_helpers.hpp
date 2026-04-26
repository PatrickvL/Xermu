#pragma once
// ---------------------------------------------------------------------------
// jit_helpers.hpp — Declarations for extern "C" helpers called from JIT code.
//
// These functions are defined in jit_helpers.cpp and called directly from
// host code emitted by the trace builder. They use C linkage so the JIT
// can emit CALL rel32 to their addresses without C++ name mangling.
// ---------------------------------------------------------------------------

#include "context.hpp"
#include <cstdint>

struct Executor;

extern "C" {

// MMIO slow-path dispatch (called from hot-patched fastmem sites).
void     mmio_dispatch_read(GuestContext* ctx, uint32_t pa,
                            uint32_t dst_gp_idx, uint32_t size_bytes);
void     mmio_dispatch_write(GuestContext* ctx, uint32_t pa,
                             uint32_t src_gp_idx, uint32_t size_bytes);
void     mmio_dispatch_write_imm(GuestContext* ctx, uint32_t pa,
                                 uint32_t imm_val, uint32_t size_bytes);

// VA→PA page-table walk (called from JIT when paging is enabled).
uint32_t translate_va_jit(GuestContext* ctx, uint32_t va, uint32_t is_write);

// PUSHFD / POPFD helpers.
void     pushfd_helper(GuestContext* ctx, uint32_t eflags_val);
uint32_t popfd_helper(GuestContext* ctx);

// Guest memory helpers (paging-aware, used by complex instructions).
uint32_t read_guest_mem32(GuestContext* ctx, uint32_t addr);
void     write_guest_mem32(GuestContext* ctx, uint32_t addr, uint32_t val);

// ESP-related helpers (ESP is not in a host register).
uint32_t call_mem_helper(GuestContext* ctx, uint32_t pa, uint32_t retaddr);
void     push_esp_helper(GuestContext* ctx);
void     pop_esp_helper(GuestContext* ctx);
void     mov_esp_from_mem(GuestContext* ctx, uint32_t pa);
void     mov_esp_to_mem(GuestContext* ctx, uint32_t pa);

// High-byte register (AH/CH/DH/BH) memory move helpers.
void     mov_highbyte_from_mem(GuestContext* ctx, uint32_t pa, uint32_t gp_idx);
void     mov_highbyte_to_mem(GuestContext* ctx, uint32_t pa, uint32_t gp_idx);

// PUSH/POP [mem] helpers.
void     push_mem_helper(GuestContext* ctx, uint32_t pa);
void     pop_mem_helper(GuestContext* ctx, uint32_t pa);

// Complex instruction helpers.
void     pushad_helper(GuestContext* ctx);
void     popad_helper(GuestContext* ctx);
void     enter_helper(GuestContext* ctx, uint32_t alloc_size, uint32_t nesting);
uint32_t xlatb_helper(GuestContext* ctx);
void     iret_helper(GuestContext* ctx);

// String instruction helpers (return updated EFLAGS).
uint32_t string_movs_helper(GuestContext* ctx, uint32_t eflags,
                             uint32_t elem_size, uint32_t rep_mode);
uint32_t string_stos_helper(GuestContext* ctx, uint32_t eflags,
                             uint32_t elem_size, uint32_t rep_mode);
uint32_t string_lods_helper(GuestContext* ctx, uint32_t eflags,
                             uint32_t elem_size, uint32_t rep_mode);
uint32_t string_cmps_helper(GuestContext* ctx, uint32_t eflags,
                             uint32_t elem_size, uint32_t rep_mode);
uint32_t string_scas_helper(GuestContext* ctx, uint32_t eflags,
                             uint32_t elem_size, uint32_t rep_mode);

} // extern "C"
