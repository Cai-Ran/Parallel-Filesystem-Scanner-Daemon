#pragma once
#include "file_event.h"
#include "request_state.h"


struct IndexRow {
    time_t   modtime;
    uint64_t size;

    std::string err;            //permission denied/broken path/
    std::string path;

    NodeType node_type;
    EventState state;
};

enum ScanState: uint8_t{
    // align to EventState
    New,
    Error,
    Deleted,
    Canceled,
    // addition state
    Changed
};

struct ScanRow {
    time_t   modtime;
    uint64_t size;

    std::string err;            //permission denied/broken path/
    std::string path;

    NodeType node_type;
    ScanState state;
};

struct ScanTaskRow {
    uint64_t scan_id = 0;
    uint64_t start_time = 0;
    uint64_t finish_time = 0;
    std::string submit_root;
    RequestState end_state;
};

struct DeleteTask {
    uint64_t scan_id = 0;
    std::string submit_root;
    bool canceled = false;
    DeleteTask(){};
    DeleteTask(uint64_t id, const std::string& root, bool cancel_) 
    : scan_id(id), submit_root(root), canceled(cancel_) {};

};
