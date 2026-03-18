#pragma once

#include <system_error>
#include <cstdint>

enum class NodeType: uint8_t {
    UNKNOWN,
    DIR,
    FILE,
    LINK
};


struct Sentinel {
    bool is_sentinel = false;
    bool is_canceled = false;
    Sentinel():is_sentinel(false), is_canceled(false){};
};


//order format to match padding
struct FileEvent {
    uint64_t scan_id;

    time_t   modtime;
    uint64_t size;

    std::error_code err;            //permission denied/broken path/

    std::string path;

    NodeType node_type;

    
    Sentinel end_signal;       
    //marks end of a scan; 
    //path = root_path
    //scan_id = id
    //others unused
};



enum class EventState: uint8_t{
    Alive,
    Error,
    Deleted,
    Canceled
};



