#pragma once

#include <multi_scanner.h>
#include <export_manager.h>
#include <scan_types.h>
#include <job_queue.h>
#include <thread_pool.h>
#include <config.h>

#include <functional>
#include <ctime>
#include <thread>

/*
// Scan life cycle?
// start_new_scan()
//   -> task_on_job_submit()    [add to registry, submit to scan_pool]
//   -> scanner.worker_job()    [worker threads process]
//   -> task_on_job_finish()    [unfinished_jobs all done == 0]
//   -> transfer_result()       [transfer results to ExportMnager, registry erase]
//   -> export_manager.push_queue()
*/

class Scheduler;

class Manager {
private:
    
    MultiScanner scanner;           //worker logic; not worker instance
    ExportManager export_manager;
    ThreadPool<ScanData> scan_pool;

    //scheduler callback
    std::function<void(uint64_t scan_id)> notify_scan_finished;
    std::function<void()> notify_dispatch_available;
    std::atomic<bool> dispatch_failed{false};

    std::unordered_map<std::uint64_t, ScanTask> registry;
    std::mutex registry_mtx;
    std::condition_variable manager_cv;



public:
    Manager();

    // scheduler api
    bool start_new_scan(uint64_t scan_id, std::string root_path, bool& queue_full);
    bool cancel_scan(uint64_t scan_id);
    void shutdown();
    void wait_to_finish(std::uint64_t scan_id);  
                        // needed for: graceful cancel/shutdown OR export

    // daemon api
    void start();
    void set_scheduler_callback(std::function<void(uint64_t scan_id)> fn1, 
                                std::function<void()> fn2,
                                std::function<void(const std::string& root)> fn3);    
    bool clean_up(uint64_t scan_id);

 
    // bool check_exported(uint64_t scan_id);
    std::vector<bool> check_exported(const std::vector<uint64_t>& scan_ids);

    // scanner api
    JobQueue<ScanData>::SubmitResult task_on_job_submit(ScanData& data, bool is_root);
    void task_on_job_finish(uint64_t scan_id, std::shared_ptr<ScanContext> ctx);
    void record_result(FileEvent&& event);

};


