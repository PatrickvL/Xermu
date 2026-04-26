#pragma once
// ---------------------------------------------------------------------------
// bootstrap.hpp — Xbox system bootstrap (shared between GUI and test runner).
//
// Provides XBE loading, BIOS boot, HLE kernel setup, and KPCR initialisation.
// Used by both the interactive emulator (main.cpp) and the headless test
// runner (test_runner.cpp).
// ---------------------------------------------------------------------------

#include "cpu/executor.hpp"
#include "xbox/xbox.hpp"
#include "xbox/pe_loader.hpp"
#include "xbox/hle/hle_kernel.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <functional>

namespace xbox {

// ---------------------------------------------------------------------------
// BootConfig — describes what to boot and how.
// ---------------------------------------------------------------------------
struct BootConfig {
    std::string xbe_path;          // XBE file to load (HLE or LLE-kernel mode)
    std::string bios_path;         // BIOS image for LLE boot
    std::string mcpx_path;         // optional MCPX ROM for LLE boot
    std::string xiso_path;         // XISO image to mount as D:
    std::string kernel_path;       // xboxkrnl.exe for LLE-kernel mode
    std::string rc4_key_path;      // 16-byte RC4 key file for 2BL decryption
    std::string dump_kernel_path;  // output path for dumped kernel image

    bool is_hle()  const { return !xbe_path.empty() && kernel_path.empty(); }
    bool is_lle()  const { return !bios_path.empty(); }
    bool is_lle_kernel() const { return !xbe_path.empty() && !kernel_path.empty(); }
};

// ---------------------------------------------------------------------------
// XboxSystem — owns the executor, hardware, and HLE heap.
// ---------------------------------------------------------------------------
struct XboxSystem {
    std::unique_ptr<Executor> exec;
    XboxHardware*             hw       = nullptr;
    xbe::XbeHeap              hle_heap;
    xbe::XbeInfo              xbe_info {};
    bool                      running  = false;
    uint32_t                  entry_eip = 0;

    XboxSystem() : exec(std::make_unique<Executor>()) {}

    ~XboxSystem() { shutdown(); }

    // Non-copyable, movable
    XboxSystem(const XboxSystem&) = delete;
    XboxSystem& operator=(const XboxSystem&) = delete;
    XboxSystem(XboxSystem&&) = default;
    XboxSystem& operator=(XboxSystem&&) = default;

    void shutdown() {
        if (exec) exec->destroy();
        delete hw;
        hw = nullptr;
        running = false;
    }
};

// ---------------------------------------------------------------------------
// read_file_to_vec — load a host file into a byte vector.
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> read_file_to_vec(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return {}; }
    std::vector<uint8_t> buf(sz);
    if (fread(buf.data(), 1, sz, f) != (size_t)sz) { fclose(f); return {}; }
    fclose(f);
    return buf;
}

// ---------------------------------------------------------------------------
// setup_kpcr — initialise a minimal KPCR + KTHREAD at the given FS base.
// ---------------------------------------------------------------------------
inline void setup_kpcr(Executor& exec, uint32_t fs_base, uint32_t stack_top) {
    uint32_t kthread_base = fs_base + 0x1000;
    memset(exec.ram + fs_base, 0, 0x2000);

    // KPCR.NtTib.ExceptionList = -1
    uint32_t neg1 = 0xFFFFFFFF;
    memcpy(exec.ram + fs_base + 0x00, &neg1, 4);
    // KPCR.NtTib.StackBase
    memcpy(exec.ram + fs_base + 0x04, &stack_top, 4);
    // KPCR.NtTib.StackLimit
    uint32_t stack_limit = stack_top - 0x10000;
    memcpy(exec.ram + fs_base + 0x08, &stack_limit, 4);
    // KPCR.NtTib.Self = fs_base
    memcpy(exec.ram + fs_base + 0x18, &fs_base, 4);
    // KPCR.SelfPcr = fs_base
    memcpy(exec.ram + fs_base + 0x1C, &fs_base, 4);
    // KPCR.Prcb (offset 0x20) — points into KPCR
    uint32_t prcb = fs_base + 0x28;
    memcpy(exec.ram + fs_base + 0x20, &prcb, 4);
    // KPCR.CurrentThread (offset 0x28)
    memcpy(exec.ram + fs_base + 0x28, &kthread_base, 4);

    // KTHREAD.TlsData (offset 0x28)
    uint32_t tls_data = kthread_base + 0x200;
    memcpy(exec.ram + kthread_base + 0x28, &tls_data, 4);
    memset(exec.ram + kthread_base + 0x200, 0, 0x100);
}

