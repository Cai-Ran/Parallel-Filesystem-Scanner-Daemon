#pragma once

#include <atomic>

// struct instead of class: member all public
struct Metrics {
        
    // scheduler
    uint64_t const_scan_max_concurrent_number;
    uint64_t const_scan_pending_queue_size;
        // state map already recorded PENDING / RUNNING scans
        // for pending queue max size adjustment
    std::atomic<int> scan_running;                  //add / sub
    std::atomic<uint64_t> scan_pending;             //follow queue (uint64_t)

    // manager
    uint64_t const_scan_job_pool_num_threads;
    uint64_t const_scan_job_queue_size;
    std::atomic<int> scan_jobs_unfinished;
        //for queue max size adjustment
    std::atomic<uint64_t> scan_jobs_submitted;     //only add
    std::atomic<uint64_t> scan_jobs_enqueue_reject;//only add
    std::atomic<uint64_t> scan_jobs_queued;        //add / sub //follow queue 
        
    // httpserver
    uint64_t const_request_pool_num_threads;
    uint64_t const_request_queue_size;
        //for queue max size adjustment
    std::atomic<uint64_t> request_jobs_submitted;     //only add
    std::atomic<uint64_t> request_jobs_failed;        //only add
    std::atomic<uint64_t> request_jobs_queued;        //add / sub //follow queue

    // export_manager
    uint64_t const_result_que_size;
    uint64_t const_delete_que_size;
    std::atomic<uint64_t>   export_result_pending;             //add / sub //follow queue
    std::atomic<int>        export_result_running;                  //add / sub
    std::atomic<uint64_t>   export_result_finished;            //only add
    std::atomic<uint64_t>   export_delete_pending;             //add / sub //follow queue
    std::atomic<int>        export_delete_running;                  //add / sub
    std::atomic<uint64_t>   export_delete_finished;            //only add

    // async_logger
    uint64_t const_logger_pending_queue_size;
    std::atomic<uint64_t> logger_pending;             //add / sub //follow queue
    std::atomic<uint64_t> logger_finished;            //add
    std::atomic<uint64_t> logger_fallback;            //add


    void reset() {

        const_scan_max_concurrent_number = 0;
        const_scan_pending_queue_size = 0;
        scan_running.store(0);
        scan_pending.store(0);

        const_scan_job_pool_num_threads = 0;
        const_scan_job_queue_size = 0;
        scan_jobs_unfinished.store(0);
        scan_jobs_submitted.store(0);
        scan_jobs_enqueue_reject.store(0);     
        scan_jobs_queued.store(0);        
            
        const_request_pool_num_threads = 0;
        const_request_queue_size = 0;
        request_jobs_submitted.store(0);     
        request_jobs_failed.store(0);    
        request_jobs_queued.store(0);       

        // export_manager
        export_result_pending.store(0);
        export_result_running.store(0);
        export_result_finished.store(0);
        export_delete_pending.store(0);
        export_delete_running.store(0);
        export_delete_finished.store(0);

        // async_logger
        const_logger_pending_queue_size = 0;
        logger_pending.store(0);
        logger_finished.store(0);
        logger_fallback.store(0);

    };


    static Metrics& measurement() {
        static Metrics metric;
        return metric;
    }
};

