#pragma once

#include <system_error>
#include <cstdint>

enum class NodeType: uint8_t {
    UNKNOWN,
    DIR,
    FILE,
    LINK
};


struct FileEvent {
    std::string path;

    NodeType node_type;

    time_t  modtime;               
    uint64_t size;

    std::error_code err;            //permission denied/broken path/
};


