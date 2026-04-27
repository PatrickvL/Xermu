#pragma once
// ---------------------------------------------------------------------------
// nboxkrnl_paths.hpp — XBE path + disk partition directory setup for nboxkrnl.
//
// Creates the host-side HDD partition directory structure and configures
// the XBE launch path in the format nboxkrnl expects.
// ---------------------------------------------------------------------------

#include "nboxkrnl_host.hpp"
#include "nboxkrnl_io.hpp"
#include <filesystem>
#include <string>
#include <cstdio>

namespace nboxkrnl {

// Default HDD directory (relative to working directory).
static constexpr const char* DEFAULT_HDD_DIR = "data/hdd";

// ---------------------------------------------------------------------------
// Set up the host-side directory structure and XBE path.
//
//   host_state  — HostState to configure (sets xbe_path)
//   io_sys      — IoSystem to configure (sets dvd_dir, hdd_dir)
//   input_path  — path to .xbe file (or future: .xiso)
//   hdd_dir     — host directory for HDD partitions (default: "data/hdd")
//
// Creates:
//   <hdd_dir>/Partition0/  through  <hdd_dir>/Partition5/
//
// For XBE input:
//   dvd_dir  = parent directory of the XBE file
//   xbe_path = "\Device\CdRom0\<filename>.xbe"
//
// Returns false if setup fails (e.g. cannot create directories).
// ---------------------------------------------------------------------------

inline bool setup_paths(HostState& host_state,
                        IoSystem& io_sys,
                        const char* input_path,
                        const char* hdd_dir = DEFAULT_HDD_DIR)
{
    namespace fs = std::filesystem;

    // --- HDD partition directories ---
    for (int i = 0; i < 6; ++i) {
        std::string dir = std::string(hdd_dir) + "/Partition" + std::to_string(i);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            fprintf(stderr, "[nboxkrnl] failed to create %s: %s\n",
                    dir.c_str(), ec.message().c_str());
            return false;
        }
    }

    // --- Determine input type and configure paths ---
    fs::path input(input_path);
    std::string ext = input.extension().string();

    // Lowercase the extension for comparison.
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    if (ext == ".xbe") {
        // XBE mode: DVD directory = parent of XBE file.
        std::string dvd_dir_str = input.parent_path().string();
        std::string xbe_name   = input.filename().string();

        host_state.xbe_path = "\\Device\\CdRom0\\" + xbe_name;
        io_sys.dvd_dir = dvd_dir_str;
        io_sys.hdd_dir = hdd_dir;

        fprintf(stderr, "[nboxkrnl] DVD dir:  %s\n", dvd_dir_str.c_str());
        fprintf(stderr, "[nboxkrnl] XBE path: %s\n", host_state.xbe_path.c_str());
        fprintf(stderr, "[nboxkrnl] HDD dir:  %s\n", hdd_dir);
    }
    else if (ext == ".iso" || ext == ".xiso") {
        // XISO mode (placeholder — not yet implemented).
        host_state.xbe_path = "\\Device\\CdRom0\\default.xbe";
        io_sys.dvd_dir = ".";
        io_sys.hdd_dir = hdd_dir;

        fprintf(stderr, "[nboxkrnl] XISO mode not yet implemented\n");
        return false;
    }
    else {
        fprintf(stderr, "[nboxkrnl] unknown input type: %s\n", ext.c_str());
        return false;
    }

    return true;
}

} // namespace nboxkrnl
