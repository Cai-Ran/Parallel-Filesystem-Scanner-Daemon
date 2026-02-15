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
    std::atomic<uint64_t> export_pending;             //add / sub //follow queue
    std::atomic<int> export_running;                  //add / sub
    std::atomic<uint64_t> export_finished;            //only add

    // async_logger
    uint64_t const_logger_pending_queue_size;
    std::atomic<uint64_t> logger_pending;             //add / sub //follow queue
    std::atomic<uint64_t> logger_finished;            //add
    std::atomic<uint64_t> logger_fallback;            //add



