#pragma once
// ---------------------------------------------------------------------------
// pe_loader.hpp — Windows PE (Portable Executable) loader for x86-32 images.
//
// Loads a standard Win32 PE file (e.g. xboxkrnl.exe) into guest RAM.
// Parses the DOS stub, PE signature, COFF + optional headers, and section
// table; copies each section's raw data to its virtual address in guest
// memory.  No relocations are applied (images are loaded at their preferred
// base).  No imports are resolved — the caller handles that.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace pe {

// ============================== PE Structures ==============================

#pragma pack(push, 1)

struct DosHeader {
    uint16_t e_magic;       // 0x00: 'MZ' = 0x5A4D
    uint8_t  _pad[58];      // 0x02: DOS stub (unused fields)
    uint32_t e_lfanew;      // 0x3C: file offset to PE signature
};

struct CoffHeader {
    uint16_t machine;               // 0x00: 0x014C = i386
    uint16_t number_of_sections;    // 0x02
    uint32_t time_date_stamp;       // 0x04
    uint32_t pointer_to_symtab;     // 0x08
    uint32_t number_of_symbols;     // 0x0C
    uint16_t size_of_optional;      // 0x10
    uint16_t characteristics;       // 0x12
};

struct OptionalHeader32 {
    uint16_t magic;                 // 0x00: 0x010B = PE32
    uint8_t  major_linker;          // 0x02
    uint8_t  minor_linker;          // 0x03
    uint32_t size_of_code;          // 0x04
    uint32_t size_of_init_data;     // 0x08
    uint32_t size_of_uninit_data;   // 0x0C
    uint32_t entry_point_rva;       // 0x10: RVA of entry point
    uint32_t base_of_code;          // 0x14
    uint32_t base_of_data;          // 0x18
    uint32_t image_base;            // 0x1C: preferred load address
    uint32_t section_alignment;     // 0x20
    uint32_t file_alignment;        // 0x24
    uint16_t major_os_version;      // 0x28
    uint16_t minor_os_version;      // 0x2A
    uint16_t major_image_version;   // 0x2C
    uint16_t minor_image_version;   // 0x2E
    uint16_t major_subsys_version;  // 0x30
    uint16_t minor_subsys_version;  // 0x32
    uint32_t win32_version_value;   // 0x34
    uint32_t size_of_image;         // 0x38
    uint32_t size_of_headers;       // 0x3C
    uint32_t checksum;              // 0x40
    uint16_t subsystem;             // 0x44
    uint16_t dll_characteristics;   // 0x46
    uint32_t size_of_stack_reserve; // 0x48
    uint32_t size_of_stack_commit;  // 0x4C
    uint32_t size_of_heap_reserve;  // 0x50
    uint32_t size_of_heap_commit;   // 0x54
    uint32_t loader_flags;          // 0x58
    uint32_t number_of_rva_and_sizes; // 0x5C
    // Data directory entries follow (16 entries × 8 bytes each)
};

struct SectionHeader {
    char     name[8];               // 0x00: section name (null-padded)
    uint32_t virtual_size;          // 0x08: size in memory
    uint32_t virtual_address;       // 0x0C: RVA
    uint32_t size_of_raw_data;      // 0x10: size on disk
    uint32_t pointer_to_raw_data;   // 0x14: file offset
    uint32_t pointer_to_relocs;     // 0x18
    uint32_t pointer_to_line_nums;  // 0x1C
    uint16_t number_of_relocs;      // 0x20
    uint16_t number_of_line_nums;   // 0x22
    uint32_t characteristics;       // 0x24
};

#pragma pack(pop)

static constexpr uint16_t DOS_MAGIC  = 0x5A4D; // 'MZ'
static constexpr uint32_t PE_SIG     = 0x00004550; // 'PE\0\0'
static constexpr uint16_t MACHINE_I386 = 0x014C;
static constexpr uint16_t PE32_MAGIC   = 0x010B;

