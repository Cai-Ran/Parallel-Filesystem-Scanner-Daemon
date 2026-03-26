#pragma once

#include <ctime>
#include <cstdint>
#include <string>
#include <queue>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include "job_queue.h"
#include "db_types.h"



class ExportManager {

private:

    std::string db_path;
    uint16_t WRITE_BATCH_SIZE;

    JobQueue<FileEvent>     result_queue;
    JobQueue<DeleteTask>    delete_queue;

    std::thread result_thread;
    std::thread delete_thread;

    bool stop_flag = false;

    void write_result_to_db();
    // export manager: result_thread push, delete_thread consume
    void mark_deleted_in_db();

    std::unordered_set<uint64_t> exported_map;
    std::mutex map_mtx;

    std::function<void(const std::string&)> notify_export_done;


public:
    ExportManager();
    ~ExportManager() {}

    // manager api
    bool start();
    void shutdown();


    void insert_scan_task(ScanTaskRow&& data);
    void update_scan_finish(uint64_t scan_id, time_t finish_time);
    void push_result_queue(FileEvent&& event);
      
    
    // bool check_exported(uint64_t scan_id);
    std::vector<bool> check_exported(const std::vector<uint64_t>& scan_ids);
    void set_scheduler_callback(std::function<void(const std::string&)> fn);
};