#pragma once
// ---------------------------------------------------------------------------
// nboxkrnl_keys.hpp — EEPROM and certificate key configuration.
//
// nboxkrnl expects 32 bytes on the initial stack:
//   Bytes  0-15: EEPROM key   (for EEPROM section decryption)
//   Bytes 16-31: Certificate key
//
// When keys are all-zero, nboxkrnl skips EEPROM decryption, so the SMBus
// EEPROM data should contain unencrypted defaults (which init_eeprom() in
// smbus.hpp already provides).
//
// For real Xbox keys, pass a 32-byte file via --keys.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nboxkrnl {

static constexpr size_t KEY_SIZE = 32;  // 16B EEPROM + 16B certificate

struct KeyConfig {
    uint8_t data[KEY_SIZE];

    // Check if all keys are zero.
    bool is_zero() const {
        for (size_t i = 0; i < KEY_SIZE; ++i)
            if (data[i] != 0) return false;
        return true;
    }

    const uint8_t* eeprom_key()     const { return data; }
    const uint8_t* certificate_key() const { return data + 16; }
};

// Return zero keys (default: unencrypted EEPROM).
inline KeyConfig default_keys() {
    KeyConfig k;
    memset(k.data, 0, KEY_SIZE);
    return k;
}

// Load 32-byte key file.  Returns false on error.
inline bool load_keys(const char* path, KeyConfig& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[nboxkrnl] cannot open keys file: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != KEY_SIZE) {
        fprintf(stderr, "[nboxkrnl] keys file must be exactly %zu bytes (got %ld)\n",
                KEY_SIZE, sz);
        fclose(f);
        return false;
    }

    if (fread(out.data, 1, KEY_SIZE, f) != KEY_SIZE) {
        fprintf(stderr, "[nboxkrnl] failed to read keys file\n");
        fclose(f);
        return false;
    }

    fclose(f);

    fprintf(stderr, "[nboxkrnl] loaded keys from %s (%s)\n",
            path, out.is_zero() ? "all-zero" : "non-zero");
    return true;
}

} // namespace nboxkrnl
