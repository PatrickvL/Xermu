#pragma once
// ---------------------------------------------------------------------------
// nboxkrnl_io.hpp — Host-side file I/O packet processing for nboxkrnl.
//
// nboxkrnl's I/O manager translates kernel file operations into IoRequest
// packets in guest RAM, submitted via OUT to port 0x206.  The host reads
// the packet, performs the actual file operation on the host filesystem,
// writes the result into an IoInfoBlock in guest memory, and marks it ready.
// The guest polls completion via OUT to port 0x208.
//
// Adapted from nxbx io.cpp — uses pass-through host filesystem I/O instead
// of full FATX metadata (timestamps fabricated, free clusters = large value).
// ---------------------------------------------------------------------------

#include "cpu/executor.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <memory>
#include <fstream>
#include <deque>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <cassert>

namespace nboxkrnl {

// ---------------------------------------------------------------------------
// Constants matching nboxkrnl/nxbx definitions.
// ---------------------------------------------------------------------------

// Disposition flags (same as NtCreate/OpenFile).
static constexpr uint32_t IO_SUPERSEDE    = 0;
static constexpr uint32_t IO_OPEN         = 1;
static constexpr uint32_t IO_CREATE       = 2;
static constexpr uint32_t IO_OPEN_IF      = 3;
static constexpr uint32_t IO_OVERWRITE    = 4;
static constexpr uint32_t IO_OVERWRITE_IF = 5;

// Extract fields from the packed type word.
static inline uint32_t io_get_type(uint32_t t)        { return t & 0xF0000000u; }
static inline uint32_t io_get_flags(uint32_t t)       { return t & 0x007FFFF8u; }
static inline uint32_t io_get_disposition(uint32_t t)  { return t & 0x00000007u; }
static inline uint32_t io_get_dev(uint32_t t)          { return (t >> 23) & 0x1Fu; }

// Request types (high nibble).
static constexpr uint32_t REQ_OPEN   = 1u << 28;
static constexpr uint32_t REQ_REMOVE = 2u << 28;
static constexpr uint32_t REQ_CLOSE  = 3u << 28;
static constexpr uint32_t REQ_READ   = 4u << 28;
static constexpr uint32_t REQ_WRITE  = 5u << 28;

// Info result codes.
enum IoInfo : uint32_t {
    INFO_NO_DATA    = 0,
    INFO_SUPERSEDED = 0,
    INFO_OPENED     = 1,
    INFO_CREATED    = 2,
    INFO_OVERWRITTEN= 3,
    INFO_EXISTS     = 4,
    INFO_NOT_EXISTS = 5,
};

// NTSTATUS codes.
static constexpr int32_t STATUS_SUCCESS              = 0;
static constexpr int32_t STATUS_IO_DEVICE_ERROR      = (int32_t)0xC0000185;
static constexpr int32_t STATUS_ACCESS_DENIED        = (int32_t)0xC0000022;
static constexpr int32_t STATUS_FILE_IS_A_DIRECTORY  = (int32_t)0xC00000BA;
static constexpr int32_t STATUS_NOT_A_DIRECTORY      = (int32_t)0xC0000103;
static constexpr int32_t STATUS_OBJECT_NAME_NOT_FOUND= (int32_t)0xC0000034;
static constexpr int32_t STATUS_OBJECT_PATH_NOT_FOUND= (int32_t)0xC000003A;

// I/O flags.
static constexpr uint32_t IO_FLAG_MUST_BE_DIR     = 1u << 4;
static constexpr uint32_t IO_FLAG_MUST_NOT_BE_DIR = 1u << 5;

// Device numbers (matching nxbx io.hpp).
static constexpr uint32_t DEV_CDROM      = 0;
static constexpr uint32_t DEV_PARTITION0 = 2;
static constexpr uint32_t DEV_PARTITION1 = 3;
static constexpr uint32_t DEV_PARTITION2 = 4;
static constexpr uint32_t DEV_PARTITION3 = 5;
static constexpr uint32_t DEV_PARTITION4 = 6;
static constexpr uint32_t DEV_PARTITION5 = 7;
static constexpr uint32_t NUM_OF_DEVS    = 10;

// Special internal handles used by the kernel.
static constexpr uint32_t FIRST_FREE_HANDLE = NUM_OF_DEVS;

// ---------------------------------------------------------------------------
// Packed packet structures (must match nboxkrnl layout exactly).
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct PackedRequestHeader {
    uint32_t id;
    uint32_t type;
};

struct PackedRequestOc {
    int64_t  initial_size;
    uint32_t size;
    uint32_t handle;
    uint32_t path;
    uint32_t attributes;
    uint32_t timestamp;
    uint32_t desired_access;
    uint32_t create_options;
};

struct PackedRequestRw {
    int64_t  offset;
    uint32_t size;
    uint32_t address;
    uint32_t handle;
    uint32_t timestamp;
};

struct PackedRequestXx {
    uint32_t handle;
};

struct PackedRequest {
    PackedRequestHeader header;
    union {
        PackedRequestOc oc;
        PackedRequestRw rw;
        PackedRequestXx xx;
    };
};

struct InfoBlock {
    uint32_t id;
    int32_t  status;
    uint32_t info;
    uint32_t ready;
};

struct InfoBlockOc {
    InfoBlock header;
    uint32_t  file_size;
    union {
        struct { uint32_t free_clusters, creation_time, last_access_time, last_write_time; } fatx;
        int64_t xdvdfs_timestamp;
    };
};

#pragma pack(pop)

static_assert(sizeof(PackedRequest) == 44);
static_assert(sizeof(InfoBlockOc)   == 36);

// ---------------------------------------------------------------------------
// Host-side request (unpacked, with buffer for read data).
// ---------------------------------------------------------------------------

struct HostRequest {
    uint32_t id;
    uint32_t type;
    union {
        int64_t  offset;
        uint32_t initial_size;
    };
    union {
        uint32_t address;   // guest VA for read/write data
        char*    path;      // heap-allocated path string for open
    };
    uint32_t size;
    uint32_t handle;
    uint32_t timestamp;
    InfoBlockOc info;

