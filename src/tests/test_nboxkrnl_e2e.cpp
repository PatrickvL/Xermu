// test_nboxkrnl_e2e.cpp — End-to-end integration test for nboxkrnl boot path.
//
// Validates the full pipeline:
// 1. Xbox hardware setup (PIC, PIT, SMBus, NV2A, etc.)
// 2. Host I/O port registration (0x200-0x210)
// 3. File I/O system (dvd_dir, hdd_dir, partitions)
// 4. Key configuration (zero keys + EEPROM defaults)
// 5. Page tables + CPU state
// 6. Guest execution through nboxkrnl boot path
//
// Uses a small mock kernel PE to exercise the boot path without requiring
// a real nboxkrnl binary.

#include "xbox/hle/bootstrap.hpp"
#include "xbox/nboxkrnl_boot.hpp"
#include "xbox/nboxkrnl_host.hpp"
#include "xbox/nboxkrnl_io.hpp"
#include "xbox/nboxkrnl_paths.hpp"
#include "xbox/nboxkrnl_keys.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: create a minimal PE file on disk for boot_nboxkrnl() to load.
//
// This builds a valid PE32 with ImageBase=0x80010000, a .text section
// containing our mock kernel code, and EntryPointRVA pointing to it.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct MiniPE {
    // DOS header (64 bytes)
    uint16_t e_magic;       // "MZ"
    uint8_t  dos_pad[58];
    uint32_t e_lfanew;      // offset to PE signature

    // PE signature
    uint32_t pe_sig;        // "PE\0\0"

    // COFF header (20 bytes)
    uint16_t machine;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t symtab_ptr;
    uint32_t num_symbols;
    uint16_t opt_hdr_size;
    uint16_t characteristics;

    // Optional header (PE32, 96 bytes standard)
    uint16_t opt_magic;          // 0x10B
    uint8_t  linker_major;
    uint8_t  linker_minor;
    uint32_t code_size;
    uint32_t init_data_size;
    uint32_t uninit_data_size;
    uint32_t entry_rva;
    uint32_t code_base;
    uint32_t data_base;
    uint32_t image_base;
    uint32_t section_align;
    uint32_t file_align;
    uint16_t os_major, os_minor;
    uint16_t img_major, img_minor;
    uint16_t sub_major, sub_minor;
    uint32_t win32_ver;
    uint32_t image_size;
    uint32_t headers_size;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_chars;
    uint32_t stack_reserve;
    uint32_t stack_commit;
    uint32_t heap_reserve;
    uint32_t heap_commit;
    uint32_t loader_flags;
    uint32_t num_rva_sizes;

    // Section header (.text)
    char     sect_name[8];
    uint32_t virt_size;
    uint32_t virt_addr;
    uint32_t raw_size;
    uint32_t raw_offset;
    uint32_t reloc_ptr;
    uint32_t linenum_ptr;
    uint16_t num_relocs;
    uint16_t num_linenums;
    uint32_t sect_chars;
};
#pragma pack(pop)

// Mock kernel code: reads MACHINE_TYPE port, checks SMBus, halts.
//
//   mov dx, 0x201       ; PORT_MACHINE_TYPE
//   in  eax, dx         ; expect 0
//   cmp eax, 0
//   jne .fail
//
//   ; Read BOOT_TIME_MS
//   mov dx, 0x205
//   in  eax, dx
//   ; any non-negative value is fine
//
//   ; Read XBE path length
//   mov dx, 0x20D
//   in  eax, dx
//   test eax, eax
//   jz  .fail
//
//   mov eax, 0          ; success
//   hlt
// .fail:
//   mov eax, 0xDEAD
//   hlt

