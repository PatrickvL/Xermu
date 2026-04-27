#pragma once
// ---------------------------------------------------------------------------
// nboxkrnl_boot.hpp — PE loader, page table setup, and boot mode for nboxkrnl.
//
// Loads the nboxkrnl.exe PE into guest RAM at PA 0x10000 (VA 0x80010000),
// sets up 32-bit non-PAE page tables, configures CPU state, and returns
// the entry EIP for KernelEntry.
// ---------------------------------------------------------------------------

#include "cpu/executor.hpp"
#include "pe_loader.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace nboxkrnl {

// nboxkrnl is linked at VA 0x80010000, which maps to PA 0x10000.
static constexpr uint32_t KERNEL_VA_BASE = 0x80010000u;
static constexpr uint32_t KERNEL_PA_BASE = 0x00010000u;
static constexpr uint32_t KERNEL_VA_OFFSET = KERNEL_VA_BASE - KERNEL_PA_BASE; // 0x80000000

// Page directory at PA 0xF000.
static constexpr uint32_t PAGE_DIR_PA = 0x0000F000u;

// Stack at VA 0x80400000 (PA 0x400000 = 4 MB).
static constexpr uint32_t STACK_VA = 0x80400000u;

// ---------------------------------------------------------------------------
// Set up nboxkrnl-compatible page tables in guest RAM.
//
// Page directory at PA 0xF000:
//   PDE[0x000-0x00F]: Identity-map first 64 MB as 4 MB large pages (0xE3)
//   PDE[0x200-0x20F]: Mirror at VA 0x80000000 → PA 0-64 MB
//   PDE[0x300]:       Self-map → PA 0xF000 (0xF063)
//   All other PDEs:   0 (not present)
// ---------------------------------------------------------------------------

inline void setup_page_tables(uint8_t* ram) {
    memset(ram + PAGE_DIR_PA, 0, 0x1000);
    auto* pd = reinterpret_cast<uint32_t*>(ram + PAGE_DIR_PA);

    // Identity-map: PDE[0..15] → PA 0, 4MB, ..., 60MB
    for (int i = 0; i < 16; ++i) {
        pd[i] = (i * 0x00400000u) | 0xE3u;
    }

    // Kernel mirror: PDE[0x200..0x20F] → same physical addresses
    for (int i = 0; i < 16; ++i) {
        pd[0x200 + i] = (i * 0x00400000u) | 0xE3u;
    }

    // Self-map: PDE[0x300] → page directory
    pd[0x300] = PAGE_DIR_PA | 0x63u;
}

// ---------------------------------------------------------------------------
// Configure CPU state for nboxkrnl entry.
// ---------------------------------------------------------------------------

inline void setup_cpu_state(Executor& exec) {
    auto& ctx = exec.ctx;

    // Control registers.
    ctx.cr0 = 0x80000021u;   // PE + NE + PG
    ctx.cr3 = PAGE_DIR_PA;
    ctx.cr4 = 0x00000610u;   // PSE + OSFXSR + OSXMMEXCPT

    // Segments: flat model.  nboxkrnl will reload these from its own GDT.
    ctx.cs_sel = 0x08;
    ctx.ds_sel = 0x10;
    ctx.ss_sel = 0x10;
    ctx.es_sel = 0x10;
    ctx.gs_sel = 0x10;
    ctx.fs_sel = 0x18;     // FS = KPCR selector (kernel will set up)

    // Segment bases: FS/GS zero (kernel will set FS base for KPCR).
    ctx.fs_base = 0;
    ctx.gs_base = 0;

    // Stack.
    ctx.gp[GP_ESP] = STACK_VA;
    ctx.gp[GP_EBP] = STACK_VA;

    // Flags.
    ctx.eflags = 0x00000002u;  // Reserved bit set, IF=0 (interrupts disabled)
}

// ---------------------------------------------------------------------------
// Load nboxkrnl PE into guest RAM.
//
// nboxkrnl is a 32-bit Native PE with ImageBase = 0x80010000.
// The PE loader expects to write to ram[image_base], but 0x80010000 is
// beyond the 128 MB RAM.  So we load into ram[0x10000] by passing a
// modified RAM pointer: ram - 0x80000000.  This way
//   ram_base[0x80010000] = ram[-0x80000000 + 0x80010000] = ram[0x10000]
//
// WARNING: We must ensure the PE loader doesn't access any address below
// image_base (0x80010000) because those would be negative offsets.  The
// PE loader zeroes ram[image_base .. image_base+image_size], copies headers,
// and copies sections.  All accesses are within [image_base, image_base+image_size).
// ---------------------------------------------------------------------------

struct BootConfig {
    const char* kernel_pe_path;   // Path to nboxkrnl.exe
    const char* input_path;       // XBE to launch
    const uint8_t* keys;          // 32 bytes: 16B EEPROM + 16B cert (may be null)
};