    // Open-specific fields.
    uint32_t attributes;
    uint32_t desired_access;
    uint32_t create_options;

    // Read data buffer.
    std::unique_ptr<char[]> buffer;

    ~HostRequest() {
        if (io_get_type(type) == REQ_OPEN && path) {
            delete[] path;
            path = nullptr;
        }
    }

    // Non-copyable.
    HostRequest() = default;
    HostRequest(const HostRequest&) = delete;
    HostRequest& operator=(const HostRequest&) = delete;
    HostRequest(HostRequest&&) = default;
    HostRequest& operator=(HostRequest&&) = default;
};

// ---------------------------------------------------------------------------
// File info for an opened file handle.
// ---------------------------------------------------------------------------

struct FileInfo {
    std::fstream fs;
    std::string  relative_path;
    bool         is_directory = false;
};

// ---------------------------------------------------------------------------
// IoSystem — manages the worker thread, queues, handle maps.
// ---------------------------------------------------------------------------

struct IoSystem {
    Executor* exec = nullptr;

    // DVD directory (parent of XBE file).
    std::string dvd_dir;

    // HDD base directory (e.g. "data/hdd/").
    std::string hdd_dir;

    // Per-device handle maps: device → (handle → FileInfo).
    std::map<uint32_t, std::unique_ptr<FileInfo>> handle_map[NUM_OF_DEVS];

    // Worker thread and queues.
    std::jthread worker_thread;
    std::deque<std::unique_ptr<HostRequest>> work_queue;
    std::vector<std::unique_ptr<HostRequest>> pending_vec; // retry buffer
    std::unordered_map<uint32_t, std::unique_ptr<HostRequest>> completed_map;
    std::mutex queue_mtx;
    std::mutex completed_mtx;
    std::atomic_flag pending_flag;
    bool has_pending_packets = false;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    ~IoSystem() { stop(); }

