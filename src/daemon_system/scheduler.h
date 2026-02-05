#pragma once
#include <config.h>
#include <request_state.h>
#include <submit_scan_result.h>
#include <metrics.h>

#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <deque>
#include <atomic>
#include <string>
#include <vector>

class Manager;

// Scheduler is called by multi thread in httpserver -> submit_scan_root / cancel / get_status 

class Scheduler {

private:
    struct PendingRoot {
        uint64_t scan_id;
        std::string root;
        PendingRoot():scan_id(0) {};
        PendingRoot(uint64_t id, std::string&& path): scan_id(id), root(std::move(path)) {};
    };

    Manager& manager;
    const size_t MAX_CONCURRENT_SCAN;
    const size_t QUEUE_MAX_SIZE;

    std::mutex mtx;
    std::condition_variable cv;
    // protect
    bool stop_flag = false;
    int running_scans = 0;       //increment: manager_accept; decrement:notify_scan_finished
    int pending_scans = 0;       //to avoid O(n) erase of pending scan
    bool manager_queue_full = false;

    std::unordered_map<uint64_t, RequestState> state_map;   //every pending_queue scan must exist in this map

    std::deque<PendingRoot> pending_queue;      
    std::atomic<uint64_t> next_scan_id{1};      // scan_id = 0: invalid id
    
    // utils
    uint64_t gen_scan_id();
    std::string normalize_path(const std::string& path);
    



