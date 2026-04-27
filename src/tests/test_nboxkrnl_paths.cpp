// test_nboxkrnl_paths.cpp — Unit tests for nboxkrnl path setup.
//
// Verifies that setup_paths() creates HDD partition directories and
// configures the XBE path correctly.

#include "xbox/nboxkrnl_host.hpp"
#include "xbox/nboxkrnl_io.hpp"
#include "xbox/nboxkrnl_paths.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>

static const char* TEST_HDD = "data/hdd_path_test";

// ---------------------------------------------------------------------------
// Test 1: setup_paths with XBE input.
// ---------------------------------------------------------------------------

static bool test_xbe_setup() {
    printf("=== Test: setup_paths with XBE ===\n");

    nboxkrnl::HostState host;
    nboxkrnl::IoSystem io;

    bool ok = nboxkrnl::setup_paths(host, io,
                                     "data/xbox dash orig_5960/xboxdash.xbe",
                                     TEST_HDD);
    if (!ok) { printf("  setup_paths failed\n"); return false; }

    // Verify XBE path.
    bool path_ok = (host.xbe_path == "\\Device\\CdRom0\\xboxdash.xbe");
    printf("  xbe_path: %s (expected \\Device\\CdRom0\\xboxdash.xbe): %s\n",
           host.xbe_path.c_str(), path_ok ? "PASS" : "FAIL");

    // Verify DVD dir.
    bool dvd_ok = (io.dvd_dir.find("xbox dash orig_5960") != std::string::npos);
    printf("  dvd_dir: %s: %s\n", io.dvd_dir.c_str(), dvd_ok ? "PASS" : "FAIL");

    // Verify HDD dir.
    bool hdd_ok = (io.hdd_dir == TEST_HDD);
    printf("  hdd_dir: %s: %s\n", io.hdd_dir.c_str(), hdd_ok ? "PASS" : "FAIL");

    // Verify partition directories exist.
    namespace fs = std::filesystem;
    bool dirs_ok = true;
    for (int i = 0; i < 6; ++i) {
        std::string dir = std::string(TEST_HDD) + "/Partition" + std::to_string(i);
        if (!fs::is_directory(dir)) {
            printf("  missing: %s\n", dir.c_str());
            dirs_ok = false;
        }
    }
    printf("  partition dirs: %s\n", dirs_ok ? "PASS" : "FAIL");

    // Clean up.
    std::error_code ec;
    fs::remove_all(TEST_HDD, ec);

    return path_ok && dvd_ok && hdd_ok && dirs_ok;
}

// ---------------------------------------------------------------------------
// Test 2: setup_paths rejects unknown extension.
// ---------------------------------------------------------------------------

static bool test_bad_extension() {
    printf("=== Test: setup_paths rejects unknown extension ===\n");

    nboxkrnl::HostState host;
    nboxkrnl::IoSystem io;

    bool ok = nboxkrnl::setup_paths(host, io, "game.zip", TEST_HDD);
    bool pass = !ok;
    printf("  returns false for .zip: %s\n", pass ? "PASS" : "FAIL");

    // Clean up any created dirs.
    std::error_code ec;
    std::filesystem::remove_all(TEST_HDD, ec);

    return pass;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    int passed = 0, failed = 0;
    auto run = [&](bool(*fn)()) {
        if (fn()) ++passed; else ++failed;
    };

    run(test_xbe_setup);
    run(test_bad_extension);

    printf("\n=== nboxkrnl path tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