// ---------------------------------------------------------------------------
// boot_hle — load an XBE and set up HLE stubs.
// Returns false on error; fills sys.entry_eip on success.
// ---------------------------------------------------------------------------
inline bool boot_hle(XboxSystem& sys, const BootConfig& cfg,
                     std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };

    sys.hw = xbox_setup(*sys.exec);

    // Load XBE file.
    auto xbe_data = read_file_to_vec(cfg.xbe_path);
    if (xbe_data.empty()) {
        say("[boot] Failed to read XBE file");
        return false;
    }

    // Copy raw file into RAM at the load PA for load_xbe to parse.
    uint32_t load_pa = 0x1000;
    sys.exec->load_guest(load_pa, xbe_data.data(), xbe_data.size());

    // Parse XBE headers and map sections.
    sys.xbe_info = xbe::load_xbe(sys.exec->ram, xbe_data.data(), xbe_data.size());
    if (!sys.xbe_info.valid) {
        say("[boot] XBE parsing failed");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[boot] XBE entry=0x%08X sections=%u",
             sys.xbe_info.entry_point, sys.xbe_info.num_sections);
    say(msg);

    // Stack at 11 MB (above HLE stubs + XBE image)
    constexpr uint32_t STACK_TOP = 0x00B0'0000u;
    constexpr uint32_t SENTINEL  = 0xFFFF'FFFFu;
    sys.exec->ctx.gp[GP_ESP] = STACK_TOP;
    memcpy(sys.exec->ram + STACK_TOP, &SENTINEL, 4);
    sys.exec->ctx.eflags = 0x0000'0202;

    // KPCR at 13 MB
    constexpr uint32_t FS_BASE = 0x00D0'0000u;
    sys.exec->ctx.fs_base = FS_BASE;
    sys.exec->ctx.gs_base = 0;
    setup_kpcr(*sys.exec, FS_BASE, STACK_TOP);

    // Write HLE stubs and install handler.
    xbe::write_hle_stubs(sys.exec->ram);
    sys.hle_heap.reset();
    sys.hle_heap.set_xbe_path(cfg.xbe_path);

    // If an XISO is provided, mount it as D:
    if (!cfg.xiso_path.empty()) {
        // Extract directory of the XISO for basic mount
        size_t sep = cfg.xiso_path.find_last_of("/\\");
        std::string xiso_dir = (sep != std::string::npos)
            ? cfg.xiso_path.substr(0, sep) : ".";
        sys.hle_heap.mounts.push_back({"\\??\\D:", xiso_dir});
        sys.hle_heap.mounts.push_back({"\\Device\\CdRom0", xiso_dir});
    }

    sys.exec->hle_handler = xbe::default_hle_handler;
    sys.exec->hle_user    = &sys.hle_heap;

    sys.entry_eip = sys.xbe_info.entry_point;
    sys.running = true;
    return true;
}

// ---------------------------------------------------------------------------
// RC4 stream cipher — used by the MCPX ROM to decrypt the 2BL.
// ---------------------------------------------------------------------------
inline void rc4_crypt(const uint8_t* key, size_t key_len,
                      uint8_t* data, size_t data_len)
{
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % key_len]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }
    int si = 0; j = 0;
    for (size_t n = 0; n < data_len; n++) {
        si = (si + 1) & 0xFF;
        j  = (j + S[si]) & 0xFF;
        uint8_t tmp = S[si]; S[si] = S[j]; S[j] = tmp;
        data[n] ^= S[(S[si] + S[j]) & 0xFF];
    }
}