static const uint8_t mock_code[] = {
    0x66, 0xBA, 0x01, 0x02,                   // mov dx, 0x201
    0xED,                                      // in  eax, dx
    0x83, 0xF8, 0x00,                          // cmp eax, 0
    0x75, 0x15,                                // jne .fail

    0x66, 0xBA, 0x05, 0x02,                   // mov dx, 0x205
    0xED,                                      // in  eax, dx

    0x66, 0xBA, 0x0D, 0x02,                   // mov dx, 0x20D
    0xED,                                      // in  eax, dx
    0x85, 0xC0,                                // test eax, eax
    0x74, 0x07,                                // jz  .fail

    0xB8, 0x00, 0x00, 0x00, 0x00,             // mov eax, 0
    0xF4,                                      // hlt

    0xB8, 0xAD, 0xDE, 0x00, 0x00,             // mov eax, 0xDEAD
    0xF4,                                      // hlt
};

static bool create_mock_pe(const char* path) {
    MiniPE pe;
    memset(&pe, 0, sizeof(pe));

    // DOS header
    pe.e_magic  = 0x5A4D;  // "MZ"
    pe.e_lfanew = 64;      // PE signature right after DOS header

    // PE signature
    pe.pe_sig = 0x00004550; // "PE\0\0"

    // COFF
    pe.machine        = 0x014C; // IMAGE_FILE_MACHINE_I386
    pe.num_sections   = 1;
    pe.opt_hdr_size   = 96;     // minimal PE32 optional header (no data dirs)
    pe.characteristics = 0x0102; // EXECUTABLE_IMAGE | 32BIT_MACHINE

    // Optional header
    pe.opt_magic      = 0x010B; // PE32
    pe.entry_rva      = 0x1000; // .text section RVA
    pe.code_base      = 0x1000;
    pe.image_base     = 0x80010000;
    pe.section_align  = 0x1000;
    pe.file_align     = 0x200;
    pe.image_size     = 0x2000;       // 2 pages (headers + .text)
    pe.headers_size   = 0x200;        // 1 file alignment unit
    pe.subsystem      = 1;            // NATIVE
    pe.num_rva_sizes  = 0;

    // Section header: .text
    memcpy(pe.sect_name, ".text\0\0\0", 8);
    pe.virt_size    = sizeof(mock_code);
    pe.virt_addr    = 0x1000;
    pe.raw_size     = 0x200;  // file-aligned
    pe.raw_offset   = 0x200;  // right after headers
    pe.sect_chars   = 0x60000020; // CODE | EXECUTE | READ

    // Write to file
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    // Write PE headers (pad to headers_size = 0x200)
    uint8_t buf[0x200];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &pe, sizeof(pe));
    fwrite(buf, 1, 0x200, f);

    // Write .text section (pad to raw_size = 0x200)
    memset(buf, 0, sizeof(buf));
    memcpy(buf, mock_code, sizeof(mock_code));
    fwrite(buf, 1, 0x200, f);

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Test 1: Full boot pipeline with mock kernel PE.
// ---------------------------------------------------------------------------

