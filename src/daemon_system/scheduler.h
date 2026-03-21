#pragma once
#include <config.h>
#include <request_state.h>
#include <submit_scan_result.h>
#include <metrics.h>

#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_set<std::string> root_map;

    std::deque<PendingRoot> pending_queue;      
    std::atomic<uint64_t> next_scan_id{1};      // scan_id = 0: invalid id
    
    // utils
    uint64_t gen_scan_id();
    std::string normalize_path(const std::string& path);
    bool check_non_overlap_path(const std::string& path);
    


public:
    Scheduler(Manager& manager_)
        :manager(manager_), 
        MAX_CONCURRENT_SCAN(!Config::cfg().scheduler().max_concurrent_scan ? 1  
                            :Config::cfg().scheduler().max_concurrent_scan),
        QUEUE_MAX_SIZE(!Config::cfg().scheduler().queue_max_size ? 1 
                       :Config::cfg().scheduler().queue_max_size) 
        {
            Metrics::measurement().const_scan_max_concurrent_number = MAX_CONCURRENT_SCAN;
            Metrics::measurement().const_scan_pending_queue_size = QUEUE_MAX_SIZE;
        }

    // daemon api
    void init_history_scan_id(uint64_t max_scan_id) { next_scan_id.store(max_scan_id+1); };
    void run();
    SubmitScanResult submit_scan_root(std::string&& root_path, uint64_t& scan_id);
    bool cancel(uint64_t scan_id);
    void shutdown();
    // RequestState get_state(uint64_t scan_id);
    std::vector<RequestState> get_state(const std::vector<uint64_t>& scan_ids);

    // manager api
    void notify_scan_finished(uint64_t scan_id);
    void notify_dispatch_available();
    void notify_export_done(const std::string& root);
    
};