// 2BL constants
static constexpr uint32_t BL2_LOAD_ADDR = 0x00090000u;  // RAM address where MCPX 1.0 loads 2BL
static constexpr uint32_t BL2_SIZE      = 0x6000u;       // MCPX 1.0 decrypts 24 KB
static constexpr uint32_t MCPX_KEY_OFF  = 0x1A5u;        // RC4 key offset in MCPX 1.0 ROM
static constexpr uint32_t MCPX_KEY_LEN  = 16u;           // RC4 key length

// ---------------------------------------------------------------------------
// find_2bl_code_entry — scan flash for the 2BL executable code entry point.
// In a pre-decrypted BIOS, the code section is in a lower-entropy region
// of the flash (typically near offset 0x3D400 for 256KB images).
// The entry is identified by a JMP near (E9) instruction at the start of the
// code section, surrounded by 24KB of data with internal CALL references.
// Returns the flash offset of the entry, or 0 on failure.
// ---------------------------------------------------------------------------
inline uint32_t find_2bl_code_entry(const uint8_t* flash, uint32_t flash_size)
{
    // Scan for E9 xx xx xx xx (JMP near rel32) followed by data/padding,
    // in the region where entropy drops (typically 0x30000-0x3FE00 for 256KB).
    // We look for the JMP and verify there are CALL near instructions nearby.
    uint32_t scan_start = flash_size > 0x40000 ? flash_size - 0x10000 : flash_size / 2;
    uint32_t scan_end   = flash_size - 0x200; // leave room for MCPX overlay

    for (uint32_t off = scan_start; off < scan_end; off += 0x10) {
        if (flash[off] != 0xE9) continue;

        // Decode JMP target — must land within reasonable range
        int32_t rel;
        memcpy(&rel, flash + off + 1, 4);
        uint32_t target = off + 5 + (uint32_t)rel;
        if (target <= off || target >= scan_end) continue;

        // Verify: count CALL near (E8) with in-range targets in the next 8KB
        uint32_t check_end = off + 0x2000;
        if (check_end > flash_size) check_end = flash_size;
        int call_count = 0;
        for (uint32_t ci = off; ci + 5 < check_end; ci++) {
            if (flash[ci] != 0xE8) continue;
            int32_t crel;
            memcpy(&crel, flash + ci + 1, 4);
            uint32_t ctarget = ci + 5 + (uint32_t)crel;
            if (ctarget >= off && ctarget < check_end)
                call_count++;
        }
        if (call_count >= 5) // real code has many internal calls
            return off;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// is_bios_pre_decrypted — check if the BIOS 2BL region appears to already
// be decrypted (e.g., copyright string visible in first 24KB).
// ---------------------------------------------------------------------------
inline bool is_bios_pre_decrypted(const uint8_t* flash, uint32_t flash_size)
{
    // Check first 24KB for "Microsoft" copyright string (visible in plaintext)
    uint32_t check_len = BL2_SIZE < flash_size ? BL2_SIZE : flash_size;
    const char* sig = "Microsoft";
    size_t sig_len = 9;
    for (uint32_t i = 0; i + sig_len <= check_len; i++) {
        if (memcmp(flash + i, sig, sig_len) == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// scan_and_dump_kernel — scan guest RAM for a valid PE image and write it to
// a file.  Returns the PA of the first kernel-sized PE found, or 0.
// ---------------------------------------------------------------------------
inline uint32_t scan_and_dump_kernel(const uint8_t* ram, uint32_t ram_size,
                                     const std::string& dump_path,
                                     std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };
    uint32_t found_pa = 0;

    for (uint32_t pa = 0; pa + 0x200 < ram_size; pa += 0x1000) {
        if (ram[pa] != 'M' || ram[pa + 1] != 'Z') continue;

        uint32_t e_lfanew;
        memcpy(&e_lfanew, ram + pa + 0x3C, 4);
        if (e_lfanew == 0 || e_lfanew >= 0x1000 || pa + e_lfanew + 0x100 >= ram_size)
            continue;

        uint32_t pe_sig;
        memcpy(&pe_sig, ram + pa + e_lfanew, 4);
        if (pe_sig != 0x00004550u) continue; // 'PE\0\0'

        uint16_t machine, num_sec;
        memcpy(&machine,  ram + pa + e_lfanew + 4, 2);
        memcpy(&num_sec,  ram + pa + e_lfanew + 6, 2);

        uint32_t opt = pa + e_lfanew + 24;
        uint32_t img_base, img_size, entry_rva;
        memcpy(&entry_rva, ram + opt + 16, 4);
        memcpy(&img_base,  ram + opt + 28, 4);
        memcpy(&img_size,  ram + opt + 56, 4);

        // Filter: must be i386, reasonable size for xboxkrnl.exe
        if (machine != 0x014C) continue;
        if (img_size < 0x10000 || img_size > 0x400000) continue;

        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[kernel] PE at PA=0x%08X: base=0x%08X size=0x%X entry=0x%X sections=%u",
                 pa, img_base, img_size, entry_rva, num_sec);
        say(msg);

        if (found_pa == 0) found_pa = pa;

        // Dump to file if requested
        if (!dump_path.empty()) {
            uint32_t dump_sz = img_size;
            if (pa + dump_sz > ram_size) dump_sz = ram_size - pa;

            FILE* df = fopen(dump_path.c_str(), "wb");
            if (df) {
                fwrite(ram + pa, 1, dump_sz, df);
                fclose(df);
                snprintf(msg, sizeof(msg),
                         "[kernel] Dumped %u bytes to %s", dump_sz, dump_path.c_str());
                say(msg);
            } else {
                snprintf(msg, sizeof(msg),
                         "[kernel] Failed to write %s", dump_path.c_str());
                say(msg);
            }
            break; // dump only the first match
        }
    }
    return found_pa;
}

// ---------------------------------------------------------------------------
// boot_lle — load BIOS (+ optional MCPX) and enter via reset vector.
// Returns false on error; fills sys.entry_eip on success.
// ---------------------------------------------------------------------------
inline bool boot_lle(XboxSystem& sys, const BootConfig& cfg,
                     std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };

    sys.hw = xbox_setup(*sys.exec);

    if (!sys.hw->flash.load_bios(cfg.bios_path.c_str())) {
        say("[boot] Failed to load BIOS image");
        return false;
    }
    say("[boot] BIOS loaded into flash");

    // --- Obtain the RC4 key for 2BL decryption ---
    // Priority: explicit key file > extract from MCPX ROM > none (skip decrypt)
    uint8_t rc4_key[MCPX_KEY_LEN] = {};
    bool have_key = false;

    if (!cfg.rc4_key_path.empty()) {
        // Load raw 16-byte key file
        auto key_data = read_file_to_vec(cfg.rc4_key_path);
        if (key_data.size() == MCPX_KEY_LEN) {
            memcpy(rc4_key, key_data.data(), MCPX_KEY_LEN);
            have_key = true;
            say("[boot] RC4 key loaded from key file");
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "[boot] RC4 key file must be %u bytes (got %zu)",
                     MCPX_KEY_LEN, key_data.size());
            say(msg);
            return false;
        }
    } else if (!cfg.mcpx_path.empty()) {
        // Extract key from MCPX ROM at known offset
        if (!sys.hw->flash.load_mcpx(cfg.mcpx_path.c_str())) {
            say("[boot] Failed to load MCPX ROM");
            return false;
        }
        say("[boot] MCPX ROM loaded");
        if (MCPX_KEY_OFF + MCPX_KEY_LEN <= FlashState::MCPX_ROM_SIZE) {
            memcpy(rc4_key, sys.hw->flash.mcpx_rom + MCPX_KEY_OFF, MCPX_KEY_LEN);
            have_key = true;
            say("[boot] RC4 key extracted from MCPX ROM");
        }
    }

    // --- Decrypt and load the 2BL ---
    bool pre_decrypted = false;
    if (have_key) {
        // Check if the BIOS is already decrypted (copyright string visible)
        if (is_bios_pre_decrypted(sys.hw->flash.data, FLASH_SIZE)) {
            say("[boot] BIOS appears pre-decrypted (copyright string found) — skipping RC4");
            pre_decrypted = true;
        } else {
            // Copy 2BL from flash offset 0 into RAM at BL2_LOAD_ADDR
            if (BL2_LOAD_ADDR + BL2_SIZE <= GUEST_RAM_SIZE) {
                memcpy(sys.exec->ram + BL2_LOAD_ADDR,
                       sys.hw->flash.data, BL2_SIZE);
                // RC4-decrypt the 2BL in-place
                rc4_crypt(rc4_key, MCPX_KEY_LEN,
                          sys.exec->ram + BL2_LOAD_ADDR, BL2_SIZE);

                char msg[256];
                snprintf(msg, sizeof(msg),
                         "[boot] 2BL decrypted: first bytes %02X %02X %02X %02X at 0x%08X",
                         sys.exec->ram[BL2_LOAD_ADDR + 0],
                         sys.exec->ram[BL2_LOAD_ADDR + 1],
                         sys.exec->ram[BL2_LOAD_ADDR + 2],
                         sys.exec->ram[BL2_LOAD_ADDR + 3],
                         BL2_LOAD_ADDR);
                say(msg);

                // Check if decryption produced valid code
                uint8_t first = sys.exec->ram[BL2_LOAD_ADDR];
                if (first == 0xCC || first == 0xFF || first == 0x00) {
                    say("[boot] WARNING: decrypted 2BL starts with suspicious byte — wrong key?");
                }
            }
        }
    } else {
        // No key provided — check if BIOS is pre-decrypted
        if (is_bios_pre_decrypted(sys.hw->flash.data, FLASH_SIZE)) {
            say("[boot] No RC4 key, but BIOS appears pre-decrypted — will boot from flash");
            pre_decrypted = true;
        } else {
            say("[boot] No RC4 key — 2BL is still encrypted.");
            say("[boot] Provide --rc4-key <key.bin> or --mcpx <mcpx.bin> for full LLE boot.");
        }
    }

    // Real-mode prologue → protected mode
    if (!sys.exec->interpret_real_mode_boot()) {
        say("[boot] Real-mode boot failed");
        return false;
    }

    char msg[256];
    if (pre_decrypted) {
        // Pre-decrypted BIOS: find the 2BL code entry in flash and execute
        // from the flash-mapped address (MMIO code fetch).
        uint32_t entry_off = find_2bl_code_entry(sys.hw->flash.data, FLASH_SIZE);
        if (entry_off == 0) {
            say("[boot] Could not find 2BL code entry in pre-decrypted BIOS");
            return false;
        }
        // Flash maps at both FLASH_BASE and BIOS_BASE; use FLASH_BASE for simplicity.
        uint32_t entry_addr = FLASH_BASE + entry_off;
        sys.entry_eip = entry_addr;
        // Set up stack in low RAM (the 2BL will set up its own eventually)
        sys.exec->ctx.gp[GP_ESP] = 0x00090000;
        snprintf(msg, sizeof(msg),
                 "[boot] Pre-decrypted BIOS: 2BL code at flash+0x%X -> EIP=0x%08X",
                 entry_off, entry_addr);
    } else if (have_key) {
        // Skip MCPX ROM — jump directly to decrypted 2BL
        sys.entry_eip = BL2_LOAD_ADDR;
        // Set up minimal CPU state that the 2BL expects:
        // - Protected mode (already set by interpret_real_mode_boot)
        // - ESP at a sane location
        sys.exec->ctx.gp[GP_ESP] = BL2_LOAD_ADDR - 4; // stack just below 2BL
        snprintf(msg, sizeof(msg), "[boot] Entering decrypted 2BL at EIP=0x%08X",
                 BL2_LOAD_ADDR);
    } else {
        snprintf(msg, sizeof(msg), "[boot] PM entry EIP=0x%08X CR0=0x%08X",
                 sys.exec->ctx.eip, sys.exec->ctx.cr0);
    }
    say(msg);

    if (!have_key && !pre_decrypted) sys.entry_eip = sys.exec->ctx.eip;
    sys.running = true;
    return true;
}

// ---------------------------------------------------------------------------
// boot_lle_kernel — load the real xboxkrnl.exe as a PE, then load an XBE
// and resolve its kernel thunks against the real kernel's export table.
// Ordinals not found in the kernel fall back to HLE stubs.
// Returns false on error; fills sys.entry_eip on success.
// ---------------------------------------------------------------------------
inline bool boot_lle_kernel(XboxSystem& sys, const BootConfig& cfg,
                            std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };

    sys.hw = xbox_setup(*sys.exec);

    // ---- Load xboxkrnl.exe into guest RAM ----
    pe::LoadResult krnl = pe::load_pe(sys.exec->ram, GUEST_RAM_SIZE, cfg.kernel_path.c_str());
    if (!krnl.ok) {
        say("[boot] Failed to load xboxkrnl.exe");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[boot] Kernel loaded: base=0x%08X entry=0x%08X size=0x%08X sections=%u",
             krnl.image_base, krnl.entry_point, krnl.image_size, krnl.num_sections);
    say(msg);

    // ---- Load XBE file ----
    auto xbe_data = read_file_to_vec(cfg.xbe_path);
    if (xbe_data.empty()) {
        say("[boot] Failed to read XBE file");
        return false;
    }

    // Copy raw XBE into RAM for load_xbe to parse
    uint32_t load_pa = 0x1000;
    sys.exec->load_guest(load_pa, xbe_data.data(), xbe_data.size());

    // Parse XBE (this also writes HLE stubs and resolves thunks to HLE)
    sys.xbe_info = xbe::load_xbe(sys.exec->ram, xbe_data.data(), xbe_data.size());
    if (!sys.xbe_info.valid) {
        say("[boot] XBE parsing failed");
        return false;
    }

    snprintf(msg, sizeof(msg), "[boot] XBE entry=0x%08X thunks=0x%08X sections=%u",
             sys.xbe_info.entry_point, sys.xbe_info.kernel_thunk_va, sys.xbe_info.num_sections);
    say(msg);

    // ---- Re-resolve kernel thunks against real kernel exports ----
    // load_xbe already resolved all thunks to HLE stubs.  Now re-patch
    // each one: if the real kernel exports that ordinal, use the real VA;
    // otherwise keep the HLE stub (fallback).
    uint32_t thunk_va = sys.xbe_info.kernel_thunk_va;
    uint32_t resolved_lle = 0, fallback_hle = 0;
    for (uint32_t off = thunk_va; off + 4 <= GUEST_RAM_SIZE; off += 4) {
        uint32_t current_va;
        memcpy(&current_va, sys.exec->ram + off, 4);
        if (current_va == 0) break;

        // Determine which ordinal this thunk was for by checking if it
        // points into the HLE stub region
        if (current_va >= xbe::HLE_STUB_BASE &&
            current_va < xbe::HLE_STUB_BASE + xbe::MAX_KERNEL_ORDINALS * xbe::HLE_STUB_SIZE) {
            uint32_t ordinal = (current_va - xbe::HLE_STUB_BASE) / xbe::HLE_STUB_SIZE;

            // Also check if it's a data export (those point to KDATA_BASE area)
            // — skip re-resolving those, they're fine as-is
            uint32_t data_va = xbe::kernel_data_addr(ordinal);
            if (data_va) continue;  // data export — keep kernel data pointer

            // Try to resolve from real kernel
            uint32_t real_va = pe::resolve_export_by_ordinal(
                sys.exec->ram, krnl.image_base, ordinal);
            if (real_va != 0) {
                memcpy(sys.exec->ram + off, &real_va, 4);
                ++resolved_lle;
            } else {
                ++fallback_hle;
            }
        }
        // If it points to KDATA area, it's a data export — leave it
    }

    snprintf(msg, sizeof(msg), "[boot] Thunks: %u resolved to kernel, %u HLE fallback",
             resolved_lle, fallback_hle);
    say(msg);

    // ---- Stack and KPCR setup (same as HLE mode) ----
    constexpr uint32_t STACK_TOP = 0x00B0'0000u;
    constexpr uint32_t SENTINEL  = 0xFFFF'FFFFu;
    sys.exec->ctx.gp[GP_ESP] = STACK_TOP;
    memcpy(sys.exec->ram + STACK_TOP, &SENTINEL, 4);
    sys.exec->ctx.eflags = 0x0000'0202;

    constexpr uint32_t FS_BASE = 0x00D0'0000u;
    sys.exec->ctx.fs_base = FS_BASE;
    sys.exec->ctx.gs_base = 0;
    setup_kpcr(*sys.exec, FS_BASE, STACK_TOP);

    // ---- Install HLE handler for fallback stubs ----
    // HLE stubs are still present in RAM — if the guest calls an ordinal
    // that wasn't in the real kernel, INT 0x20 fires and we handle it.
    sys.hle_heap.reset();
    sys.hle_heap.set_xbe_path(cfg.xbe_path);

    if (!cfg.xiso_path.empty()) {
        size_t sep = cfg.xiso_path.find_last_of("/\\");
        std::string xiso_dir = (sep != std::string::npos)
            ? cfg.xiso_path.substr(0, sep) : ".";
        sys.hle_heap.mounts.push_back({"\\??\\D:", xiso_dir});
        sys.hle_heap.mounts.push_back({"\\Device\\CdRom0", xiso_dir});
    }

    sys.exec->hle_handler = xbe::default_hle_handler;
    sys.exec->hle_user    = &sys.hle_heap;

    // ---- Choose entry point ----
    // The real Xbox boot has the kernel run first (KiSystemStartup), which
    // then loads and calls the XBE.  For now, we start at the XBE entry
    // point since the kernel's init depends on hardware state we don't
    // fully emulate.  The thunks point into real kernel code, so when the
    // XBE calls kernel functions they execute natively.
    sys.entry_eip = sys.xbe_info.entry_point;
    sys.running = true;

    say("[boot] LLE-kernel mode ready — starting at XBE entry point");
    return true;
}