    void init(Executor* e, const std::string& dvd, const std::string& hdd) {
        exec = e;
        dvd_dir = dvd;
        hdd_dir = hdd;

        // Ensure HDD partition directories exist.
        namespace fs = std::filesystem;
        for (int i = 0; i < 6; ++i) {
            std::string dir = hdd_dir + "/Partition" + std::to_string(i);
            fs::create_directories(dir);
        }

        worker_thread = std::jthread([this](std::stop_token stok) { worker(stok); });
    }

    void stop() {
        if (worker_thread.joinable()) {
            worker_thread.request_stop();
            queue_mtx.lock();
            pending_flag.test_and_set();
            pending_flag.notify_one();
            queue_mtx.unlock();
            worker_thread.join();
        }
    }

    // -----------------------------------------------------------------------
    // Path translation — maps Xbox device paths to host paths.
    //
    // Input:  "\Device\CdRom0\subdir\file.ext"
    //         "\Device\Harddisk0\Partition2\subdir\file.ext"
    // Output: relative path under the device root, e.g. "subdir/file.ext"
    // -----------------------------------------------------------------------

    std::string parse_path(const char* xbox_path, uint32_t len, uint32_t dev) {
        std::string_view path(xbox_path, len);

        // Skip "\Device\<devname>\"
        size_t dev_pos = path.find_first_of('\\', 1);
        if (dev_pos == std::string_view::npos) return "";
        size_t pos = path.find_first_of('\\', dev_pos + 1);
        if (pos == std::string_view::npos) pos = path.length();

        if (dev != DEV_CDROM) {
            // Skip partition name too: "\Device\Harddisk0\Partition2\..."
            size_t pos2 = path.find_first_of('\\', pos + 1);
            if (pos2 == std::string_view::npos) pos2 = path.length();
            pos = pos2;
        }

        std::string name;
        if (pos + 1 < path.length()) {
            name = std::string(path.substr(pos + 1));
        }
        // Convert backslashes to forward slashes.
        std::replace(name.begin(), name.end(), '\\', '/');
        return name;
    }

    // Resolve a relative path to a full host path for a device.
    std::string resolve_host_path(uint32_t dev, const std::string& relative) {
        namespace fs = std::filesystem;
        if (dev == DEV_CDROM) {
            return (fs::path(dvd_dir) / relative).string();
        }
        // HDD partition.
        uint32_t part = dev - DEV_PARTITION0;
        std::string part_dir = hdd_dir + "/Partition" + std::to_string(part);
        return (fs::path(part_dir) / relative).string();
    }

    // Case-insensitive file search: given a host directory and a relative path,
    // find the actual file (Xbox paths are case-insensitive).
    bool find_file_ci(const std::string& base_dir, const std::string& relative,
                      std::string& resolved_out) {
        namespace fs = std::filesystem;

        // Split relative path into components.
        fs::path rel(relative);
        fs::path current = base_dir;

        for (auto it = rel.begin(); it != rel.end(); ++it) {
            std::string component = it->string();
            if (component.empty() || component == "." || component == "/") continue;

            bool found = false;
            std::error_code ec;
            for (auto& entry : fs::directory_iterator(current, ec)) {
                std::string entry_name = entry.path().filename().string();
                if (entry_name.size() == component.size()) {
                    bool match = true;
                    for (size_t i = 0; i < entry_name.size(); ++i) {
                        if (tolower((unsigned char)entry_name[i]) !=
                            tolower((unsigned char)component[i])) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        current = entry.path();
                        found = true;
                        break;
                    }
                }
            }
            if (!found) return false;
        }

        resolved_out = current.string();
        return true;
    }

    // -----------------------------------------------------------------------
    // Submit / flush / query — called from I/O port handlers.
    // -----------------------------------------------------------------------

