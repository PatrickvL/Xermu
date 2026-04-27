// test_nboxkrnl_io.cpp — Unit tests for nboxkrnl host-side file I/O system.
//
// Tests the full submit → process → query pipeline by constructing IoRequest
// packets in guest RAM and exercising open, read, write, and close operations
// via the IoSystem worker thread.

#include "cpu/executor.hpp"
#include "xbox/nboxkrnl_host.hpp"
#include "xbox/nboxkrnl_io.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// Stub MMIO
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t, unsigned, void*) { return 0; }
static void stub_mmio_write(uint32_t, uint32_t, unsigned, void*) {}

static MmioMap make_mmio() {
    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);
    return mmio;
}

// Helper: sleep a bit to let the I/O worker process requests.
static void wait_io() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ---------------------------------------------------------------------------
// Test directory setup.
// ---------------------------------------------------------------------------

static const char* TEST_DIR = "data/hdd_test";

static void setup_test_dir() {
    namespace fs = std::filesystem;
    // Clean any leftover from a prior run.
    std::error_code ec;
    fs::remove_all(TEST_DIR, ec);
    fs::create_directories(std::string(TEST_DIR) + "/Partition2");

    // Create a test file with known content.
    std::ofstream ofs(std::string(TEST_DIR) + "/Partition2/test.txt",
                      std::ios::binary);
    ofs << "Hello from test file!";
    ofs.close();
}

static void cleanup_test_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(TEST_DIR, ec);
}

// ---------------------------------------------------------------------------
// Test 1: Open a file via IoRequest packet, verify completion.
// ---------------------------------------------------------------------------

