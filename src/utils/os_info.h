#pragma once
#include <sys/statvfs.h>
#include <cstdint>
#include <string>
#include <fstream>
#include <iterator>

struct DiskInfo {
    uint64_t capacity_bytes     = 0;
    uint64_t free_bytes         = 0;
    uint64_t available_bytes    = 0;
};

namespace os_info {

    inline bool is_wsl() {
        std::ifstream f("/proc/version");
        std::string s((std::istreambuf_iterator<char>(f)), {});
        return s.find("microsoft") != std::string::npos ||
               s.find("Microsoft") != std::string::npos;
    }


    inline bool get_disk_info_linux(const std::string& path, DiskInfo& info) {

        const char* effective_path = is_wsl() ? "/mnt/c" : path.c_str();

        struct statvfs st {};

        // Query filesystem statistics for the filesystem containing `path`.
        if (statvfs(effective_path, &st) != 0) return false;

        // File system fragment size (fundamental block size in bytes)
        uint64_t block_size = static_cast<uint64_t>(st.f_frsize);

        info.capacity_bytes  = block_size * static_cast<uint64_t>(st.f_blocks);
        info.free_bytes      = block_size * static_cast<uint64_t>(st.f_bfree);
        info.available_bytes = block_size * static_cast<uint64_t>(st.f_bavail);

        return true;
    };
};