    void submit_io_packet(uint32_t guest_va) {
        PackedRequest pkt;
        exec->read_guest_block(guest_va, sizeof(pkt), &pkt);

        uint32_t io_type = io_get_type(pkt.header.type);

        if (io_type == REQ_OPEN) {
            auto req = std::make_unique<HostRequest>();
            req->id        = pkt.header.id;
            req->type      = pkt.header.type;
            req->initial_size = static_cast<uint32_t>(pkt.oc.initial_size);
            req->size      = pkt.oc.size;
            req->handle    = pkt.oc.handle;
            req->timestamp = pkt.oc.timestamp;
            req->attributes    = pkt.oc.attributes;
            req->desired_access = pkt.oc.desired_access;
            req->create_options = pkt.oc.create_options;

            // Read path string from guest.
            req->path = new char[pkt.oc.size + 1];
            exec->read_guest_block(pkt.oc.path, pkt.oc.size,
                                   reinterpret_cast<uint8_t*>(req->path));
            req->path[pkt.oc.size] = '\0';

            enqueue(std::move(req));
        }
        else if (io_type == REQ_READ || io_type == REQ_WRITE) {
            auto req = std::make_unique<HostRequest>();
            req->id        = pkt.header.id;
            req->type      = pkt.header.type;
            req->offset    = pkt.rw.offset;
            req->address   = pkt.rw.address;
            req->size      = pkt.rw.size;
            req->handle    = pkt.rw.handle;
            req->timestamp = pkt.rw.timestamp;
            req->buffer    = std::make_unique_for_overwrite<char[]>(pkt.rw.size);

            if (io_type == REQ_WRITE) {
                // Copy write data from guest now.
                exec->read_guest_block(pkt.rw.address, pkt.rw.size,
                                       reinterpret_cast<uint8_t*>(req->buffer.get()));
            }

            enqueue(std::move(req));
        }
        else {
            // close / remove
            auto req = std::make_unique<HostRequest>();
            req->id     = pkt.header.id;
            req->type   = pkt.header.type;
            req->handle = pkt.xx.handle;

            enqueue(std::move(req));
        }
    }

    void flush_pending_packets() {
        if (!pending_vec.empty()) {
            if (queue_mtx.try_lock()) {
                for (auto& p : pending_vec) {
                    work_queue.push_back(std::move(p));
                }
                pending_vec.clear();
                has_pending_packets = false;
                pending_flag.test_and_set();
                pending_flag.notify_one();
                queue_mtx.unlock();
            }
        }
    }

    void query_io_packet(uint32_t guest_va) {
        if (completed_mtx.try_lock()) {
            InfoBlockOc block;
            exec->read_guest_block(guest_va, sizeof(InfoBlockOc),
                                   reinterpret_cast<uint8_t*>(&block));

            auto it = completed_map.find(block.header.id);
            if (it != completed_map.end()) {
                HostRequest* req = it->second.get();

                // For read operations, transfer data to guest now.
                if (io_get_type(req->type) == REQ_READ &&
                    req->info.header.status == STATUS_SUCCESS) {
                    auto* rw_req = req;
                    exec->write_guest_block(rw_req->address, rw_req->size,
                                            rw_req->buffer.get());
                }

                // Write result back to guest.
                uint32_t result_size;
                if (io_get_type(req->type) == REQ_OPEN) {
                    block = req->info;
                    result_size = sizeof(InfoBlockOc);
                } else {
                    block.header = req->info.header;
                    result_size = sizeof(InfoBlock);
                }
                block.header.ready = 1;
                exec->write_guest_block(guest_va, result_size,
                                        reinterpret_cast<uint8_t*>(&block));

                completed_map.erase(it);
            }
            completed_mtx.unlock();
        }
    }

private:
    void enqueue(std::unique_ptr<HostRequest> req) {
        if (queue_mtx.try_lock()) {
            work_queue.push_back(std::move(req));
            pending_flag.test_and_set();
            pending_flag.notify_one();
            queue_mtx.unlock();
        } else {
            pending_vec.push_back(std::move(req));
            has_pending_packets = true;
        }
    }

