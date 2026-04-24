// ---------------------------------------------------------------------------
// test_pe_loader.cpp — Unit test for pe_loader.hpp
//
// Constructs a minimal PE image in a temp file, loads it with pe::load_pe(),
// and verifies headers, section content, and export resolution.
// ---------------------------------------------------------------------------

#include "pe_loader.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", msg); } \
} while(0)

// Build a minimal PE image in memory.
// - Image base: 0x00010000
// - 1 section (.text) at RVA 0x1000, containing 16 bytes of code
// - Export directory exporting 2 functions (ordinals 1 and 2)
static void build_test_pe(uint8_t* buf, uint32_t* out_size) {
    memset(buf, 0, 4096);

    // --- DOS Header ---
    pe::DosHeader* dos = reinterpret_cast<pe::DosHeader*>(buf);
    dos->e_magic = pe::DOS_MAGIC;  // 'MZ'
    dos->e_lfanew = 0x80;          // PE signature at offset 0x80

    // --- PE Signature ---
    uint32_t pe_off = 0x80;
    memcpy(buf + pe_off, "\x50\x45\x00\x00", 4); // 'PE\0\0'

    // --- COFF Header ---
    pe::CoffHeader* coff = reinterpret_cast<pe::CoffHeader*>(buf + pe_off + 4);
    coff->machine = pe::MACHINE_I386;
    coff->number_of_sections = 1;
    coff->size_of_optional = sizeof(pe::OptionalHeader32) + 16 * 8; // opt + 16 data dirs

    // --- Optional Header ---
    pe::OptionalHeader32* opt = reinterpret_cast<pe::OptionalHeader32*>(
        buf + pe_off + 4 + sizeof(pe::CoffHeader));
    opt->magic = pe::PE32_MAGIC;
    opt->entry_point_rva = 0x1000;  // entry at section start
    opt->image_base = 0x00010000;
    opt->section_alignment = 0x1000;
    opt->file_alignment = 0x200;
    opt->size_of_image = 0x3000;     // 3 pages: headers + .text + .edata
    opt->size_of_headers = 0x200;
    opt->number_of_rva_and_sizes = 16;

    // Data directory[0] = Export Table at RVA 0x2000, size 0x100
    uint8_t* data_dirs = reinterpret_cast<uint8_t*>(opt) + 0x60;
    uint32_t export_rva = 0x2000;
    uint32_t export_size = 0x100;
    memcpy(data_dirs + 0, &export_rva, 4);
    memcpy(data_dirs + 4, &export_size, 4);

    // --- Section Table ---
    uint32_t sect_offset = pe_off + 4 + sizeof(pe::CoffHeader) + coff->size_of_optional;
    pe::SectionHeader* sect = reinterpret_cast<pe::SectionHeader*>(buf + sect_offset);
    memcpy(sect->name, ".text\0\0\0", 8);
    sect->virtual_size = 0x10;
    sect->virtual_address = 0x1000;
    sect->size_of_raw_data = 0x10;
    sect->pointer_to_raw_data = 0x200;
    sect->characteristics = pe::IMAGE_SCN_CNT_CODE | pe::IMAGE_SCN_MEM_EXECUTE;

    // --- .text section data at file offset 0x200 ---
    // Fill with a recognizable pattern: 0xCC (int3) repeated
    memset(buf + 0x200, 0xCC, 0x10);
    // Put a specific signature at the start
    buf[0x200] = 0x55;  // push ebp
    buf[0x201] = 0x8B;  // mov ebp, esp
    buf[0x202] = 0xEC;

    // --- Export directory at file offset corresponding to RVA 0x2000 ---
    // We need a second section for .edata, OR we embed it in a larger .text.
    // For simplicity: make the file large enough and place export data
    // at offset 0x400 (pretend RVA 0x2000 maps to file offset 0x400).
    // We need a 2nd section header.
    coff->number_of_sections = 2;
    pe::SectionHeader* sect2 = reinterpret_cast<pe::SectionHeader*>(
        buf + sect_offset + sizeof(pe::SectionHeader));
    memcpy(sect2->name, ".edata\0\0", 8);
    sect2->virtual_size = 0x100;
    sect2->virtual_address = 0x2000;
    sect2->size_of_raw_data = 0x100;
    sect2->pointer_to_raw_data = 0x400;
    sect2->characteristics = pe::IMAGE_SCN_CNT_INITIALIZED_DATA | pe::IMAGE_SCN_MEM_READ;

    // Build export directory at file offset 0x400
    pe::ExportDirectory* edir = reinterpret_cast<pe::ExportDirectory*>(buf + 0x400);
    edir->ordinal_base = 1;
    edir->number_of_functions = 2;
    edir->number_of_names = 0;  // ordinal-only exports
    // Function address table at RVA 0x2000 + 0x40
    edir->address_of_functions = 0x2000 + 0x40;

    // Write 2 function RVAs at file offset 0x440
    uint32_t func1_rva = 0x1000;  // ordinal 1 -> RVA 0x1000
    uint32_t func2_rva = 0x1008;  // ordinal 2 -> RVA 0x1008
    memcpy(buf + 0x440, &func1_rva, 4);
    memcpy(buf + 0x444, &func2_rva, 4);

    *out_size = 0x600;  // total file size
}