// Section characteristics flags
static constexpr uint32_t IMAGE_SCN_CNT_CODE               = 0x00000020;
static constexpr uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA   = 0x00000040;
static constexpr uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
static constexpr uint32_t IMAGE_SCN_MEM_EXECUTE            = 0x20000000;
static constexpr uint32_t IMAGE_SCN_MEM_READ               = 0x40000000;
static constexpr uint32_t IMAGE_SCN_MEM_WRITE              = 0x80000000;

// ============================== Load Result ================================

struct LoadResult {
    bool     ok;
    uint32_t entry_point;       // VA of entry point
    uint32_t image_base;        // preferred image base (VA)
    uint32_t image_size;        // total size of image in memory
    uint16_t num_sections;      // number of sections loaded
};

// ============================= PE Loader ===================================

// Load a PE image from file into guest RAM.
//
//   ram       — pointer to guest RAM buffer
//   ram_size  — size of guest RAM in bytes
//   path      — path to the PE file on the host filesystem
//
// The image is loaded at its preferred base address (image_base).
// Sections are mapped according to their virtual addresses.
// The PE headers themselves are copied to image_base.
// No relocations or imports are processed.
//
// Returns a LoadResult with ok=true on success.
inline LoadResult load_pe(uint8_t* ram, uint32_t ram_size, const char* path) {
    LoadResult result{};

    FILE* f = fopen(path, "rb");
    if (!f) return result;

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < (long)sizeof(DosHeader)) { fclose(f); return result; }

    // Read entire file into a temporary buffer
    auto* buf = static_cast<uint8_t*>(malloc((size_t)file_size));
    if (!buf) { fclose(f); return result; }
    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return result;
    }
    fclose(f);

    // Parse DOS header
    auto* dos = reinterpret_cast<const DosHeader*>(buf);
    if (dos->e_magic != DOS_MAGIC) { free(buf); return result; }

    uint32_t pe_offset = dos->e_lfanew;
    if (pe_offset + sizeof(uint32_t) + sizeof(CoffHeader) > (uint32_t)file_size) {
        free(buf); return result;
    }

    // Check PE signature
    uint32_t pe_sig;
    memcpy(&pe_sig, buf + pe_offset, 4);
    if (pe_sig != PE_SIG) { free(buf); return result; }

    // Parse COFF header
    auto* coff = reinterpret_cast<const CoffHeader*>(buf + pe_offset + 4);
    if (coff->machine != MACHINE_I386) { free(buf); return result; }
    if (coff->size_of_optional < sizeof(OptionalHeader32)) { free(buf); return result; }

    // Parse Optional header
    auto* opt = reinterpret_cast<const OptionalHeader32*>(
        buf + pe_offset + 4 + sizeof(CoffHeader));
    if (opt->magic != PE32_MAGIC) { free(buf); return result; }

    uint32_t image_base = opt->image_base;
    uint32_t image_size = opt->size_of_image;
    uint32_t hdr_size   = opt->size_of_headers;

    // Bounds check: image must fit within guest RAM
    if (image_base + image_size > ram_size) { free(buf); return result; }

    // Zero-fill the entire image region in guest RAM
    memset(ram + image_base, 0, image_size);

    // Copy PE headers to image base
    if (hdr_size > (uint32_t)file_size) hdr_size = (uint32_t)file_size;
    memcpy(ram + image_base, buf, hdr_size);

    // Parse section table (immediately after optional header)
    uint32_t sect_offset = pe_offset + 4 + sizeof(CoffHeader) + coff->size_of_optional;
    uint16_t num_sections = coff->number_of_sections;

    for (uint16_t i = 0; i < num_sections; ++i) {
        uint32_t entry_off = sect_offset + i * sizeof(SectionHeader);
        if (entry_off + sizeof(SectionHeader) > (uint32_t)file_size) break;

        auto* sect = reinterpret_cast<const SectionHeader*>(buf + entry_off);

        uint32_t va   = sect->virtual_address;
        uint32_t vsize = sect->virtual_size;
        uint32_t roff  = sect->pointer_to_raw_data;
        uint32_t rsize = sect->size_of_raw_data;

        // Bounds check for destination
        if (image_base + va + vsize > ram_size) continue;

        // Copy raw data (if any)
        uint32_t copy_size = (rsize < vsize) ? rsize : vsize;
        if (roff + copy_size > (uint32_t)file_size) {
            copy_size = ((uint32_t)file_size > roff) ? (uint32_t)file_size - roff : 0;
        }
        if (copy_size > 0) {
            memcpy(ram + image_base + va, buf + roff, copy_size);
        }
        // Remainder (vsize - copy_size) is already zeroed from the memset above
    }

    free(buf);

    result.ok           = true;
    result.entry_point  = image_base + opt->entry_point_rva;
    result.image_base   = image_base;
    result.image_size   = image_size;
    result.num_sections = num_sections;
    return result;
}