inline uint32_t boot_nboxkrnl(Executor& exec, const BootConfig& cfg) {
    // Load the PE file to get header info.
    FILE* f = fopen(cfg.kernel_pe_path, "rb");
    if (!f) {
        fprintf(stderr, "[nboxkrnl] cannot open kernel PE: %s\n", cfg.kernel_pe_path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    auto* file_buf = static_cast<uint8_t*>(malloc((size_t)file_size));
    if (!file_buf) { fclose(f); return 0; }
    if (fread(file_buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(file_buf); fclose(f); return 0;
    }
    fclose(f);

    // Parse PE headers manually (same as pe_loader but we load to PA offset).
    auto* dos = reinterpret_cast<const pe::DosHeader*>(file_buf);
    if (dos->e_magic != pe::DOS_MAGIC) {
        fprintf(stderr, "[nboxkrnl] invalid DOS magic\n");
        free(file_buf); return 0;
    }

    uint32_t pe_offset = dos->e_lfanew;
    uint32_t pe_sig;
    memcpy(&pe_sig, file_buf + pe_offset, 4);
    if (pe_sig != pe::PE_SIG) {
        fprintf(stderr, "[nboxkrnl] invalid PE signature\n");
        free(file_buf); return 0;
    }

    auto* coff = reinterpret_cast<const pe::CoffHeader*>(file_buf + pe_offset + 4);
    if (coff->machine != pe::MACHINE_I386) {
        fprintf(stderr, "[nboxkrnl] not an i386 PE\n");
        free(file_buf); return 0;
    }

    auto* opt = reinterpret_cast<const pe::OptionalHeader32*>(
        file_buf + pe_offset + 4 + sizeof(pe::CoffHeader));
    if (opt->magic != pe::PE32_MAGIC) {
        fprintf(stderr, "[nboxkrnl] not PE32\n");
        free(file_buf); return 0;
    }

    uint32_t image_base = opt->image_base;
    uint32_t image_size = opt->size_of_image;
    uint32_t hdr_size   = opt->size_of_headers;

    if (image_base < KERNEL_VA_BASE) {
        fprintf(stderr, "[nboxkrnl] unexpected image base 0x%08X (expected >= 0x%08X)\n",
                image_base, KERNEL_VA_BASE);
        free(file_buf); return 0;
    }

    // Physical address where the image will be loaded.
    uint32_t load_pa = image_base - KERNEL_VA_OFFSET;
    uint32_t end_pa  = load_pa + image_size;

    if (end_pa > GUEST_RAM_SIZE) {
        fprintf(stderr, "[nboxkrnl] image too large: PA 0x%08X + 0x%08X > 0x%08X\n",
                load_pa, image_size, GUEST_RAM_SIZE);
        free(file_buf); return 0;
    }

    fprintf(stderr, "[nboxkrnl] loading PE at PA 0x%08X (VA 0x%08X), size 0x%08X\n",
            load_pa, image_base, image_size);

    // Zero-fill the image region in guest RAM.
    memset(exec.ram + load_pa, 0, image_size);

    // Copy PE headers.
    if (hdr_size > (uint32_t)file_size) hdr_size = (uint32_t)file_size;
    memcpy(exec.ram + load_pa, file_buf, hdr_size);

    // Copy sections.
    uint32_t sect_offset = pe_offset + 4 + sizeof(pe::CoffHeader) + coff->size_of_optional;
    uint16_t num_sections = coff->number_of_sections;

    for (uint16_t i = 0; i < num_sections; ++i) {
        uint32_t entry_off = sect_offset + i * sizeof(pe::SectionHeader);
        if (entry_off + sizeof(pe::SectionHeader) > (uint32_t)file_size) break;

        auto* sect = reinterpret_cast<const pe::SectionHeader*>(file_buf + entry_off);
        uint32_t va    = sect->virtual_address;
        uint32_t vsize = sect->virtual_size;
        uint32_t roff  = sect->pointer_to_raw_data;
        uint32_t rsize = sect->size_of_raw_data;

        // Destination PA.
        uint32_t dest_pa = load_pa + va;
        if (dest_pa + vsize > GUEST_RAM_SIZE) continue;

        uint32_t copy_size = (rsize < vsize) ? rsize : vsize;
        if (roff + copy_size > (uint32_t)file_size) {
            copy_size = ((uint32_t)file_size > roff) ? (uint32_t)file_size - roff : 0;
        }
        if (copy_size > 0) {
            memcpy(exec.ram + dest_pa, file_buf + roff, copy_size);
        }

        char name[9] = {};
        memcpy(name, sect->name, 8);
        fprintf(stderr, "[nboxkrnl]   section %-8s: VA 0x%08X PA 0x%08X size 0x%08X\n",
                name, image_base + va, dest_pa, vsize);
    }

    free(file_buf);

    // --- Set up page tables ---
    setup_page_tables(exec.ram);

    // --- Configure CPU state ---
    setup_cpu_state(exec);

    // --- Place keys on stack ---
    // nboxkrnl's KernelEntry expects 32 bytes at [ESP]:
    //   Bytes 0-15:  EEPROM key
    //   Bytes 16-31: Certificate key
    uint8_t keys[32] = {};
    if (cfg.keys) {
        memcpy(keys, cfg.keys, 32);
    }
    // Write keys to guest at stack VA (which maps to PA via our page tables).
    uint32_t stack_pa = STACK_VA - KERNEL_VA_OFFSET;  // 0x400000
    memcpy(exec.ram + stack_pa, keys, 32);

    // Entry point.
    uint32_t entry_eip = image_base + opt->entry_point_rva;
    fprintf(stderr, "[nboxkrnl] entry: EIP=0x%08X ESP=0x%08X CR3=0x%08X\n",
            entry_eip, STACK_VA, PAGE_DIR_PA);

    return entry_eip;
}

} // namespace nboxkrnl