int main() {
    // ---- Build and write test PE ----
    uint8_t pe_buf[4096];
    uint32_t pe_size = 0;
    build_test_pe(pe_buf, &pe_size);

    const char* tmp_path = "test_pe_tmp.exe";
    FILE* f = fopen(tmp_path, "wb");
    if (!f) { fprintf(stderr, "Cannot create temp file\n"); return 1; }
    fwrite(pe_buf, 1, pe_size, f);
    fclose(f);

    // ---- Allocate guest RAM ----
    constexpr uint32_t RAM_SIZE = 4 * 1024 * 1024; // 4 MB
    auto* ram = static_cast<uint8_t*>(calloc(1, RAM_SIZE));
    if (!ram) { fprintf(stderr, "Cannot allocate RAM\n"); return 1; }

    // ---- Test 1: Load PE ----
    pe::LoadResult res = pe::load_pe(ram, RAM_SIZE, tmp_path);
    CHECK(res.ok, "load_pe returns ok=true");
    CHECK(res.image_base == 0x00010000, "image_base == 0x10000");
    CHECK(res.entry_point == 0x00011000, "entry_point == 0x11000");
    CHECK(res.image_size == 0x3000, "image_size == 0x3000");
    CHECK(res.num_sections == 2, "num_sections == 2");

    // ---- Test 2: Section content ----
    CHECK(ram[0x11000] == 0x55, ".text[0] == 0x55 (push ebp)");
    CHECK(ram[0x11001] == 0x8B, ".text[1] == 0x8B");
    CHECK(ram[0x11002] == 0xEC, ".text[2] == 0xEC (mov ebp,esp)");

    // ---- Test 3: Export resolution ----
    uint32_t addr1 = pe::resolve_export_by_ordinal(ram, res.image_base, 1);
    CHECK(addr1 == 0x00011000, "export ordinal 1 -> 0x11000");

    uint32_t addr2 = pe::resolve_export_by_ordinal(ram, res.image_base, 2);
    CHECK(addr2 == 0x00011008, "export ordinal 2 -> 0x11008");

    // ---- Test 4: Invalid ordinal returns 0 ----
    uint32_t addr_bad = pe::resolve_export_by_ordinal(ram, res.image_base, 999);
    CHECK(addr_bad == 0, "export ordinal 999 -> 0 (invalid)");

    // ---- Test 5: Invalid file returns ok=false ----
    pe::LoadResult bad_res = pe::load_pe(ram, RAM_SIZE, "nonexistent_file.exe");
    CHECK(!bad_res.ok, "nonexistent file returns ok=false");

    // ---- Cleanup ----
    free(ram);
    remove(tmp_path);

    printf("PE loader test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