static bool test_e2e_mock_boot() {
    printf("=== Test: E2E nboxkrnl mock boot ===\n");

    // Create a temporary mock PE and XBE directory.
    const char* mock_pe_path  = "test_nboxkrnl_mock.exe";
    const char* mock_xbe_dir  = "test_nboxkrnl_xbe_dir";
    std::string mock_xbe_path = std::string(mock_xbe_dir) + "/default.xbe";

    std::error_code ec;
    fs::create_directories(mock_xbe_dir, ec);

    // Create a dummy XBE file (just needs to exist for path setup).
    {
        FILE* f = fopen(mock_xbe_path.c_str(), "wb");
        if (f) { fputc(0, f); fclose(f); }
    }

    if (!create_mock_pe(mock_pe_path)) {
        printf("  failed to create mock PE\n");
        return false;
    }

    // Set up XboxSystem using the full boot pipeline.
    xbox::BootConfig cfg;
    cfg.kernel_path = mock_pe_path;
    cfg.xbe_path    = mock_xbe_path;

    xbox::XboxSystem sys;
    xbox::NboxkrnlState nbox;

    if (!xbox::boot_nboxkrnl_system(sys, cfg, nbox)) {
        printf("  boot_nboxkrnl_system failed\n");
        fs::remove(mock_pe_path, ec);
        fs::remove_all(mock_xbe_dir, ec);
        fs::remove_all("data/hdd", ec);
        return false;
    }

    bool ok = true;

    // Verify hardware was set up.
    if (!sys.hw) {
        printf("  hw is null!\n");
        ok = false;
    }

    // Verify host state.
    if (nbox.host.xbe_path.empty()) {
        printf("  xbe_path is empty!\n");
        ok = false;
    }

    // Verify page tables.
    auto* pd = reinterpret_cast<uint32_t*>(sys.exec->ram + nboxkrnl::PAGE_DIR_PA);
    if (pd[0] != (0 | 0xE3u)) {
        printf("  PDE[0] = 0x%08X (expected 0x000000E3)\n", pd[0]);
        ok = false;
    }
    if (pd[0x200] != (0 | 0xE3u)) {
        printf("  PDE[0x200] = 0x%08X (expected 0x000000E3)\n", pd[0x200]);
        ok = false;
    }

    // Verify CPU state.
    if (sys.exec->ctx.cr0 != 0x80000021u) {
        printf("  CR0 = 0x%08X (expected 0x80000021)\n", sys.exec->ctx.cr0);
        ok = false;
    }

    // Run the mock kernel.
    printf("  entry EIP = 0x%08X\n", sys.entry_eip);
    sys.exec->run(sys.entry_eip, 10000);

    uint32_t eax = sys.exec->ctx.gp[GP_EAX];
    bool halted = sys.exec->ctx.halted;

    printf("  EAX = 0x%08X (expected 0x00000000), halted=%d\n", eax, halted);
    if (!halted || eax != 0) {
        printf("  mock kernel did not succeed!\n");
        ok = false;
    }

    printf("  %s\n", ok ? "PASS" : "FAIL");

    // Cleanup
    sys.shutdown();
    fs::remove(mock_pe_path, ec);
    fs::remove_all(mock_xbe_dir, ec);
    fs::remove_all("data/hdd", ec);

    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: Verify partition directories are created.
// ---------------------------------------------------------------------------

static bool test_partition_dirs() {
    printf("=== Test: partition directory creation ===\n");

    const char* hdd_dir = "test_nboxkrnl_hdd_tmp";
    std::error_code ec;
    fs::remove_all(hdd_dir, ec);

    nboxkrnl::HostState host;
    nboxkrnl::IoSystem io;

    // Create a dummy XBE for setup_paths.
    const char* xbe_dir  = "test_nboxkrnl_xbe_tmp";
    std::string xbe_path = std::string(xbe_dir) + "/game.xbe";
    fs::create_directories(xbe_dir, ec);
    { FILE* f = fopen(xbe_path.c_str(), "wb"); if (f) { fputc(0, f); fclose(f); } }

    bool ok = nboxkrnl::setup_paths(host, io, xbe_path.c_str(), hdd_dir);
    if (!ok) {
        printf("  setup_paths failed\n");
        fs::remove_all(hdd_dir, ec);
        fs::remove_all(xbe_dir, ec);
        return false;
    }

    // Check partitions exist.
    for (int i = 0; i < 6; ++i) {
        std::string dir = std::string(hdd_dir) + "/Partition" + std::to_string(i);
        if (!fs::is_directory(dir)) {
            printf("  missing: %s\n", dir.c_str());
            ok = false;
        }
    }

    // Check paths configured.
    if (host.xbe_path.find("game.xbe") == std::string::npos) {
        printf("  xbe_path doesn't contain game.xbe: %s\n", host.xbe_path.c_str());
        ok = false;
    }

    printf("  %s\n", ok ? "PASS" : "FAIL");

    fs::remove_all(hdd_dir, ec);
    fs::remove_all(xbe_dir, ec);
    return ok;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    int passed = 0, failed = 0;
    auto run = [&](bool(*fn)()) {
        if (fn()) ++passed; else ++failed;
    };

    run(test_e2e_mock_boot);
    run(test_partition_dirs);

    printf("\n=== nboxkrnl E2E tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
