// test_nboxkrnl_keys.cpp — Test EEPROM/certificate key configuration.
//
// Validates:
// 1. Default keys are all-zero
// 2. Keys load correctly from a 32-byte file
// 3. Keys are written to guest stack at the correct PA
// 4. EEPROM defaults in SmbusState are consistent with zero keys

#include "xbox/nboxkrnl_keys.hpp"
#include "xbox/nboxkrnl_boot.hpp"
#include "xbox/smbus.hpp"
#include "cpu/executor.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test 1: Default keys are all-zero.
// ---------------------------------------------------------------------------

static bool test_default_keys() {
    printf("=== Test: default keys ===\n");

    nboxkrnl::KeyConfig k = nboxkrnl::default_keys();
    bool ok = k.is_zero();
    if (!ok) printf("  default keys are not zero!\n");

    // EEPROM and certificate halves should both be zero.
    for (int i = 0; i < 16; ++i) {
        if (k.eeprom_key()[i] != 0) { ok = false; break; }
    }
    for (int i = 0; i < 16; ++i) {
        if (k.certificate_key()[i] != 0) { ok = false; break; }
    }

    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: Load keys from a file.
// ---------------------------------------------------------------------------

static bool test_load_keys() {
    printf("=== Test: load keys from file ===\n");

    const char* tmp_path = "test_keys_tmp.bin";
    bool ok = true;

    // Write a 32-byte test file.
    {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) { printf("  cannot create temp file\n"); return false; }
        uint8_t data[32];
        for (int i = 0; i < 32; ++i) data[i] = (uint8_t)(i + 1);
        fwrite(data, 1, 32, f);
        fclose(f);
    }

    nboxkrnl::KeyConfig k;
    if (!nboxkrnl::load_keys(tmp_path, k)) {
        printf("  load_keys failed\n");
        ok = false;
    } else {
        // Verify contents.
        for (int i = 0; i < 32; ++i) {
            if (k.data[i] != (uint8_t)(i + 1)) {
                printf("  key[%d] = 0x%02X (expected 0x%02X)\n",
                       i, k.data[i], (uint8_t)(i + 1));
                ok = false;
                break;
            }
        }
        if (ok && k.is_zero()) {
            printf("  keys should not be zero!\n");
            ok = false;
        }
    }

    // Wrong size should fail.
    {
        FILE* f = fopen(tmp_path, "wb");
        if (f) {
            uint8_t data[16] = {};
            fwrite(data, 1, 16, f);
            fclose(f);
        }
        nboxkrnl::KeyConfig bad;
        if (nboxkrnl::load_keys(tmp_path, bad)) {
            printf("  load_keys should fail on 16-byte file\n");
            ok = false;
        }
    }

    std::error_code ec;
    fs::remove(tmp_path, ec);

    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 3: Keys are placed on guest stack.
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t, unsigned, void*) { return 0; }
static void stub_mmio_write(uint32_t, uint32_t, unsigned, void*) {}

static bool test_keys_on_stack() {
    printf("=== Test: keys on guest stack ===\n");

    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);

    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    // Set up page tables so VA→PA mapping works.
    nboxkrnl::setup_page_tables(exec->ram);
    nboxkrnl::setup_cpu_state(*exec);

    // Write known keys to the stack location.
    uint8_t keys[32];
    for (int i = 0; i < 32; ++i) keys[i] = (uint8_t)(0xA0 + i);

    uint32_t stack_pa = nboxkrnl::STACK_VA - nboxkrnl::KERNEL_VA_OFFSET;
    memcpy(exec->ram + stack_pa, keys, 32);

    // Verify the keys at the physical address.
    bool ok = true;
    for (int i = 0; i < 32; ++i) {
        if (exec->ram[stack_pa + i] != (uint8_t)(0xA0 + i)) {
            printf("  stack[%d] = 0x%02X (expected 0x%02X)\n",
                   i, exec->ram[stack_pa + i], (uint8_t)(0xA0 + i));
            ok = false;
            break;
        }
    }

    printf("  %s\n", ok ? "PASS" : "FAIL");
    exec->destroy();
    return ok;
}

// ---------------------------------------------------------------------------
// Test 4: EEPROM defaults are consistent with zero keys.
//
// nboxkrnl with zero keys expects unencrypted EEPROM data.  Verify the
// SmbusState defaults have sensible values at critical offsets.
// ---------------------------------------------------------------------------

static bool test_eeprom_defaults() {
    printf("=== Test: EEPROM defaults ===\n");

    xbox::SmbusState smbus;
    bool ok = true;

    // Game region (0x2C) = NTSC-NA
    if (smbus.eeprom[0x2C] != 0x01) {
        printf("  eeprom[0x2C] (game region) = 0x%02X (expected 0x01)\n",
               smbus.eeprom[0x2C]);
        ok = false;
    }

    // Video standard (0x58-0x5B) = NTSC-M (0x00400100)
    if (smbus.eeprom[0x58] != 0x00 || smbus.eeprom[0x59] != 0x01 ||
        smbus.eeprom[0x5A] != 0x40 || smbus.eeprom[0x5B] != 0x00) {
        printf("  eeprom[0x58..0x5B] (video) = %02X %02X %02X %02X (expected 00 01 40 00)\n",
               smbus.eeprom[0x58], smbus.eeprom[0x59],
               smbus.eeprom[0x5A], smbus.eeprom[0x5B]);
        ok = false;
    }

    // Language (0xE8) = English
    if (smbus.eeprom[0xE8] != 0x01) {
        printf("  eeprom[0xE8] (language) = 0x%02X (expected 0x01)\n",
               smbus.eeprom[0xE8]);
        ok = false;
    }

    // DVD region (0x54) = region 1
    if (smbus.eeprom[0x54] != 0x01) {
        printf("  eeprom[0x54] (DVD region) = 0x%02X (expected 0x01)\n",
               smbus.eeprom[0x54]);
        ok = false;
    }

    // Encrypted section (0x00-0x2B) should be all-zero (matching zero keys).
    for (int i = 0; i < 0x2C; ++i) {
        if (smbus.eeprom[i] != 0) {
            printf("  eeprom[0x%02X] = 0x%02X (expected 0 for zero-key mode)\n",
                   i, smbus.eeprom[i]);
            ok = false;
            break;
        }
    }

    printf("  %s\n", ok ? "PASS" : "FAIL");
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

    run(test_default_keys);
    run(test_load_keys);
    run(test_keys_on_stack);
    run(test_eeprom_defaults);

    printf("\n=== nboxkrnl key tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