// ========================= Export Table Parsing ============================
//
// For xboxkrnl.exe, we need to find the export directory to resolve kernel
// function addresses by ordinal.

struct ExportDirectory {
    uint32_t characteristics;       // 0x00
    uint32_t time_date_stamp;       // 0x04
    uint16_t major_version;         // 0x08
    uint16_t minor_version;         // 0x0A
    uint32_t name_rva;              // 0x0C: RVA of DLL name
    uint32_t ordinal_base;          // 0x10: starting ordinal (usually 1)
    uint32_t number_of_functions;   // 0x14
    uint32_t number_of_names;       // 0x18
    uint32_t address_of_functions;  // 0x1C: RVA of function address table
    uint32_t address_of_names;      // 0x20: RVA of name pointer table
    uint32_t address_of_name_ords;  // 0x24: RVA of ordinal table
};

// Look up a kernel export address by ordinal from a loaded PE image.
//
//   ram        — guest RAM
//   image_base — VA where the image was loaded
//   ordinal    — export ordinal to resolve
//
// Returns the VA of the exported function, or 0 on failure.
inline uint32_t resolve_export_by_ordinal(const uint8_t* ram, uint32_t image_base,
                                           uint32_t ordinal) {
    // Re-parse PE headers from guest RAM to find the export directory
    auto* dos = reinterpret_cast<const DosHeader*>(ram + image_base);
    if (dos->e_magic != DOS_MAGIC) return 0;

    uint32_t pe_off = dos->e_lfanew;
    uint32_t pe_sig;
    memcpy(&pe_sig, ram + image_base + pe_off, 4);
    if (pe_sig != PE_SIG) return 0;

    auto* coff = reinterpret_cast<const CoffHeader*>(ram + image_base + pe_off + 4);
    auto* opt = reinterpret_cast<const OptionalHeader32*>(
        ram + image_base + pe_off + 4 + sizeof(CoffHeader));

    if (opt->number_of_rva_and_sizes == 0) return 0;

    // Data directory[0] = Export Table: 8 bytes at offset 0x60 of optional header
    uint32_t export_rva, export_size;
    memcpy(&export_rva,  reinterpret_cast<const uint8_t*>(opt) + 0x60, 4);
    memcpy(&export_size, reinterpret_cast<const uint8_t*>(opt) + 0x64, 4);
    if (export_rva == 0 || export_size == 0) return 0;

    auto* edir = reinterpret_cast<const ExportDirectory*>(
        ram + image_base + export_rva);

    uint32_t index = ordinal - edir->ordinal_base;
    if (index >= edir->number_of_functions) return 0;

    // Read function RVA from the address table
    uint32_t func_rva;
    memcpy(&func_rva,
           ram + image_base + edir->address_of_functions + index * 4, 4);

    return image_base + func_rva;
}

} // namespace pe