    // -----------------------------------------------------------------------
    // Worker thread — processes I/O requests.
    // -----------------------------------------------------------------------

    void worker(std::stop_token stok) {
        while (true) {
            pending_flag.wait(false);

            if (stok.stop_requested()) {
                // Clean up.
                has_pending_packets = false;
                work_queue.clear();
                completed_map.clear();
                for (auto& hm : handle_map) hm.clear();
                pending_vec.clear();
                return;
            }

            queue_mtx.lock();
            if (work_queue.empty()) {
                pending_flag.clear();
                queue_mtx.unlock();
                continue;
            }
            auto req = std::move(work_queue.front());
            work_queue.pop_front();
            queue_mtx.unlock();

            uint32_t io_type = io_get_type(req->type);
            uint32_t dev     = io_get_dev(req->type);

            if (io_type == REQ_OPEN) {
                process_open(req.get(), dev);
            } else {
                process_other(req.get(), io_type, dev);
            }

            completed_mtx.lock();
            completed_map.emplace(req->id, std::move(req));
            completed_mtx.unlock();
        }
    }

    // -----------------------------------------------------------------------
    // Open/create processing.
    // -----------------------------------------------------------------------

    void process_open(HostRequest* req, uint32_t dev) {
        memset(&req->info, 0, sizeof(req->info));
        req->info.header.status = STATUS_IO_DEVICE_ERROR;

        uint32_t disposition = io_get_disposition(req->type);
        uint32_t flags       = io_get_flags(req->type);
        std::string relative = parse_path(req->path, req->size, dev);

        std::string base_dir;
        if (dev == DEV_CDROM) {
            base_dir = dvd_dir;
        } else {
            uint32_t part = dev - DEV_PARTITION0;
            base_dir = hdd_dir + "/Partition" + std::to_string(part);
        }

        // Case-insensitive file search.
        std::string resolved;
        bool exists = find_file_ci(base_dir, relative, resolved);

        if (exists) {
            namespace fs = std::filesystem;
            std::error_code ec;
            bool is_dir = fs::is_directory(resolved, ec);

            req->info.header.info = INFO_EXISTS;

            if ((flags & IO_FLAG_MUST_BE_DIR) && !is_dir) {
                req->info.header.status = STATUS_NOT_A_DIRECTORY;
            }
            else if ((flags & IO_FLAG_MUST_NOT_BE_DIR) && is_dir) {
                req->info.header.status = STATUS_FILE_IS_A_DIRECTORY;
            }
            else if (disposition == IO_OPEN || disposition == IO_OPEN_IF) {
                req->info.header.status = STATUS_SUCCESS;
                req->info.header.info   = INFO_OPENED;

                uint64_t fsize = 0;
                if (!is_dir) {
                    fsize = fs::file_size(resolved, ec);
                }
                req->info.file_size = static_cast<uint32_t>(fsize);

                // Fabricate timestamps.
                req->info.fatx.creation_time     = req->timestamp;
                req->info.fatx.last_access_time  = req->timestamp;
                req->info.fatx.last_write_time   = req->timestamp;
                req->info.fatx.free_clusters     = 0x7FFFF; // large value

                // Create file info and store in handle map.
                auto fi = std::make_unique<FileInfo>();
                fi->relative_path = relative;
                fi->is_directory  = is_dir;
                if (!is_dir) {
                    fi->fs.open(resolved, std::ios::in | std::ios::out | std::ios::binary);
                    if (!fi->fs.is_open()) {
                        // Try read-only (e.g. DVD files).
                        fi->fs.open(resolved, std::ios::in | std::ios::binary);
                    }
                }
                handle_map[dev][req->handle] = std::move(fi);

                fprintf(stderr, "[io] opened %s handle=0x%08X path=%s\n",
                        is_dir ? "dir" : "file", req->handle, relative.c_str());
            }
            else if (disposition == IO_CREATE) {
                req->info.header.status = STATUS_ACCESS_DENIED;
                req->info.header.info   = INFO_EXISTS;
            }
            else {
                // SUPERSEDE / OVERWRITE / OVERWRITE_IF — truncate and rewrite.
                req->info.header.status = STATUS_SUCCESS;
                req->info.header.info   = (disposition == IO_SUPERSEDE)
                                           ? INFO_SUPERSEDED : INFO_OVERWRITTEN;

                auto fi = std::make_unique<FileInfo>();
                fi->relative_path = relative;
                fi->is_directory  = is_dir;
                if (!is_dir) {
                    fi->fs.open(resolved, std::ios::in | std::ios::out |
                                std::ios::binary | std::ios::trunc);
                }
                handle_map[dev][req->handle] = std::move(fi);
                req->info.file_size = 0;
                req->info.fatx.creation_time    = req->timestamp;
                req->info.fatx.last_access_time = req->timestamp;
                req->info.fatx.last_write_time  = req->timestamp;
                req->info.fatx.free_clusters    = 0x7FFFF;
            }
        }
        else {
            // File doesn't exist.
            req->info.header.info = INFO_NOT_EXISTS;

            if (disposition == IO_CREATE || disposition == IO_SUPERSEDE ||
                disposition == IO_OPEN_IF || disposition == IO_OVERWRITE_IF) {
                // Create the file/directory.
                namespace fs = std::filesystem;
                std::string full_path = (fs::path(base_dir) / relative).string();

                bool is_dir = (req->attributes & 0x10) != 0; // IO_FILE_DIRECTORY
                bool created = false;

                if (is_dir) {
                    std::error_code ec;
                    created = fs::create_directories(full_path, ec);
                } else {
                    // Ensure parent directory exists.
                    fs::path parent = fs::path(full_path).parent_path();
                    std::error_code ec;
                    fs::create_directories(parent, ec);

                    std::ofstream ofs(full_path, std::ios::binary);
                    created = ofs.good();
                    ofs.close();
                }

                if (created || std::filesystem::exists(full_path)) {
                    req->info.header.status = STATUS_SUCCESS;
                    req->info.header.info   = INFO_CREATED;
                    req->info.file_size     = 0;
                    req->info.fatx.creation_time    = req->timestamp;
                    req->info.fatx.last_access_time = req->timestamp;
                    req->info.fatx.last_write_time  = req->timestamp;
                    req->info.fatx.free_clusters    = 0x7FFFF;

                    auto fi = std::make_unique<FileInfo>();
                    fi->relative_path = relative;
                    fi->is_directory  = is_dir;
                    if (!is_dir) {
                        fi->fs.open(full_path, std::ios::in | std::ios::out | std::ios::binary);
                    }
                    handle_map[dev][req->handle] = std::move(fi);

                    fprintf(stderr, "[io] created %s handle=0x%08X path=%s\n",
                            is_dir ? "dir" : "file", req->handle, relative.c_str());
                }
            }
            else {
                // IO_OPEN or IO_OVERWRITE — file must exist.
                req->info.header.status = STATUS_OBJECT_NAME_NOT_FOUND;
                req->info.header.info   = INFO_NOT_EXISTS;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Read / write / close / remove processing.
    // -----------------------------------------------------------------------

    void process_other(HostRequest* req, uint32_t io_type, uint32_t dev) {
        memset(&req->info.header, 0, sizeof(InfoBlock));
        req->info.header.status = STATUS_IO_DEVICE_ERROR;

        auto it = handle_map[dev].find(req->handle);
        if (it == handle_map[dev].end()) {
            // Raw device handles (< FIRST_FREE_HANDLE) have no opened file.
            if (req->handle < FIRST_FREE_HANDLE) {
                if (io_type == REQ_READ) {
                    memset(req->buffer.get(), 0, req->size);
                    // Synthesize FATX superblock for partitions 1-5 at offset 0.
                    if (dev >= DEV_PARTITION1 && dev <= DEV_PARTITION5 &&
                        req->offset == 0 && req->size >= 16) {
                        auto* sb = reinterpret_cast<uint32_t*>(req->buffer.get());
                        sb[0] = 0x58544146;  // 'XTAF' (FATX signature, LE)
                        sb[1] = 0x12345678 + dev;  // VolumeID
                        sb[2] = 32;          // ClusterSize (32 sectors = 16KB)
                        sb[3] = 1;           // RootDirCluster
                    }
                    req->info.header.status = STATUS_SUCCESS;
                    req->info.header.info   = req->size;
                    return;
                }
                if (io_type == REQ_WRITE) {
                    // Discard writes to raw partition handles.
                    req->info.header.status = STATUS_SUCCESS;
                    req->info.header.info   = req->size;
                    return;
                }
                if (io_type == REQ_CLOSE) {
                    req->info.header.status = STATUS_SUCCESS;
                    return;
                }
            }
            fprintf(stderr, "[io] handle 0x%08X not found (dev=%u type=0x%08X)\n",
                    req->handle, dev, io_type);
            return;
        }

        std::fstream* fs = &it->second->fs;

        switch (io_type) {
        case REQ_CLOSE:
            fprintf(stderr, "[io] closed handle=0x%08X path=%s\n",
                    req->handle, it->second->relative_path.c_str());
            handle_map[dev].erase(it);
            req->info.header.status = STATUS_SUCCESS;
            break;

        case REQ_READ: {
            req->info.header.status = STATUS_IO_DEVICE_ERROR;
            req->info.header.info   = INFO_NO_DATA;

            if (!fs->is_open()) {
                fprintf(stderr, "[io] read on directory handle 0x%08X\n", req->handle);
                break;
            }
            fs->seekg(req->offset);
            fs->read(req->buffer.get(), req->size);
            if (fs->good() || fs->eof()) {
                req->info.header.status = STATUS_SUCCESS;
                req->info.header.info   = static_cast<uint32_t>(fs->gcount());
                fprintf(stderr, "[io] read handle=0x%08X off=%lld req_size=%u got=%u addr=0x%08X\n",
                        req->handle, (long long)req->offset, req->size,
                        (unsigned)fs->gcount(), req->address);
                // Actual size to transfer back may be less than requested.
                req->size = static_cast<uint32_t>(fs->gcount());
            } else {
                fprintf(stderr, "[io] read failed handle=0x%08X offset=%lld size=%u\n",
                        req->handle, (long long)req->offset, req->size);
            }
            fs->clear();
            break;
        }

        case REQ_WRITE: {
            req->info.header.status = STATUS_IO_DEVICE_ERROR;
            req->info.header.info   = INFO_NO_DATA;

            if (!fs->is_open()) {
                fprintf(stderr, "[io] write on directory handle 0x%08X\n", req->handle);
                break;
            }
            fs->seekp(req->offset);
            fs->write(req->buffer.get(), req->size);
            if (fs->good()) {
                req->info.header.status = STATUS_SUCCESS;
                req->info.header.info   = req->size;
            } else {
                fprintf(stderr, "[io] write failed handle=0x%08X offset=%lld size=%u\n",
                        req->handle, (long long)req->offset, req->size);
                fs->clear();
            }
            break;
        }

        case REQ_REMOVE:
            fprintf(stderr, "[io] remove handle=0x%08X path=%s (stub)\n",
                    req->handle, it->second->relative_path.c_str());
            req->info.header.status = STATUS_SUCCESS;
            break;

        default:
            fprintf(stderr, "[io] unknown request type 0x%08X\n", io_type);
            break;
        }
    }
};

} // namespace nboxkrnl