// ---------------------------------------------------------------------------
// run_step — run the executor for a batch of steps.  Returns true if still
// running (not halted).  Dispatches pending HLE threads non-blockingly:
// if a thread is pending after the main code halts, it gets set up for the
// *next* frame's run_step call (never blocks the UI thread).
//
// Wait/delay HLE stubs set halted=true as a "yield" — the guest is still
// alive but wants to give up its timeslice.  Only a halt at the sentinel
// EIP (0xFFFFFFFF) or the HalReturnToFirmware stub is a real stop.
// ---------------------------------------------------------------------------
inline bool run_step(XboxSystem& sys, uint32_t max_steps = 500'000) {
    if (!sys.running) return false;

    // Fire any pending DPCs before running guest code.
    // DPC routines are called at DISPATCH_LEVEL between timeslices.
    while (!sys.hle_heap.pending_dpcs.empty()) {
        xbe::PendingDpc dpc = sys.hle_heap.pending_dpcs.front();
        sys.hle_heap.pending_dpcs.erase(sys.hle_heap.pending_dpcs.begin());

        // DPC routine: void (PKDPC, PVOID Context, PVOID SysArg1, PVOID SysArg2)
        uint32_t esp = sys.exec->ctx.gp[GP_ESP];
        uint32_t zero = 0;
        // Push args right-to-left: SysArg2, SysArg1(timer), Context, Dpc
        esp -= 4; if (esp + 4 <= GUEST_RAM_SIZE) memcpy(sys.exec->ram + esp, &zero, 4);
        esp -= 4; if (esp + 4 <= GUEST_RAM_SIZE) memcpy(sys.exec->ram + esp, &dpc.timer_va, 4);
        esp -= 4; if (esp + 4 <= GUEST_RAM_SIZE) memcpy(sys.exec->ram + esp, &dpc.context, 4);
        esp -= 4; if (esp + 4 <= GUEST_RAM_SIZE) memcpy(sys.exec->ram + esp, &dpc.dpc_va, 4);
        // Push sentinel return address
        uint32_t sentinel = 0xFFFFFFFFu;
        esp -= 4; if (esp + 4 <= GUEST_RAM_SIZE) memcpy(sys.exec->ram + esp, &sentinel, 4);
        sys.exec->ctx.gp[GP_ESP] = esp;

        sys.exec->ctx.halted = false;
        sys.exec->ctx.stop_reason = STOP_NONE;
        sys.exec->run(dpc.routine, max_steps);
        // DPC routine has finished (or hit max_steps) — continue
    }

    sys.exec->ctx.halted = false;
    sys.exec->ctx.stop_reason = STOP_NONE;
    sys.exec->run(sys.entry_eip, max_steps);

    // Check for error stops — these are always permanent
    uint32_t sr = sys.exec->ctx.stop_reason;
    if (sr == STOP_INVALID_OPCODE || sr == STOP_DIVIDE_ERROR) {
        sys.running = false;
        return false;
    }

    // Check if this is a permanent halt (sentinel or HalReturnToFirmware)
    uint32_t eip = sys.exec->ctx.eip;
    bool real_halt = sys.exec->ctx.halted &&
        (eip == 0xFFFFFFFF ||
         eip == xbe::hle_stub_addr(xbe::ORD_HalReturnToFirmware));

    // If permanently halted and there are pending threads, dispatch one
    // for the *next* frame (never block the UI thread).
    if (real_halt && !sys.hle_heap.pending_threads.empty()) {
        xbe::PendingThread t = sys.hle_heap.pending_threads.front();
        sys.hle_heap.pending_threads.erase(sys.hle_heap.pending_threads.begin());

        // Allocate a fresh stack for the new thread (64 KB, page-aligned).
        constexpr uint32_t THREAD_STACK_SIZE = 0x10000u;
        uint32_t stack_base = sys.hle_heap.alloc(THREAD_STACK_SIZE);
        uint32_t esp = stack_base ? (stack_base + THREAD_STACK_SIZE) : sys.exec->ctx.gp[GP_ESP];

        // Push start_context as argument
        esp -= 4;
        if (esp + 4 <= GUEST_RAM_SIZE)
            memcpy(sys.exec->ram + esp, &t.start_context, 4);
        // Push halt return address (will stop execution when thread returns)
        uint32_t halt_ret = xbe::hle_stub_addr(xbe::ORD_HalReturnToFirmware);
        esp -= 4;
        if (esp + 4 <= GUEST_RAM_SIZE)
            memcpy(sys.exec->ram + esp, &halt_ret, 4);
        sys.exec->ctx.gp[GP_ESP] = esp;

        // Set entry EIP for next frame — don't run it now
        sys.entry_eip = t.start_routine;
        return true;  // still running — will dispatch thread next frame
    }

    // Permanent halt with no pending threads → done
    if (real_halt) {
        sys.running = false;
        return false;
    }

    // Either max_steps exhausted or a yield-halt (wait/delay stub).
    // Continue from current EIP next frame.
    sys.entry_eip = sys.exec->ctx.eip;
    return true;
}

} // namespace xbox
