#pragma once
#include <cstdint>

struct ScanStats {
    // Entry state summary 
    uint64_t total_entries      = 0;
    uint64_t alive_entries      = 0;
    uint64_t deleted_entries    = 0;
    uint64_t error_entries      = 0;

    // Content summary 
    uint64_t directory_count    = 0;
    uint64_t file_count         = 0;
    uint64_t total_file_size    = 0;          //bytes
    uint64_t symlink_count      = 0;

};


struct IndexStats {
    time_t take_time;
    uint64_t entries_count      = 0;
    uint64_t dir_count          = 0;
    uint64_t file_count         = 0;
    uint64_t link_count         = 0;
    uint64_t total_bytes        = 0;
};