static bool test_open_file() {
    printf("=== Test: open file via I/O packet ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    // Set up I/O system.
    nboxkrnl::IoSystem io;
    io.init(exec.get(), ".", TEST_DIR);

    // Construct the open request packet in guest RAM at PA 0x10000.
    // Path: "\Device\Harddisk0\Partition2\test.txt"
    const char* xbox_path = "\\Device\\Harddisk0\\Partition2\\test.txt";
    uint32_t path_len = (uint32_t)strlen(xbox_path);

    // Write path to guest at PA 0x11000.
    memcpy(exec->ram + 0x11000, xbox_path, path_len);

    // Build the packed request at PA 0x10000.
    nboxkrnl::PackedRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 1;
    // type: open(1<<28) | dev=DEV_PARTITION2(4<<23) | disposition=IO_OPEN(1)
    pkt.header.type  = (1u << 28) | (4u << 23) | 1u;
    pkt.oc.size      = path_len;
    pkt.oc.handle    = 100;
    pkt.oc.path      = 0x11000;  // guest VA of path string
    pkt.oc.timestamp = 0x12345678;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    // Submit the packet.
    io.submit_io_packet(0x10000);

    // Wait for worker to process.
    wait_io();

    // Build an info block query at PA 0x12000.
    nboxkrnl::InfoBlockOc info;
    memset(&info, 0, sizeof(info));
    info.header.id = 1;  // match the request ID
    memcpy(exec->ram + 0x12000, &info, sizeof(info));

    // Query.
    io.query_io_packet(0x12000);

    // Read back the info block from guest RAM.
    memcpy(&info, exec->ram + 0x12000, sizeof(info));

    bool pass = (info.header.ready == 1 &&
                 info.header.status == nboxkrnl::STATUS_SUCCESS &&
                 info.header.info == nboxkrnl::INFO_OPENED &&
                 info.file_size == 21);  // "Hello from test file!" = 21 bytes

    printf("  ready=%u status=0x%08X info=%u file_size=%u\n",
           info.header.ready, (uint32_t)info.header.status,
           info.header.info, info.file_size);
    printf("  %s\n", pass ? "PASS" : "FAIL");

    io.stop();
    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 2: Open + read back data, verify content.
// ---------------------------------------------------------------------------

static bool test_read_file() {
    printf("=== Test: read file data via I/O packets ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::IoSystem io;
    io.init(exec.get(), ".", TEST_DIR);

    // Step 1: Open the file.
    const char* xbox_path = "\\Device\\Harddisk0\\Partition2\\test.txt";
    uint32_t path_len = (uint32_t)strlen(xbox_path);
    memcpy(exec->ram + 0x11000, xbox_path, path_len);

    nboxkrnl::PackedRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 10;
    pkt.header.type  = (1u << 28) | (4u << 23) | 1u;  // open, partition2, IO_OPEN
    pkt.oc.size      = path_len;
    pkt.oc.handle    = 200;
    pkt.oc.path      = 0x11000;
    pkt.oc.timestamp = 0;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    io.submit_io_packet(0x10000);
    wait_io();

    // Query open completion.
    nboxkrnl::InfoBlockOc info;
    memset(&info, 0, sizeof(info));
    info.header.id = 10;
    memcpy(exec->ram + 0x12000, &info, sizeof(info));
    io.query_io_packet(0x12000);
    memcpy(&info, exec->ram + 0x12000, sizeof(info));

    if (info.header.status != nboxkrnl::STATUS_SUCCESS) {
        printf("  open failed: status=0x%08X\n", (uint32_t)info.header.status);
        io.stop(); exec->destroy(); return false;
    }

    // Step 2: Read 21 bytes.
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 11;
    pkt.header.type  = (4u << 28) | (4u << 23);  // read, partition2
    pkt.rw.offset    = 0;
    pkt.rw.size      = 21;
    pkt.rw.address   = 0x13000;  // guest VA for data buffer
    pkt.rw.handle    = 200;
    pkt.rw.timestamp = 0;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    // Clear the data area.
    memset(exec->ram + 0x13000, 0, 256);

    io.submit_io_packet(0x10000);
    wait_io();

    // Query read completion.
    nboxkrnl::InfoBlock rinfo;
    memset(&rinfo, 0, sizeof(rinfo));
    rinfo.id = 11;
    memcpy(exec->ram + 0x14000, &rinfo, sizeof(rinfo));
    io.query_io_packet(0x14000);
    memcpy(&rinfo, exec->ram + 0x14000, sizeof(rinfo));

    // Data should now be at guest PA 0x13000.
    char data[32] = {};
    memcpy(data, exec->ram + 0x13000, 21);

    bool pass = (rinfo.ready == 1 &&
                 rinfo.status == nboxkrnl::STATUS_SUCCESS &&
                 memcmp(data, "Hello from test file!", 21) == 0);

    printf("  read ready=%u status=0x%08X data=\"%s\"\n",
           rinfo.ready, (uint32_t)rinfo.status, data);
    printf("  %s\n", pass ? "PASS" : "FAIL");

    io.stop();
    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 3: Create a new file, write data, close, reopen, read back.
// ---------------------------------------------------------------------------

static bool test_create_write_read() {
    printf("=== Test: create + write + read round-trip ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::IoSystem io;
    io.init(exec.get(), ".", TEST_DIR);

    // Step 1: Create a new file.
    const char* xbox_path = "\\Device\\Harddisk0\\Partition2\\newfile.txt";
    uint32_t path_len = (uint32_t)strlen(xbox_path);
    memcpy(exec->ram + 0x11000, xbox_path, path_len);

    nboxkrnl::PackedRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 20;
    pkt.header.type  = (1u << 28) | (4u << 23) | 2u;  // open, partition2, IO_CREATE
    pkt.oc.size      = path_len;
    pkt.oc.handle    = 300;
    pkt.oc.path      = 0x11000;
    pkt.oc.timestamp = 0;
    pkt.oc.attributes = 0;  // not a directory
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    io.submit_io_packet(0x10000);
    wait_io();

    nboxkrnl::InfoBlockOc info;
    memset(&info, 0, sizeof(info));
    info.header.id = 20;
    memcpy(exec->ram + 0x12000, &info, sizeof(info));
    io.query_io_packet(0x12000);
    memcpy(&info, exec->ram + 0x12000, sizeof(info));

    if (info.header.status != nboxkrnl::STATUS_SUCCESS) {
        printf("  create failed: status=0x%08X\n", (uint32_t)info.header.status);
        io.stop(); exec->destroy(); return false;
    }

    // Step 2: Write data to the file.
    const char* write_data = "Test write data 12345";
    uint32_t write_len = (uint32_t)strlen(write_data);
    memcpy(exec->ram + 0x13000, write_data, write_len);

    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 21;
    pkt.header.type  = (5u << 28) | (4u << 23);  // write, partition2
    pkt.rw.offset    = 0;
    pkt.rw.size      = write_len;
    pkt.rw.address   = 0x13000;
    pkt.rw.handle    = 300;
    pkt.rw.timestamp = 0;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    io.submit_io_packet(0x10000);
    wait_io();

    nboxkrnl::InfoBlock winfo;
    memset(&winfo, 0, sizeof(winfo));
    winfo.id = 21;
    memcpy(exec->ram + 0x14000, &winfo, sizeof(winfo));
    io.query_io_packet(0x14000);
    memcpy(&winfo, exec->ram + 0x14000, sizeof(winfo));

    if (winfo.status != nboxkrnl::STATUS_SUCCESS) {
        printf("  write failed: status=0x%08X\n", (uint32_t)winfo.status);
        io.stop(); exec->destroy(); return false;
    }

    // Step 3: Close the file.
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 22;
    pkt.header.type  = (3u << 28) | (4u << 23);  // close, partition2
    pkt.xx.handle    = 300;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    io.submit_io_packet(0x10000);
    wait_io();

    nboxkrnl::InfoBlock cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.id = 22;
    memcpy(exec->ram + 0x15000, &cinfo, sizeof(cinfo));
    io.query_io_packet(0x15000);

    // Step 4: Reopen and read back.
    memcpy(exec->ram + 0x11000, xbox_path, path_len);

    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 23;
    pkt.header.type  = (1u << 28) | (4u << 23) | 1u;  // open, partition2, IO_OPEN
    pkt.oc.size      = path_len;
    pkt.oc.handle    = 301;
    pkt.oc.path      = 0x11000;
    pkt.oc.timestamp = 0;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    io.submit_io_packet(0x10000);
    wait_io();

    memset(&info, 0, sizeof(info));
    info.header.id = 23;
    memcpy(exec->ram + 0x12000, &info, sizeof(info));
    io.query_io_packet(0x12000);
    memcpy(&info, exec->ram + 0x12000, sizeof(info));

    if (info.header.status != nboxkrnl::STATUS_SUCCESS) {
        printf("  reopen failed: status=0x%08X\n", (uint32_t)info.header.status);
        io.stop(); exec->destroy(); return false;
    }

    // Read the data back.
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 24;
    pkt.header.type  = (4u << 28) | (4u << 23);  // read, partition2
    pkt.rw.offset    = 0;
    pkt.rw.size      = write_len;
    pkt.rw.address   = 0x16000;
    pkt.rw.handle    = 301;
    pkt.rw.timestamp = 0;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));
    memset(exec->ram + 0x16000, 0, 256);

    io.submit_io_packet(0x10000);
    wait_io();

    nboxkrnl::InfoBlock rinfo;
    memset(&rinfo, 0, sizeof(rinfo));
    rinfo.id = 24;
    memcpy(exec->ram + 0x17000, &rinfo, sizeof(rinfo));
    io.query_io_packet(0x17000);
    memcpy(&rinfo, exec->ram + 0x17000, sizeof(rinfo));

    char readback[32] = {};
    memcpy(readback, exec->ram + 0x16000, write_len);

    bool pass = (rinfo.ready == 1 &&
                 rinfo.status == nboxkrnl::STATUS_SUCCESS &&
                 memcmp(readback, write_data, write_len) == 0);

    printf("  readback: \"%s\" (expected \"%s\")\n", readback, write_data);
    printf("  %s\n", pass ? "PASS" : "FAIL");

    io.stop();
    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 4: Open non-existent file with IO_OPEN → STATUS_OBJECT_NAME_NOT_FOUND.
// ---------------------------------------------------------------------------

static bool test_open_nonexistent() {
    printf("=== Test: open non-existent file ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::IoSystem io;
    io.init(exec.get(), ".", TEST_DIR);

    const char* xbox_path = "\\Device\\Harddisk0\\Partition2\\nosuchfile.txt";
    uint32_t path_len = (uint32_t)strlen(xbox_path);
    memcpy(exec->ram + 0x11000, xbox_path, path_len);

    nboxkrnl::PackedRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.id   = 30;
    pkt.header.type  = (1u << 28) | (4u << 23) | 1u;  // open, partition2, IO_OPEN
    pkt.oc.size      = path_len;
    pkt.oc.handle    = 400;
    pkt.oc.path      = 0x11000;
    pkt.oc.timestamp = 0;
    memcpy(exec->ram + 0x10000, &pkt, sizeof(pkt));

    io.submit_io_packet(0x10000);
    wait_io();

    nboxkrnl::InfoBlockOc info;
    memset(&info, 0, sizeof(info));
    info.header.id = 30;
    memcpy(exec->ram + 0x12000, &info, sizeof(info));
    io.query_io_packet(0x12000);
    memcpy(&info, exec->ram + 0x12000, sizeof(info));

    bool pass = (info.header.ready == 1 &&
                 info.header.status == nboxkrnl::STATUS_OBJECT_NAME_NOT_FOUND);

    printf("  status=0x%08X (expected 0x%08X)\n",
           (uint32_t)info.header.status,
           (uint32_t)nboxkrnl::STATUS_OBJECT_NAME_NOT_FOUND);
    printf("  %s\n", pass ? "PASS" : "FAIL");

    io.stop();
    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    setup_test_dir();

    int passed = 0, failed = 0;
    auto run = [&](bool(*fn)()) {
        fflush(stdout); fflush(stderr);
        if (fn()) ++passed; else ++failed;
        fflush(stdout); fflush(stderr);
    };

    run(test_open_file);
    run(test_read_file);
    run(test_create_write_read);
    run(test_open_nonexistent);

    cleanup_test_dir();

    printf("\n=== nboxkrnl I/O tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
